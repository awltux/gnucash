// GnuCash Qt 6 shell.
//
// Two modes:
//   gnucash-qt <book>                                   open a book, stay open
//   gnucash-qt --self-test <backend> <book> <report>    headless harness (CI)
//
// The GUI thread owns the engine (ADR 0012), so the GLib context is iterated
// here and engine events arrive on this thread; nothing is marshalled.
//
// Reports come from a child process through ReportRunner (ADR 0015): gnc-report
// links GTK3, so rendering must not happen in this process.

#include <atomic>
#include <cstdio>

#include <dlfcn.h>

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QShortcut>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QVector>
#include <QWebEngineView>

#include "account-model.h"
#include "report-runner.h"

extern "C" {
#include <cashobjects.h>
#include <gnc-date.h>
#include <gnc-engine.h>
#include <gnc-gsettings.h>
#include <gnc-prefs.h>
#include <qof.h>
#include <qofbook.h>
#include <qofevent.h>
#include <qofsession.h>
#include <Account.h>
#include <Split.h>
#include <Transaction.h>
}

namespace
{

using InitFn = void (*)();

constexpr int kRenames = 20;

std::atomic<int> g_event_count{0};
std::atomic<bool> g_event_off_thread{false};
QVector<QByteArray> g_observed;
gncqt::AccountModel* g_model = nullptr;
bool g_self_test = false;

bool load_backend(const QString& path)
{
    void* handle = dlopen(path.toUtf8().constData(), RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr)
    {
        std::fprintf(stderr, "dlopen failed for %s: %s\n", qPrintable(path), dlerror());
        return false;
    }

    InitFn init = reinterpret_cast<InitFn>(dlsym(handle, "qof_backend_module_init"));
    if (init == nullptr)
        init = reinterpret_cast<InitFn>(dlsym(handle, "gnc_module_init_backend_xml"));
    if (init == nullptr)
    {
        std::fprintf(stderr, "no backend init symbol in %s\n", qPrintable(path));
        return false;
    }

    init();
    return true;
}

QString default_backend_path()
{
    const QFileInfo application(QCoreApplication::applicationDirPath());
    const QString build_dir = application.dir().absolutePath();
    return build_dir + QStringLiteral("/lib/gnucash/libgncmod-backend-xml.so");
}

// Events arrive on the thread that made the change, which per ADR 0012 is the
// GUI thread. The model is updated directly; nothing is queued.
void on_qof_event(QofInstance* entity, QofEventId type, gpointer, gpointer)
{
    if (entity == nullptr)
        return;

    // Events arrive for every object type; only accounts belong in this model,
    // and casting anything else trips the engine's own type assertions.
    if (!GNC_IS_ACCOUNT(entity))
        return;

    if (QThread::currentThread() != QCoreApplication::instance()->thread())
        g_event_off_thread.store(true);

    auto* account = reinterpret_cast<Account*>(entity);

    if (g_model != nullptr)
    {
        if (type & QOF_EVENT_ADD)
            g_model->account_added(account);
        else if (type & QOF_EVENT_REMOVE)
            g_model->account_removed(account);
        else if (type & QOF_EVENT_MODIFY)
            g_model->account_changed(account);
    }

    if (g_self_test && (type & QOF_EVENT_MODIFY))
    {
        const char* name = xaccAccountGetName(account);
        g_observed.append(QByteArray(name ? name : ""));
        g_event_count.fetch_add(1);
    }
}

// Adds and removes an account and checks the model followed: one row inserted,
// one removed, one signal of each kind. A rename never exercises this path.
struct EditTest
{
    int baseline = -1;
    int after_add = -1;
    int after_remove = -1;
    int inserts = 0;
    int removes = 0;
    bool ok = false;
};

EditTest run_model_edit_test(gncqt::AccountModel* model, QofBook* book,
                             Account* root)
{
    EditTest result;
    if (model == nullptr || book == nullptr || root == nullptr)
        return result;

    result.baseline = model->rowCount();

    Account* fresh = xaccMallocAccount(book);
    if (fresh == nullptr)
        return result;
    xaccAccountSetName(fresh, "Model Test Account");

    gnc_account_append_child(root, fresh);
    result.after_add = model->rowCount();
    result.inserts = model->inserted_rows();

    gnc_account_remove_child(root, fresh);
    result.after_remove = model->rowCount();
    result.removes = model->removed_rows();

    result.ok = result.after_add == result.baseline + 1
                && result.after_remove == result.baseline && result.inserts == 1
                && result.removes == 1;
    return result;
}

// The first account in the tree that can hold splits (has a commodity); the
// fixture's top-level accounts are placeholders and have none.
static Account* first_account_with_commodity(Account* parent, Account* skip)
{
    GList* children = gnc_account_get_children(parent);
    Account* found = nullptr;

    for (GList* node = children; node != nullptr && found == nullptr; node = node->next)
    {
        auto* candidate = static_cast<Account*>(node->data);
        if (candidate != skip && xaccAccountGetCommodity(candidate) != nullptr)
            found = candidate;
    }

    for (GList* node = children; node != nullptr && found == nullptr; node = node->next)
        found = first_account_with_commodity(static_cast<Account*>(node->data), skip);

    g_list_free(children);
    return found;
}

static QModelIndex find_account_index(const gncqt::AccountModel* model,
                                      const QModelIndex& parent,
                                      Account* account, int column)
{
    const int rows = model->rowCount(parent);
    for (int row = 0; row < rows; ++row)
    {
        const QModelIndex candidate =
            model->index(row, gncqt::AccountModel::NameColumn, parent);
        if (model->account_for(candidate) == account)
            return model->index(row, column, parent);

        const QModelIndex deeper = find_account_index(model, candidate, account, column);
        if (deeper.isValid())
            return deeper;
    }
    return {};
}

// Gives a fresh account a commodity and a place in the tree, so the test does
// not depend on how the fixture happens to be shaped.
static Account* make_holding_account(QofBook* book, Account* parent,
                                     const char* name, gnc_commodity* commodity)
{
    Account* account = xaccMallocAccount(book);
    if (account == nullptr)
        return nullptr;

    xaccAccountSetName(account, name);
    xaccAccountSetType(account, ACCT_TYPE_BANK);
    xaccAccountSetCommodity(account, commodity);
    gnc_account_append_child(parent, account);

    return account;
}

// Posts a split into a child account and checks the total columns followed:
// the child's totals must move, and so must its parent's, because a total
// includes its children. The parent holds no splits of its own, so its total
// can only move by recursion. A second split dated ten years ago separates the
// two columns: it moves the present balance and must leave the period total
// alone.
struct TotalTest
{
    QString before_leaf;
    QString after_leaf;
    QString before_parent;
    QString after_parent;
    QString after_leaf_period;
    QString later_present;
    QString later_period;
    bool ok = false;
    bool period_ok = false;
};

TotalTest run_total_test(gncqt::AccountModel* model, QofBook* book, Account* root)
{
    TotalTest result;
    if (model == nullptr || book == nullptr || root == nullptr)
        return result;

    Account* donor = first_account_with_commodity(root, nullptr);
    if (donor == nullptr)
        return result;

    gnc_commodity* commodity = xaccAccountGetCommodity(donor);
    Account* top = make_holding_account(book, root, "Total Test Parent", commodity);
    Account* leaf = make_holding_account(book, top, "Total Test Child", commodity);
    if (top == nullptr || leaf == nullptr)
        return result;

    const QModelIndex leaf_total =
        find_account_index(model, QModelIndex(), leaf, gncqt::AccountModel::TotalColumn);
    const QModelIndex parent_total =
        find_account_index(model, QModelIndex(), top, gncqt::AccountModel::TotalColumn);
    const QModelIndex leaf_period =
        find_account_index(model, QModelIndex(), leaf,
                           gncqt::AccountModel::PeriodTotalColumn);
    if (!leaf_total.isValid() || !parent_total.isValid() || !leaf_period.isValid())
        return result;

    result.before_leaf = model->data(leaf_total, Qt::DisplayRole).toString();
    result.before_parent = model->data(parent_total, Qt::DisplayRole).toString();

    const auto post_split = [&](time64 date, gnc_numeric amount) {
        Transaction* trans = xaccMallocTransaction(book);
        xaccTransBeginEdit(trans);
        xaccTransSetCurrency(trans, commodity);
        xaccTransSetDatePostedSecsNormalized(trans, date);

        Split* debit = xaccMallocSplit(book);
        xaccSplitSetAccount(debit, leaf);
        xaccSplitSetParent(debit, trans);
        xaccSplitSetAmount(debit, amount);
        xaccSplitSetValue(debit, amount);

        Split* credit = xaccMallocSplit(book);
        xaccSplitSetAccount(credit, donor);
        xaccSplitSetParent(credit, trans);
        xaccSplitSetAmount(credit, gnc_numeric_neg(amount));
        xaccSplitSetValue(credit, gnc_numeric_neg(amount));

        xaccTransCommitEdit(trans);
    };

    post_split(gnc_time64_get_today_start(), gnc_numeric_create(100, 1));

    // The indexes stay valid: nothing was inserted or removed, and the model
    // dropped its totals when the engine reported the split.
    result.after_leaf = model->data(leaf_total, Qt::DisplayRole).toString();
    result.after_parent = model->data(parent_total, Qt::DisplayRole).toString();
    result.after_leaf_period = model->data(leaf_period, Qt::DisplayRole).toString();

    result.ok = !result.after_leaf.isEmpty()
                && result.after_leaf != result.before_leaf
                && result.after_parent != result.before_parent
                // The parent holds no splits of its own, so its recursive
                // total must equal its only child's, not merely change.
                && result.after_parent == result.after_leaf;

    // Ten years before the fiscal period: the balance moves, the period does not.
    const time64 ten_years_ago =
        gnc_time64_get_today_start() - static_cast<time64>(10 * 365 * 24 * 60 * 60);
    post_split(ten_years_ago, gnc_numeric_create(50, 1));

    result.later_present = model->data(leaf_total, Qt::DisplayRole).toString();
    result.later_period = model->data(leaf_period, Qt::DisplayRole).toString();

    result.period_ok = !result.after_leaf_period.isEmpty()
                       // The account is fresh, so its only in-period split is
                       // the one above: both columns read the same number.
                       && result.after_leaf_period == result.after_leaf
                       // The out-of-period split moved the balance...
                       && result.later_present != result.after_leaf
                       // ...and left the period total alone.
                       && result.later_period == result.after_leaf_period;
    return result;
}

// The tree's colouring follows a user preference, so the model is told the
// current value whenever it changes; the model itself stays policy-free.
struct PrefWatch
{
    gncqt::AccountModel* model = nullptr;
    QTreeView* tree = nullptr;
};

void apply_negative_in_red(PrefWatch* watch)
{
    const bool enabled =
        gnc_prefs_get_bool(GNC_PREFS_GROUP_GENERAL, GNC_PREF_NEGATIVE_IN_RED) != FALSE;

    if (watch->model != nullptr)
        watch->model->set_negative_in_red(enabled);
    if (watch->tree != nullptr)
        watch->tree->viewport()->update();
}

// Delivered on the thread that changed the preference, which is this one
// (ADR 0012).
void on_negative_in_red_changed(gpointer, gchar*, gpointer user_data)
{
    apply_negative_in_red(static_cast<PrefWatch*>(user_data));
}

// A negative total, and the preference that decides whether it is red. The
// check that matters is the read-back: a preference nothing loaded a backend
// for reads as FALSE and would look like it worked.
bool run_prefs_test(gncqt::AccountModel* model, QofBook* book, Account* root,
                    PrefWatch* watch)
{
    if (model == nullptr || book == nullptr || root == nullptr || watch == nullptr)
        return false;

    Account* donor = first_account_with_commodity(root, nullptr);
    if (donor == nullptr)
        return false;
    gnc_commodity* commodity = xaccAccountGetCommodity(donor);

    Account* negative =
        make_holding_account(book, root, "Negative Total Test", commodity);
    if (negative == nullptr)
        return false;

    Transaction* trans = xaccMallocTransaction(book);
    xaccTransBeginEdit(trans);
    xaccTransSetCurrency(trans, commodity);
    xaccTransSetDatePostedSecsNormalized(trans, gnc_time64_get_today_start());

    Split* debit = xaccMallocSplit(book);
    xaccSplitSetAccount(debit, negative);
    xaccSplitSetParent(debit, trans);
    xaccSplitSetAmount(debit, gnc_numeric_create(-100, 1));
    xaccSplitSetValue(debit, gnc_numeric_create(-100, 1));

    Split* credit = xaccMallocSplit(book);
    xaccSplitSetAccount(credit, donor);
    xaccSplitSetParent(credit, trans);
    xaccSplitSetAmount(credit, gnc_numeric_create(100, 1));
    xaccSplitSetValue(credit, gnc_numeric_create(100, 1));

    xaccTransCommitEdit(trans);

    const QModelIndex total = find_account_index(model, QModelIndex(), negative,
                                                 gncqt::AccountModel::TotalColumn);
    if (!total.isValid())
        return false;

    const bool original =
        gnc_prefs_get_bool(GNC_PREFS_GROUP_GENERAL, GNC_PREF_NEGATIVE_IN_RED) != FALSE;

    gnc_gsettings_set_bool(GNC_PREFS_GROUP_GENERAL, GNC_PREF_NEGATIVE_IN_RED, TRUE);
    const bool reads_back_on =
        gnc_prefs_get_bool(GNC_PREFS_GROUP_GENERAL, GNC_PREF_NEGATIVE_IN_RED) != FALSE;
    apply_negative_in_red(watch);
    const bool red_when_on = model->data(total, Qt::ForegroundRole).isValid();

    gnc_gsettings_set_bool(GNC_PREFS_GROUP_GENERAL, GNC_PREF_NEGATIVE_IN_RED, FALSE);
    const bool reads_back_off =
        gnc_prefs_get_bool(GNC_PREFS_GROUP_GENERAL, GNC_PREF_NEGATIVE_IN_RED) == FALSE;
    apply_negative_in_red(watch);
    const bool plain_when_off = !model->data(total, Qt::ForegroundRole).isValid();

    // Leave the user's setting exactly as it was.
    gnc_gsettings_set_bool(GNC_PREFS_GROUP_GENERAL, GNC_PREF_NEGATIVE_IN_RED, original);
    apply_negative_in_red(watch);

    return reads_back_on && reads_back_off && red_when_on && plain_when_off;
}

gboolean rename_burst(gpointer data)
{
    auto* account = static_cast<Account*>(data);
    for (int i = 0; i < kRenames; ++i)
    {
        const QByteArray name = QByteArray("Renamed-") + QByteArray::number(i);
        xaccAccountSetName(account, name.constData());
    }
    return G_SOURCE_REMOVE;
}

struct Session
{
    QofSession* session = nullptr;
    Account* root = nullptr;
    int descendant_count = 0;
};

// One report page. Each tab owns its own view: two reports open at once must
// not fight over one widget, which is what the single-pane shell did.
struct ReportTab
{
    QString name;
    QWebEngineView* view = nullptr;
    bool loaded = false;
};

Session open_book(const QString& path)
{
    Session result;

    QofBook* book = qof_book_new();
    result.session = qof_session_new(book);

    qof_session_begin(result.session, path.toUtf8().constData(), SESSION_READ_ONLY);
    if (qof_session_get_error(result.session) != ERR_BACKEND_NO_ERR)
    {
        std::fprintf(stderr, "begin failed: %s\n",
                     qof_session_get_error_message(result.session));
        return result;
    }

    qof_session_load(result.session, nullptr);
    if (qof_session_get_error(result.session) != ERR_BACKEND_NO_ERR)
    {
        std::fprintf(stderr, "load failed: %s\n",
                     qof_session_get_error_message(result.session));
        return result;
    }

    QofBook* loaded = qof_session_get_book(result.session);
    result.root = gnc_book_get_root_account(loaded);
    result.descendant_count = result.root ? gnc_account_n_descendants(result.root) : 0;
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("gnucash-qt"));

    const QStringList args = app.arguments();
    g_self_test = args.size() > 1 && args.at(1) == QStringLiteral("--self-test");

    QString backend_path;
    QString book_path;
    QString report_name = QStringLiteral("Trial Balance");

    if (g_self_test)
    {
        if (args.size() < 5)
        {
            std::fprintf(stderr,
                         "usage: %s --self-test <backend-module> <book> <report>\n",
                         qPrintable(args.at(0)));
            return 2;
        }
        backend_path = args.at(2);
        book_path = args.at(3);
        report_name = args.at(4);
    }
    else if (args.size() > 1)
    {
        book_path = args.at(1);
        backend_path = default_backend_path();
    }
    else
    {
        backend_path = default_backend_path();
    }

    qof_init();
    cashobjects_register();
    // Preferences are the shared GSettings store (ADR 0013), but the accessors
    // in core-utils answer FALSE until a backend is loaded; they do not reach
    // GSettings on their own.
    gnc_gsettings_load_backend();
    if (!backend_path.isEmpty() && !load_backend(backend_path))
        return 1;

    QMainWindow window;
    auto* model = new gncqt::AccountModel(&window);
    g_model = model;

    auto* tree = new QTreeView(&window);
    tree->setModel(model);
    tree->header()->setSectionResizeMode(gncqt::AccountModel::NameColumn,
                                         QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(gncqt::AccountModel::TotalColumn,
                                         QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(gncqt::AccountModel::PeriodTotalColumn,
                                         QHeaderView::ResizeToContents);

    // The preference decides the colour; the model only carries the flag.
    PrefWatch prefs{model, tree};
    apply_negative_in_red(&prefs);
    const gulong pref_cb = gnc_prefs_register_cb(
        GNC_PREFS_GROUP_GENERAL, GNC_PREF_NEGATIVE_IN_RED,
        reinterpret_cast<gpointer>(&on_negative_in_red_changed), &prefs);

    auto* tabs = new QTabWidget(&window);
    tabs->setTabsClosable(true);
    tabs->setDocumentMode(true);
    tabs->addTab(tree, QStringLiteral("Accounts"));
    // The accounts page is the shell's home; only reports close.
    tabs->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);
    tabs->tabBar()->setTabButton(0, QTabBar::LeftSide, nullptr);

    window.setCentralWidget(tabs);
    window.resize(1200, 800);
    window.setWindowTitle(QStringLiteral("GnuCash (Qt)"));

    // Bindings compared against GTK in doc/qt6-keyboard-audit.md. They are
    // declared once so the self-test can assert the same objects the UI uses.
    const QKeySequence kOpen(QStringLiteral("Ctrl+O"));
    const QKeySequence kQuit(QStringLiteral("Ctrl+Q"));
    const QKeySequence kReloadKeys[] = {
        QKeySequence::Refresh, QKeySequence(QStringLiteral("Ctrl+R"))
    };
    const QKeySequence kClosePage(QStringLiteral("Ctrl+W"));
    const QKeySequence kNextTab[] = {
        QKeySequence(QStringLiteral("Ctrl+Tab")),
        QKeySequence(QStringLiteral("Ctrl+PageDown"))
    };
    const QKeySequence kPrevTab[] = {
        QKeySequence(QStringLiteral("Ctrl+Shift+Tab")),
        QKeySequence(QStringLiteral("Ctrl+PageUp"))
    };

    // GTK's tree expands and collapses with +/- (GtkTreeView defaults); the Qt
    // tree has no such binding, so it gets one scoped to itself.
    auto* expand_key = new QShortcut(QKeySequence(Qt::Key_Plus), tree);
    expand_key->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(expand_key, &QShortcut::activated, [&]() {
        if (tree->currentIndex().isValid())
            tree->expand(tree->currentIndex());
    });
    auto* collapse_key = new QShortcut(QKeySequence(Qt::Key_Minus), tree);
    collapse_key->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(collapse_key, &QShortcut::activated, [&]() {
        if (tree->currentIndex().isValid())
            tree->collapse(tree->currentIndex());
    });

    Session session;
    bool pane_loaded = false;  // the first report tab, the gate the old harness checked
    QString current_book;
    int render_seq = 0;

    QVector<ReportTab*> report_tabs;

    const gncqt::ReportRunner runner;

    auto render_into = [&](ReportTab* tab) -> bool {
        if (current_book.isEmpty() || tab == nullptr || tab->view == nullptr)
            return false;

        // One file per render: two tabs must not overwrite each other's HTML.
        const QString html_path = QDir::tempPath()
            + QStringLiteral("/gnucash-qt-report-%1.html").arg(++render_seq);
        QFile::remove(html_path);

        if (!runner.render(current_book, tab->name, html_path))
        {
            std::fprintf(stderr, "report generation failed for \"%s\"\n",
                         qPrintable(tab->name));
            return false;
        }

        QFile report(html_path);
        if (!report.open(QIODevice::ReadOnly))
            return false;

        const QByteArray html = report.readAll();
        std::printf("report bytes: %d\n", static_cast<int>(html.size()));
        tab->loaded = false;
        tab->view->setHtml(QString::fromUtf8(html));
        return true;
    };

    auto all_report_tabs_loaded = [&]() -> bool {
        if (report_tabs.isEmpty())
            return false;
        for (const ReportTab* tab : report_tabs)
            if (!tab->loaded)
                return false;
        return true;
    };

    auto open_report_tab = [&](const QString& name) -> ReportTab* {
        for (ReportTab* existing : report_tabs)
        {
            if (existing->name == name)
            {
                tabs->setCurrentWidget(existing->view);
                return existing;
            }
        }

        auto* tab = new ReportTab;
        tab->name = name;
        tab->view = new QWebEngineView(tabs);

        QObject::connect(tab->view, &QWebEngineView::loadFinished, [&, tab](bool ok) {
            tab->loaded = ok;
            std::printf("report tab \"%s\" load ok=%d\n",
                        qPrintable(tab->name), ok ? 1 : 0);
            if (!report_tabs.isEmpty() && tab == report_tabs.first())
                pane_loaded = ok;
        });

        report_tabs.append(tab);
        tabs->addTab(tab->view, name);
        tabs->setCurrentWidget(tab->view);
        render_into(tab);
        return tab;
    };

    auto close_report_tabs = [&]() {
        while (!report_tabs.isEmpty())
        {
            ReportTab* stale = report_tabs.takeLast();
            tabs->removeTab(tabs->indexOf(stale->view));
            delete stale->view;
            delete stale;
        }
        pane_loaded = false;
    };

    auto open_and_show = [&](const QString& path) -> bool {
        session = open_book(path);
        if (session.session == nullptr
            || qof_session_get_error(session.session) != ERR_BACKEND_NO_ERR)
        {
            QMessageBox::warning(&window, QStringLiteral("Cannot open"),
                                 QStringLiteral("Could not open %1").arg(path));
            return false;
        }

        current_book = path;
        model->set_root_account(session.root);
        window.setWindowTitle(QStringLiteral("GnuCash (Qt) - %1")
                                  .arg(QFileInfo(path).fileName()));

        // A different book invalidates the reports rendered from the old one.
        close_report_tabs();
        return open_report_tab(report_name) != nullptr;
    };

    // Closing a page drops its tab and its view together. The accounts page is
    // the shell's home and cannot be closed.
    auto close_page = [&](int index) {
        if (index <= 0 || index >= tabs->count())
            return;

        QWidget* widget = tabs->widget(index);
        tabs->removeTab(index);
        for (int i = 0; i < report_tabs.size(); ++i)
        {
            if (report_tabs.at(i)->view == widget)
            {
                ReportTab* gone = report_tabs.takeAt(i);
                delete gone->view;
                delete gone;
                break;
            }
        }
    };

    auto* file_menu = window.menuBar()->addMenu(QStringLiteral("&File"));
    auto* open_action = file_menu->addAction(QStringLiteral("&Open..."));
    open_action->setShortcuts({kOpen});
    QObject::connect(open_action, &QAction::triggered, [&]() {
        const QString path = QFileDialog::getOpenFileName(
            &window, QStringLiteral("Open a book"), QString(),
            QStringLiteral("GnuCash files (*.gnucash *.xac *.xml);;All files (*)"));
        if (!path.isEmpty())
            open_and_show(path);
    });
    file_menu->addSeparator();
    auto* close_action = file_menu->addAction(QStringLiteral("&Close page"));
    close_action->setShortcuts({kClosePage});
    QObject::connect(close_action, &QAction::triggered, [&]() {
        close_page(tabs->currentIndex());
    });
    file_menu->addSeparator();
    auto* quit_action = file_menu->addAction(QStringLiteral("&Quit"));
    quit_action->setShortcuts({kQuit});
    QObject::connect(quit_action, &QAction::triggered, &app, &QApplication::quit);

    // The report list comes from the helper: this build's installed reports,
    // not a hardcoded table.
    const QStringList report_names = runner.list_reports();
    auto* reports_menu = window.menuBar()->addMenu(QStringLiteral("&Reports"));
    for (const QString& name : report_names)
    {
        auto* action = reports_menu->addAction(name);
        QObject::connect(action, &QAction::triggered, [&, name]() {
            open_report_tab(name);
        });
    }
    if (report_names.isEmpty())
        reports_menu->addAction(QStringLiteral("(none available)"))->setEnabled(false);

    auto* view_menu = window.menuBar()->addMenu(QStringLiteral("&View"));
    auto* reload_action = view_menu->addAction(QStringLiteral("&Reload report"));
    reload_action->setShortcuts({kReloadKeys[0], kReloadKeys[1]});
    QObject::connect(reload_action, &QAction::triggered, [&]() {
        QWidget* current = tabs->currentWidget();
        for (ReportTab* tab : report_tabs)
        {
            if (tab->view == current)
            {
                render_into(tab);
                break;
            }
        }
    });

    auto* next_tab_action = view_menu->addAction(QStringLiteral("&Next tab"));
    next_tab_action->setShortcuts({kNextTab[0], kNextTab[1]});
    QObject::connect(next_tab_action, &QAction::triggered, [&]() {
        if (tabs->count() > 0)
            tabs->setCurrentIndex((tabs->currentIndex() + 1) % tabs->count());
    });

    auto* prev_tab_action = view_menu->addAction(QStringLiteral("&Previous tab"));
    prev_tab_action->setShortcuts({kPrevTab[0], kPrevTab[1]});
    QObject::connect(prev_tab_action, &QAction::triggered, [&]() {
        if (tabs->count() > 0)
            tabs->setCurrentIndex((tabs->currentIndex() + tabs->count() - 1)
                                  % tabs->count());
    });

    // The tab bar's close button and Ctrl+W do the same thing.
    QObject::connect(tabs, &QTabWidget::tabCloseRequested, [&](int index) {
        close_page(index);
    });

    // The engine may schedule GLib sources; they run on this thread (ADR 0012).
    GMainContext* context = g_main_context_default();
    QTimer pump;
    QObject::connect(&pump, &QTimer::timeout, [context]() {
        while (g_main_context_iteration(context, FALSE))
        {
        }
    });
    pump.start(5);

    qof_event_register_handler(on_qof_event, nullptr);

    if (!book_path.isEmpty() && !open_and_show(book_path))
        return 1;

    if (!g_self_test)
    {
        window.show();
        const int status = app.exec();
        pump.stop();
        if (session.session != nullptr)
            qof_session_end(session.session);
        return status;
    }

    // ---- self-test: tabs, model edits, totals, rename burst, assertions ----
    // Recorded before the tests run, so the temporary objects they create
    // cannot change what this reports about the book that was loaded.
    const int top_level = model->rowCount();

    // A second report page, to prove tabs are pages and not a relabelled pane.
    // The helper's list is the source, so this stays true as reports change.
    QString second_report;
    for (const QString& name : report_names)
    {
        if (name != report_name)
        {
            second_report = name;
            break;
        }
    }
    if (!second_report.isEmpty())
        open_report_tab(second_report);

    EditTest edit;
    TotalTest total;
    bool prefs_ok = false;
    if (session.session != nullptr && session.root != nullptr)
    {
        QofBook* book = qof_session_get_book(session.session);
        edit = run_model_edit_test(model, book, session.root);
        total = run_total_test(model, book, session.root);
        prefs_ok = run_prefs_test(model, book, session.root, &prefs) && pref_cb != 0;
    }

    // The keyboard audit's browsing bindings, asserted on the actions rather
    // than by sending key events: the action is what the menu and the shortcut
    // both go through.
    const auto has_shortcut = [](const QAction* action, const QKeySequence& keys) {
        return action != nullptr && action->shortcuts().contains(keys);
    };
    const bool keys_ok =
        has_shortcut(open_action, kOpen)
        && has_shortcut(quit_action, kQuit)
        && has_shortcut(reload_action, kReloadKeys[0])
        && has_shortcut(reload_action, kReloadKeys[1])
        && has_shortcut(close_action, kClosePage)
        && has_shortcut(next_tab_action, kNextTab[0])
        && has_shortcut(next_tab_action, kNextTab[1])
        && has_shortcut(prev_tab_action, kPrevTab[0])
        && has_shortcut(prev_tab_action, kPrevTab[1])
        && tree->findChildren<QShortcut*>().size() == 2;

    std::printf("keys: open=%d quit=%d reload_f5=%d reload_cr=%d close=%d "
                "next_tab=%d next_pg=%d prev_tab=%d prev_pg=%d tree=%d\n",
                has_shortcut(open_action, kOpen) ? 1 : 0,
                has_shortcut(quit_action, kQuit) ? 1 : 0,
                has_shortcut(reload_action, kReloadKeys[0]) ? 1 : 0,
                has_shortcut(reload_action, kReloadKeys[1]) ? 1 : 0,
                has_shortcut(close_action, kClosePage) ? 1 : 0,
                has_shortcut(next_tab_action, kNextTab[0]) ? 1 : 0,
                has_shortcut(next_tab_action, kNextTab[1]) ? 1 : 0,
                has_shortcut(prev_tab_action, kPrevTab[0]) ? 1 : 0,
                has_shortcut(prev_tab_action, kPrevTab[1]) ? 1 : 0,
                static_cast<int>(tree->findChildren<QShortcut*>().size()));

    Account* target = nullptr;
    if (session.root != nullptr)
    {
        GList* children = gnc_account_get_children(session.root);
        if (children != nullptr)
            target = static_cast<Account*>(children->data);
        g_list_free(children);
    }

    if (target != nullptr)
    {
        // Loading the book itself emits modify events; the burst is what this
        // test measures, so the counters start here rather than at startup.
        g_observed.clear();
        g_event_count.store(0);
        g_event_off_thread.store(false);

        GSource* source = g_timeout_source_new(300);
        g_source_set_callback(source, rename_burst, target, nullptr);
        g_source_attach(source, context);
        g_source_unref(source);
    }

    QTimer poll;
    int ticks = 0;
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        const bool events_done = g_event_count.load() >= kRenames;
        const bool reports_loaded = all_report_tabs_loaded();
        if ((!events_done || !reports_loaded) && ++ticks < 600)
            return;

        bool in_order = (g_observed.size() == kRenames);
        for (int i = 0; in_order && i < kRenames; ++i)
        {
            const QByteArray expected = QByteArray("Renamed-") + QByteArray::number(i);
            in_order = (g_observed.at(i) == expected);
        }

        // The page model: one accounts page, then a page per report, in the
        // order the shell opened them.
        bool tabs_ok = tabs->count() == 1 + report_tabs.size()
                       && tabs->tabText(0) == QStringLiteral("Accounts");
        for (int i = 0; tabs_ok && i < report_tabs.size(); ++i)
            tabs_ok = tabs->tabText(i + 1) == report_tabs.at(i)->name;
        tabs_ok = tabs_ok && !second_report.isEmpty()
                  && report_tabs.size() == 2 && reports_loaded;

        std::printf("accounts=%d top_level=%d pane_ok=%d tabs_ok=%d keys_ok=%d "
                    "events=%d/%d in_order=%d on_qt_thread=%d model_ok=%d "
                    "total_ok=%d period_ok=%d prefs_ok=%d inserts=%d removes=%d "
                    "reports=%d\n",
                    session.descendant_count, top_level,
                    pane_loaded ? 1 : 0, tabs_ok ? 1 : 0, keys_ok ? 1 : 0,
                    g_event_count.load(), kRenames,
                    in_order ? 1 : 0, g_event_off_thread.load() ? 0 : 1,
                    edit.ok ? 1 : 0, total.ok ? 1 : 0, total.period_ok ? 1 : 0,
                    prefs_ok ? 1 : 0, edit.inserts, edit.removes,
                    static_cast<int>(report_names.size()));

        app.exit(events_done && in_order && pane_loaded && tabs_ok && keys_ok
                         && edit.ok && total.ok && total.period_ok && prefs_ok
                         && !g_event_off_thread.load()
                         && !report_names.isEmpty()
                     ? 0
                     : 1);
    });
    poll.start(50);

    QTimer::singleShot(60000, &app, [&]() {
        std::fprintf(stderr, "timeout\n");
        app.exit(3);
    });

    window.show();
    const int status = app.exec();

    pump.stop();
    if (session.session != nullptr)
        qof_session_end(session.session);
    return status;
}

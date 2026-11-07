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
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QVector>
#include <QWebEngineView>

#include "account-model.h"
#include "report-runner.h"

extern "C" {
#include <cashobjects.h>
#include <gnc-engine.h>
#include <qof.h>
#include <qofbook.h>
#include <qofevent.h>
#include <qofsession.h>
#include <Account.h>
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
    if (!backend_path.isEmpty() && !load_backend(backend_path))
        return 1;

    QMainWindow window;
    auto* model = new gncqt::AccountModel(&window);
    g_model = model;

    auto* tree = new QTreeView(&window);
    tree->setModel(model);

    auto* view = new QWebEngineView(&window);

    auto* splitter = new QSplitter(&window);
    splitter->addWidget(tree);
    splitter->addWidget(view);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    window.setCentralWidget(splitter);
    window.resize(1200, 800);
    window.setWindowTitle(QStringLiteral("GnuCash (Qt)"));

    Session session;
    bool pane_loaded = false;
    QString current_book;
    QString current_report = report_name;

    const gncqt::ReportRunner runner;

    auto render_current = [&]() -> bool {
        if (current_book.isEmpty())
            return false;

        const QString html_path =
            QDir::tempPath() + QStringLiteral("/gnucash-qt-report.html");
        QFile::remove(html_path);

        if (!runner.render(current_book, current_report, html_path))
        {
            std::fprintf(stderr, "report generation failed for \"%s\"\n",
                         qPrintable(current_report));
            return false;
        }

        QFile report(html_path);
        if (!report.open(QIODevice::ReadOnly))
            return false;

        const QByteArray html = report.readAll();
        std::printf("report bytes: %d\n", static_cast<int>(html.size()));
        view->setHtml(QString::fromUtf8(html));
        return true;
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
        return render_current();
    };

    auto* file_menu = window.menuBar()->addMenu(QStringLiteral("&File"));
    auto* open_action = file_menu->addAction(QStringLiteral("&Open..."));
    open_action->setShortcut(QKeySequence::Open);
    QObject::connect(open_action, &QAction::triggered, [&]() {
        const QString path = QFileDialog::getOpenFileName(
            &window, QStringLiteral("Open a book"), QString(),
            QStringLiteral("GnuCash files (*.gnucash *.xac *.xml);;All files (*)"));
        if (!path.isEmpty())
            open_and_show(path);
    });
    file_menu->addSeparator();
    auto* quit_action = file_menu->addAction(QStringLiteral("&Quit"));
    quit_action->setShortcut(QKeySequence::Quit);
    QObject::connect(quit_action, &QAction::triggered, &app, &QApplication::quit);

    // The report list comes from the helper: this build's installed reports,
    // not a hardcoded table.
    const QStringList report_names = runner.list_reports();
    auto* reports_menu = window.menuBar()->addMenu(QStringLiteral("&Reports"));
    for (const QString& name : report_names)
    {
        auto* action = reports_menu->addAction(name);
        QObject::connect(action, &QAction::triggered, [&, name]() {
            current_report = name;
            render_current();
        });
    }
    if (report_names.isEmpty())
        reports_menu->addAction(QStringLiteral("(none available)"))->setEnabled(false);

    auto* view_menu = window.menuBar()->addMenu(QStringLiteral("&View"));
    auto* reload_action = view_menu->addAction(QStringLiteral("&Reload report"));
    reload_action->setShortcut(QKeySequence::Refresh);
    QObject::connect(reload_action, &QAction::triggered, [&]() { render_current(); });

    QObject::connect(view, &QWebEngineView::loadFinished, [&](bool ok) {
        pane_loaded = ok;
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

    // ---- self-test: model edits, rename burst, then assertions ----
    EditTest edit;
    if (session.session != nullptr && session.root != nullptr)
        edit = run_model_edit_test(model, qof_session_get_book(session.session),
                                   session.root);

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
        if ((!events_done || !pane_loaded) && ++ticks < 200)
            return;

        bool in_order = (g_observed.size() == kRenames);
        for (int i = 0; in_order && i < kRenames; ++i)
        {
            const QByteArray expected = QByteArray("Renamed-") + QByteArray::number(i);
            in_order = (g_observed.at(i) == expected);
        }

        std::printf("accounts=%d top_level=%d pane_ok=%d events=%d/%d in_order=%d "
                    "on_qt_thread=%d model_ok=%d inserts=%d removes=%d reports=%d\n",
                    session.descendant_count, model->rowCount(),
                    pane_loaded ? 1 : 0, g_event_count.load(), kRenames,
                    in_order ? 1 : 0, g_event_off_thread.load() ? 0 : 1,
                    edit.ok ? 1 : 0, edit.inserts, edit.removes,
                    static_cast<int>(report_names.size()));

        app.exit(events_done && in_order && pane_loaded && edit.ok
                         && !g_event_off_thread.load() && !report_names.isEmpty()
                     ? 0
                     : 1);
    });
    poll.start(50);

    QTimer::singleShot(30000, &app, [&]() {
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

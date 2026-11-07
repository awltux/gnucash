// GnuCash Qt 6 shell — Phase 2 slice.
//
// Loads a book through libgnucash, shows its accounts, renders a report
// document in a QWebEngineView, and runs an event-load self-test: N renaming
// operations are performed on the pumped GLib context (ADR 0002) and every
// resulting QofEvent must reach a Qt slot, on the Qt thread, in order.
//
// The single-event version only proved the bridge exists. This proves it
// survives a burst, which is what a register will produce in practice.

#include <atomic>
#include <cstdio>
#include <thread>

#include <dlfcn.h>

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QMainWindow>
#include <QSplitter>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVector>
#include <QWebEnginePage>
#include <QWebEngineView>

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

std::atomic<bool> g_stop{false};
std::atomic<int> g_qt_event_count{0};
std::atomic<bool> g_any_event_off_thread{false};

// Written and read only on the Qt thread; the handler posts into it.
QVector<QByteArray> g_observed;

bool load_backend(const char* path)
{
    void* handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr)
    {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return false;
    }

    InitFn init = reinterpret_cast<InitFn>(dlsym(handle, "qof_backend_module_init"));
    if (init == nullptr)
        init = reinterpret_cast<InitFn>(dlsym(handle, "gnc_module_init_backend_xml"));
    if (init == nullptr)
    {
        std::fprintf(stderr, "no backend init symbol in %s\n", path);
        return false;
    }

    init();
    return true;
}

// Runs on the GLib thread. The name is sampled here, not in the Qt slot:
// by the time the queued call executes the account has been renamed again.
void on_qof_event(QofInstance* entity, QofEventId type, gpointer, gpointer)
{
    if (!(type & QOF_EVENT_MODIFY) || entity == nullptr)
        return;

    auto* account = reinterpret_cast<Account*>(entity);
    const char* name = xaccAccountGetName(account);
    const QByteArray observed(name ? name : "");

    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [observed]() {
            const bool on_qt_thread =
                QThread::currentThread() == QCoreApplication::instance()->thread();
            if (!on_qt_thread)
                g_any_event_off_thread.store(true);
            g_observed.append(observed);
            g_qt_event_count.fetch_add(1);
        },
        Qt::QueuedConnection);
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

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    if (argc < 4)
    {
        std::fprintf(stderr,
                     "usage: %s <backend-module> <book.gnucash> <report.html>\n", argv[0]);
        return 2;
    }

    qof_init();
    cashobjects_register();
    if (!load_backend(argv[1]))
        return 1;

    QofBook* book = qof_book_new();
    QofSession* session = qof_session_new(book);

    qof_session_begin(session, argv[2], SESSION_READ_ONLY);
    if (qof_session_get_error(session) != ERR_BACKEND_NO_ERR)
    {
        std::fprintf(stderr, "begin failed: %s\n", qof_session_get_error_message(session));
        return 1;
    }
    qof_session_load(session, nullptr);
    if (qof_session_get_error(session) != ERR_BACKEND_NO_ERR)
    {
        std::fprintf(stderr, "load failed: %s\n", qof_session_get_error_message(session));
        return 1;
    }

    QofBook* loaded = qof_session_get_book(session);
    Account* root = gnc_book_get_root_account(loaded);
    const int total = root ? gnc_account_n_descendants(root) : 0;

    GList* children = root ? gnc_account_get_children(root) : nullptr;
    Account* target = children ? static_cast<Account*>(children->data) : nullptr;

    QMainWindow window;
    auto* accounts = new QTreeWidget(&window);
    accounts->setHeaderLabel(QStringLiteral("Accounts"));
    int top_level = 0;
    for (GList* node = children; node != nullptr; node = node->next)
    {
        auto* account = static_cast<Account*>(node->data);
        const char* name = xaccAccountGetName(account);
        accounts->addTopLevelItem(
            new QTreeWidgetItem(QStringList{QString::fromUtf8(name ? name : "")}));
        ++top_level;
    }
    g_list_free(children);

    QFile report(argv[3]);
    if (!report.open(QIODevice::ReadOnly))
    {
        std::fprintf(stderr, "cannot read %s\n", argv[3]);
        return 1;
    }
    const QByteArray html = report.readAll();

    auto* view = new QWebEngineView(&window);
    auto* splitter = new QSplitter(&window);
    splitter->addWidget(accounts);
    splitter->addWidget(view);
    window.setCentralWidget(splitter);
    window.setWindowTitle(QStringLiteral("GnuCash Qt shell"));
    window.resize(1200, 800);

    GMainContext* context = g_main_context_new();
    std::thread glib_thread([context, target]() {
        g_main_context_push_thread_default(context);

        if (target != nullptr)
        {
            GSource* source = g_timeout_source_new(300);
            g_source_set_callback(source, rename_burst, target, nullptr);
            g_source_attach(source, context);
            g_source_unref(source);
        }

        while (!g_stop.load())
            g_main_context_iteration(context, TRUE);

        g_main_context_pop_thread_default(context);
    });

    qof_event_register_handler(on_qof_event, nullptr);

    bool pane_loaded = false;
    QObject::connect(view, &QWebEngineView::loadFinished, [&](bool ok) {
        pane_loaded = ok;
    });

    QTimer poll;
    int ticks = 0;
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        const bool events_done = g_qt_event_count.load() >= kRenames;
        // Wait for the pane as well: WebEngine loads asynchronously and takes
        // longer than a 300 ms rename burst.
        if ((!events_done || !pane_loaded) && ++ticks < 200)
            return;

        bool in_order = (g_observed.size() == kRenames);
        if (in_order)
        {
            for (int i = 0; i < kRenames; ++i)
            {
                const QByteArray expected =
                    QByteArray("Renamed-") + QByteArray::number(i);
                if (g_observed.at(i) != expected)
                {
                    in_order = false;
                    break;
                }
            }
        }

        std::printf("accounts=%d top_level=%d pane_ok=%d events=%d/%d in_order=%d "
                    "on_qt_thread=%d\n",
                    total, top_level, pane_loaded ? 1 : 0,
                    g_qt_event_count.load(), kRenames, in_order ? 1 : 0,
                    g_any_event_off_thread.load() ? 0 : 1);

        app.exit(events_done && in_order && pane_loaded
                     && !g_any_event_off_thread.load() ? 0 : 1);
    });
    poll.start(50);

    QTimer::singleShot(30000, &app, [&]() {
        std::fprintf(stderr, "timeout\n");
        app.exit(3);
    });

    view->setHtml(QString::fromUtf8(html));
    window.show();

    const int status = app.exec();

    g_stop.store(true);
    g_main_context_wakeup(context);
    glib_thread.join();
    g_main_context_unref(context);
    qof_session_end(session);
    return status;
}

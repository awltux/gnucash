// GnuCash Qt 6 shell — Phase 2 first slice.
//
// What this is: a real build target that loads a book through libgnucash,
// shows its accounts, and renders a report document in a QWebEngineView.
//
// What it proves beyond the earlier spikes: an engine change made on the GLib
// side reaches a Qt slot *on the Qt thread*, through the pumped context from
// ADR 0002. That is the structural unknown Phase 2 depends on.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include <dlfcn.h>

#include <QApplication>
#include <QFile>
#include <QMainWindow>
#include <QSplitter>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
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

std::atomic<bool> g_stop{false};
std::atomic<bool> g_qt_saw_event{false};
std::atomic<int> g_qt_event_count{0};
std::atomic<bool> g_event_on_qt_thread{false};
QByteArray g_observed_name;

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

// Runs on whichever thread emits the engine event — here, the GLib thread.
void on_qof_event(QofInstance* entity, QofEventId type, gpointer, gpointer)
{
    if (!(type & QOF_EVENT_MODIFY) || entity == nullptr)
        return;

    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [entity]() {
            g_qt_saw_event.store(true);
            g_qt_event_count.fetch_add(1);
            g_event_on_qt_thread.store(
                QThread::currentThread() == QCoreApplication::instance()->thread());

            auto* account = reinterpret_cast<Account*>(entity);
            const char* name = xaccAccountGetName(account);
            g_observed_name = QByteArray(name ? name : "");
        },
        Qt::QueuedConnection);
}

gboolean rename_into_glib(gpointer data)
{
    auto* account = static_cast<Account*>(data);
    xaccAccountSetName(account, "Renamed by the GLib thread");
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

    // The account the GLib thread will rename.
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

    // The pumped context from ADR 0002, on its own thread.
    GMainContext* context = g_main_context_new();
    std::thread glib_thread([context, target]() {
        g_main_context_push_thread_default(context);

        if (target != nullptr)
        {
            GSource* rename_source = g_timeout_source_new(300);
            g_source_set_callback(rename_source, rename_into_glib, target, nullptr);
            g_source_attach(rename_source, context);
            g_source_unref(rename_source);
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

    // Poll for the crossed event; quit as soon as it arrives or give up.
    QTimer poll;
    int ticks = 0;
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        if (g_qt_saw_event.load() || ++ticks > 100)
        {
            std::printf("accounts=%d top_level=%d pane_ok=%d event_on_qt_thread=%d "
                        "events=%d renamed_to=%s\n",
                        total, top_level, pane_loaded ? 1 : 0,
                        g_event_on_qt_thread.load() ? 1 : 0,
                        g_qt_event_count.load(), g_observed_name.constData());
            app.exit(g_qt_saw_event.load() && g_event_on_qt_thread.load() && pane_loaded
                         ? 0 : 1);
        }
    });
    poll.start(50);

    QTimer::singleShot(20000, &app, [&]() {
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

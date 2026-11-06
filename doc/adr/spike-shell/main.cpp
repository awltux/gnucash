// Qt Phase 1 shell spike: does the combination hold in one process?
//
// Left pane  : accounts read from a real book through libgnucash.
// Right pane : HTML produced by GnuCash's report engine, in a QWebEngineView.
//
// The two halves were proven separately (ADR 0009, spike-report-pane). What is
// new here is that they share a process, which is the shape Phase 1 needs.

#include <cstdio>

#include <dlfcn.h>

#include <QApplication>
#include <QFile>
#include <QMainWindow>
#include <QSplitter>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QWebEnginePage>
#include <QWebEngineView>

extern "C" {
#include <cashobjects.h>
#include <qof.h>
#include <qofbook.h>
#include <qofsession.h>
#include <Account.h>
}

namespace
{

using InitFn = void (*)();

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

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <backend-module.so> <book.gnucash> <report.html>\n",
                     argv[0]);
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

    QMainWindow window;
    auto* accounts = new QTreeWidget(&window);
    accounts->setHeaderLabel(QStringLiteral("Accounts"));

    int top_level = 0;
    GList* children = root ? gnc_account_get_children(root) : nullptr;
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
        return 2;
    }
    const QByteArray html = report.readAll();

    auto* view = new QWebEngineView(&window);

    QObject::connect(view, &QWebEngineView::loadFinished, [&](bool ok) {
        view->page()->toHtml([&, ok](const QString& rendered) {
            std::printf("accounts=%d top_level=%d html_ok=%d rendered_bytes=%d\n",
                        total, top_level, ok ? 1 : 0, static_cast<int>(rendered.size()));
            app.exit(ok && !rendered.isEmpty() && total > 0 ? 0 : 1);
        });
    });

    auto* splitter = new QSplitter(&window);
    splitter->addWidget(accounts);
    splitter->addWidget(view);
    window.setCentralWidget(splitter);
    window.setWindowTitle(QStringLiteral("GnuCash Qt shell spike"));
    window.resize(1200, 800);

    QTimer::singleShot(15000, &app, [&]() {
        std::fprintf(stderr, "timeout waiting for the report pane\n");
        app.exit(3);
    });

    view->setHtml(QString::fromUtf8(html));
    window.show();

    const int status = app.exec();
    qof_session_end(session);
    return status;
}

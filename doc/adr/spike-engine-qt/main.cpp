// Qt + libgnucash spike: can a QCoreApplication open a real book?
//
// Answers three questions at once, in the order they fail:
//   1. do Qt and the engine coexist in one process (link/ABI)?
//   2. how does a non-GnuCash process register a storage backend?
//   3. does a real book fixture load and expose its objects?
//
// This is evidence for doc/adr/0009 (or its successor), not application code.

#include <cstdio>
#include <cstring>

#include <dlfcn.h>

#include <QCoreApplication>

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

    // The module exports one of these depending on how it was built.
    InitFn init = reinterpret_cast<InitFn>(dlsym(handle, "qof_backend_module_init"));
    if (init == nullptr)
        init = reinterpret_cast<InitFn>(dlsym(handle, "gnc_module_init_backend_xml"));
    if (init == nullptr)
    {
        std::fprintf(stderr, "no init symbol found in %s\n", path);
        return false;
    }

    init();
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <backend-module.so> <book.gnucash>\n", argv[0]);
        return 2;
    }

    qof_init();
    cashobjects_register();

    if (!load_backend(argv[1]))
        return 1;

    QofBook* book = qof_book_new();
    QofSession* session = qof_session_new(book);

    qof_session_begin(session, argv[2], SESSION_READ_ONLY);
    QofBackendError err = qof_session_get_error(session);
    if (err != ERR_BACKEND_NO_ERR)
    {
        std::fprintf(stderr, "begin failed (%d): %s\n", static_cast<int>(err),
                     qof_session_get_error_message(session));
        return 1;
    }

    qof_session_load(session, nullptr);
    err = qof_session_get_error(session);
    if (err != ERR_BACKEND_NO_ERR)
    {
        std::fprintf(stderr, "load failed (%d): %s\n", static_cast<int>(err),
                     qof_session_get_error_message(session));
        return 1;
    }

    QofBook* loaded = qof_session_get_book(session);
    Account* root = gnc_book_get_root_account(loaded);
    const int accounts = root ? gnc_account_n_descendants(root) : -1;

    std::printf("accounts under root: %d\n", accounts);

    qof_session_end(session);
    return accounts > 0 ? 0 : 1;
}

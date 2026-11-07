/********************************************************************\
 * gnucash-webd -- a local web server that owns the engine          *
 *                                                                  *
 * The engine is single-threaded (no locking model), so everything  *
 * happens on one thread: Guile initialises here, the book is       *
 * opened here, and every request is handled on the GMainLoop that  *
 * this file runs. No worker threads, no marshalling.               *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
\********************************************************************/

#include <config.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include <gio/gio.h>
#include <glib.h>
#include <libguile.h>

extern "C" {
#include <cashobjects.h>
#include <gnc-engine.h>
#include <gnc-report.h>
#include <gnc-session.h>
#include <gnc-ui-balances.h>
#include <gnc-ui-util.h>
#include <qof.h>
#include <qofbook.h>
#include <qofsession.h>
#include <Account.h>
}

namespace
{

/* ------------------------------------------------------------------ state */

struct Server
{
    std::string token;
    Account* root = nullptr;
    int top_level = 0;
    int accounts = 0;
};

Server g_server;

/* ------------------------------------------------------------------- json */

std::string
json_escape (const char* text)
{
    std::string out;
    if (text == nullptr)
        return out;

    for (const char* p = text; *p; ++p)
    {
        switch (*p)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(*p) < 0x20)
            {
                char esc[8];
                g_snprintf (esc, sizeof esc, "\\u%04x", *p);
                out += esc;
            }
            else
                out += *p;
        }
    }
    return out;
}

std::string
account_json (Account* account)
{
    std::string out = "{\"name\":\"";
    out += json_escape (xaccAccountGetName (account));
    out += "\",\"total\":\"";

    gboolean negative = FALSE;
    gchar* text = gnc_ui_account_get_print_balance (
        xaccAccountGetPresentBalanceInCurrency, account, TRUE, &negative);
    out += json_escape (text != nullptr ? text : "");
    g_free (text);

    out += "\",\"negative\":";
    out += negative ? "true" : "false";
    out += ",\"children\":[";

    GList* children = gnc_account_get_children (account);
    for (GList* node = children; node != nullptr; node = node->next)
    {
        if (node != children)
            out += ",";
        out += account_json (static_cast<Account*>(node->data));
    }
    g_list_free (children);

    out += "]}";
    return out;
}

/* ------------------------------------------------------------------- http */

void
send_response (GSocketConnection* connection, const char* status,
               const char* content_type, const std::string& body)
{
    std::string response = "HTTP/1.1 ";
    response += status;
    response += "\r\nContent-Type: ";
    response += content_type;
    response += "\r\nContent-Length: ";
    response += std::to_string (body.size());
    response += "\r\nConnection: close\r\n";
    response += "X-Content-Type-Options: nosniff\r\n\r\n";
    response += body;

    GOutputStream* out = g_io_stream_get_output_stream (G_IO_STREAM (connection));
    gsize written = 0;
    g_output_stream_write_all (out, response.data(), response.size(), &written,
                               nullptr, nullptr);
}

std::string
url_decode (const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == '%' && i + 2 < in.size())
        {
            const int high = g_ascii_xdigit_value (in[i + 1]);
            const int low = g_ascii_xdigit_value (in[i + 2]);
            if (high >= 0 && low >= 0)
            {
                out += static_cast<char> (high * 16 + low);
                i += 2;
                continue;
            }
        }
        out += in[i] == '+' ? ' ' : in[i];
    }
    return out;
}

/* Reads until the end of the request headers. Returns the path, or an empty
 * string if the request is malformed. */
std::string
read_request_path (GSocketConnection* connection)
{
    GInputStream* in = g_io_stream_get_input_stream (G_IO_STREAM (connection));
    std::string request;
    char buffer[1024];

    while (request.find ("\r\n\r\n") == std::string::npos
           && request.size() < 16 * 1024)
    {
        gssize read = g_input_stream_read (in, buffer, sizeof buffer, nullptr,
                                           nullptr);
        if (read <= 0)
            break;
        request.append (buffer, static_cast<size_t>(read));
    }

    const size_t line_end = request.find ("\r\n");
    if (line_end == std::string::npos)
        return {};

    const std::string line = request.substr (0, line_end);
    /* "GET <path> HTTP/1.1" */
    const size_t first = line.find (' ');
    const size_t second = line.find (' ', first + 1);
    if (first == std::string::npos || second == std::string::npos)
        return {};

    return line.substr (first + 1, second - first - 1);
}

/* The first path segment is the token. Anything else is a 404, which is also
 * what an attacker without the token sees. */
bool
authorised (const std::string& path)
{
    const std::string prefix = "/" + g_server.token + "/";
    return path.compare (0, prefix.size(), prefix) == 0;
}

std::string
static_file (const char* name)
{
    std::string body;
    gchar* contents = nullptr;
    gsize length = 0;
    const gchar* dir = g_getenv ("GNC_WEBUI_DIR");

    if (dir == nullptr)
        dir = GNC_WEBUI_DIR;

    gchar* path = g_build_filename (dir, name, nullptr);
    if (g_file_get_contents (path, &contents, &length, nullptr))
        body.assign (contents, length);
    g_free (path);
    g_free (contents);
    return body;
}

gboolean
on_incoming (GSocketService*, GSocketConnection* connection, GObject*, gpointer)
{
    const std::string path = read_request_path (connection);

    if (!authorised (path))
    {
        send_response (connection, "404 Not Found", "text/plain", "not found\n");
        return TRUE;
    }

    const std::string prefix = "/" + g_server.token + "/";
    const std::string route = path.substr (prefix.size());

    if (route == "api/book")
    {
        std::string body = "{\"accounts\":";
        body += std::to_string (g_server.accounts);
        body += ",\"top_level\":";
        body += std::to_string (g_server.top_level);
        body += "}";
        send_response (connection, "200 OK", "application/json", body);
    }
    else if (route == "api/accounts")
    {
        send_response (connection, "200 OK", "application/json",
                       account_json (g_server.root));
    }
    else if (route.compare (0, 7, "report/") == 0)
    {
        /* Rendered here, in this process: the report engine is toolkit-free,
         * so there is no helper process to spawn and no GTK to avoid. */
        const std::string name = url_decode (route.substr (7));
        SCM get_id = scm_c_eval_string ("gnc:cmdline-get-report-id");
        SCM id = scm_call_1 (get_id, scm_from_utf8_string (name.c_str ()));

        if (scm_is_false (id))
        {
            send_response (connection, "404 Not Found", "text/plain",
                           "no such report\n");
            return TRUE;
        }

        char* html = nullptr;
        char* errmsg = nullptr;
        if (gnc_run_report_with_error_handling (scm_to_int (id), &html, &errmsg))
        {
            send_response (connection, "200 OK", "text/html; charset=utf-8",
                           html != nullptr ? html : "");
            g_free (html);
        }
        else
        {
            send_response (connection, "500 Internal Server Error", "text/plain",
                           errmsg != nullptr ? errmsg : "report failed\n");
            g_free (errmsg);
        }
    }
    else if (route.empty() || route == "index.html")
    {
        send_response (connection, "200 OK", "text/html; charset=utf-8",
                       static_file ("index.html"));
    }
    else
    {
        send_response (connection, "404 Not Found", "text/plain", "not found\n");
    }

    return TRUE;
}

/* ---------------------------------------------------------------- startup */

std::string
random_token ()
{
    char raw[16];
    for (char& byte : raw)
        byte = static_cast<char> (g_random_int_range (0, 256));

    gchar* hex = g_strdup_printf (
        "%02x%02x%02x%02x%02x%02x%02x%02x"
        "%02x%02x%02x%02x%02x%02x%02x%02x",
        (unsigned char) raw[0], (unsigned char) raw[1],
        (unsigned char) raw[2], (unsigned char) raw[3],
        (unsigned char) raw[4], (unsigned char) raw[5],
        (unsigned char) raw[6], (unsigned char) raw[7],
        (unsigned char) raw[8], (unsigned char) raw[9],
        (unsigned char) raw[10], (unsigned char) raw[11],
        (unsigned char) raw[12], (unsigned char) raw[13],
        (unsigned char) raw[14], (unsigned char) raw[15]);
    std::string token (hex);
    g_free (hex);
    return token;
}

struct Arguments
{
    const char* backend = nullptr;
    const char* book = nullptr;
};

void
webd_main (void* data, int, char**)
{
    auto* args = static_cast<Arguments*>(data);

    /* The same prelude the CLI uses for `-R run`: the Guile modules bring up
     * the engine (qof_init and the object registry) themselves, and doing it
     * again here trips the engine's own assertions. Backends are found through
     * GNC_MODULE_PATH by qof_session_begin. */
    scm_c_eval_string ("(debug-set! stack 200000)");
    scm_c_use_module ("gnucash utilities");
    scm_c_use_module ("gnucash app-utils");
    scm_c_use_module ("gnucash reports");

    gnc_report_init ();

    QofSession* session = gnc_get_current_session ();
    if (session == nullptr)
    {
        std::fprintf (stderr, "webd: no session\n");
        exit (1);
    }

    qof_session_begin (session, args->book, SESSION_READ_ONLY);
    if (qof_session_get_error (session) != ERR_BACKEND_NO_ERR)
    {
        std::fprintf (stderr, "webd: cannot open %s: %s\n", args->book,
                      qof_session_get_error_message (session));
        exit (1);
    }

    qof_session_load (session, nullptr);
    if (qof_session_get_error (session) != ERR_BACKEND_NO_ERR)
    {
        std::fprintf (stderr, "webd: cannot load %s: %s\n", args->book,
                      qof_session_get_error_message (session));
        exit (1);
    }

    QofBook* book = qof_session_get_book (session);
    g_server.root = gnc_book_get_root_account (book);
    g_server.accounts = g_server.root ? gnc_account_n_descendants (g_server.root) : 0;
    g_server.top_level = g_server.root
        ? static_cast<int> (g_list_length (gnc_account_get_children (g_server.root)))
        : 0;
    g_server.token = random_token ();

    GSocketService* service = g_socket_service_new ();
    GInetAddress* loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
    GSocketAddress* address = g_inet_socket_address_new (loopback, 0);
    GError* error = nullptr;
    guint16 port = 0;

    GSocketAddress* effective = nullptr;
    if (!g_socket_listener_add_address (
            G_SOCKET_LISTENER (service), address, G_SOCKET_TYPE_STREAM,
            G_SOCKET_PROTOCOL_TCP, nullptr, &effective, &error))
    {
        std::fprintf (stderr, "webd: cannot listen: %s\n", error->message);
        exit (1);
    }

    port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (effective));
    g_object_unref (effective);
    g_object_unref (address);
    g_object_unref (loopback);

    g_signal_connect (service, "incoming", G_CALLBACK (on_incoming), nullptr);
    g_socket_service_start (service);

    std::printf ("webd listening on http://127.0.0.1:%u/%s/\n", port,
                 g_server.token.c_str ());
    std::fflush (stdout);

    g_main_loop_run (g_main_loop_new (nullptr, FALSE));
}

} // namespace

int
main (int argc, char** argv)
{
    Arguments args;

    for (int i = 1; i < argc; ++i)
    {
        if (g_strcmp0 (argv[i], "--backend") == 0 && i + 1 < argc)
            args.backend = argv[++i];
        else if (args.book == nullptr)
            args.book = argv[i];
    }

    if (args.book == nullptr)
    {
        std::fprintf (stderr,
                      "usage: %s [--backend <backend-module>] <book>\n", argv[0]);
        return 2;
    }

    /* Guile owns the main thread; everything above happens inside this. */
    scm_boot_guile (argc, argv, webd_main, &args);
    return 0;
}

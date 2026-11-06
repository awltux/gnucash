// Event-loop bridge spike for the GnuCash Qt 6 presentation tier.
//
// Proves the two directions that Phase 1 depends on:
//   GLib -> Qt : a GLib timeout on a dedicated GMainContext marshals a value
//                into the Qt thread with QMetaObject::invokeMethod.
//   Qt -> GLib : a QTimer invokes work on the GLib context with
//                g_main_context_invoke.
//
// Deliberately tiny, and deliberately a separate program: it is the experiment
// recorded in doc/adr/0001-qt6-presentation-tier.md, not application code.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <glib.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>

namespace
{
struct Counters
{
    std::atomic<int> glib_ticks{0};
    std::atomic<int> delivered_to_qt{0};
    std::atomic<int> delivered_to_glib{0};
    std::atomic<bool> stop{false};
};

gboolean glib_tick(gpointer data)
{
    auto* counters = static_cast<Counters*>(data);
    counters->glib_ticks.fetch_add(1);

    const int value = counters->glib_ticks.load();
    // Deliver into the Qt thread without needing a Q_OBJECT moc pass.
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [counters, value]() { counters->delivered_to_qt.store(value); },
        Qt::QueuedConnection);
    return G_SOURCE_CONTINUE;
}

gboolean glib_work(gpointer data)
{
    static_cast<Counters*>(data)->delivered_to_glib.fetch_add(1);
    return G_SOURCE_REMOVE;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Counters counters;

    GMainContext* context = g_main_context_new();

    std::thread glib_thread([&]() {
        g_main_context_push_thread_default(context);

        // Attach the timeout to *this* context explicitly. g_timeout_add()
        // attaches to the global default context, which nothing iterates
        // here: that version ticked on Linux and delivered zero ticks on
        // Windows, which is exactly the kind of bug a spike is for.
        GSource* tick = g_timeout_source_new(10);
        g_source_set_callback(tick, glib_tick, &counters, nullptr);
        g_source_attach(tick, context);
        g_source_unref(tick);

        while (!counters.stop.load())
            g_main_context_iteration(context, TRUE);
        g_main_context_pop_thread_default(context);
    });

    // Qt -> GLib, once per event-loop turn.
    QTimer qt_timer;
    QObject::connect(&qt_timer, &QTimer::timeout, [&]() {
        g_main_context_invoke(context, glib_work, &counters);
    });
    qt_timer.start(10);

    // Give both directions half a second of traffic, then shut down.
    QTimer::singleShot(500, &app, [&]() {
        counters.stop.store(true);
        g_main_context_wakeup(context);
        app.quit();
    });

    app.exec();

    glib_thread.join();
    g_main_context_unref(context);

    std::printf("glib ticks:      %d\n", counters.glib_ticks.load());
    std::printf("glib -> qt:      %d\n", counters.delivered_to_qt.load());
    std::printf("qt -> glib:      %d\n", counters.delivered_to_glib.load());

    const bool ok = counters.glib_ticks.load() >= 5
                    && counters.delivered_to_qt.load() >= 5
                    && counters.delivered_to_glib.load() >= 5;
    std::printf("%s\n", ok ? "BRIDGE OK" : "BRIDGE FAILED");
    return ok ? 0 : 1;
}

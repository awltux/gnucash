// Qt report-pane spike: render an HTML file produced elsewhere in QWebEngineView.
//
// The point is the pane, not the renderer's feature list: does a Qt 6
// application host QWebEngineView headlessly, load a document produced by the
// Guile side, and read it back? That is the shape Phase 1 needs.

#include <cstdio>

#include <QApplication>
#include <QFile>
#include <QString>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <html-file>\n", argv[0]);
        return 2;
    }

    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::ReadOnly))
    {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    const QByteArray html = file.readAll();

    QWebEngineView view;
    view.resize(1024, 768);

    QObject::connect(&view, &QWebEngineView::loadFinished, [&](bool ok) {
        view.page()->toHtml([&, ok](const QString& rendered) {
            std::printf("load ok=%d input_bytes=%d rendered_bytes=%d\n",
                        ok ? 1 : 0, static_cast<int>(html.size()),
                        static_cast<int>(rendered.size()));
            app.exit(ok && !rendered.isEmpty() ? 0 : 1);
        });
    });

    QTimer::singleShot(15000, &app, [&]() {
        std::fprintf(stderr, "timeout waiting for render\n");
        app.exit(3);
    });

    view.setHtml(QString::fromUtf8(html));
    view.show();

    return app.exec();
}

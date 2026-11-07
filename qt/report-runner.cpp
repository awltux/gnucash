#include "report-runner.h"

#include <cstdio>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

namespace gncqt
{

QString ReportRunner::build_directory()
{
    // The binary lives in <build>/bin, and everything it passes to the helper
    // (libraries, modules, scheme code) is rooted at <build>.
    const QFileInfo application(QCoreApplication::applicationDirPath());
    return application.dir().absolutePath();
}

QString ReportRunner::guile_ccache_directory(const QString& build_dir)
{
    // <build>/lib/<multiarch>/guile/3.0/site-ccache — the multiarch triple
    // varies by distribution, so it is discovered rather than assumed.
    const QDir lib_root(build_dir + QStringLiteral("/lib"));
    const auto entries = lib_root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries)
    {
        const QString candidate =
            lib_root.absolutePath() + QLatin1Char('/') + entry
            + QStringLiteral("/guile/3.0/site-ccache");
        if (QDir(candidate).exists())
            return candidate;
    }
    return {};
}

QStringList ReportRunner::list_reports() const
{
    const QString build_dir = build_directory();
    const QString cli = build_dir + QStringLiteral("/bin/gnucash-cli");
    if (!QFileInfo::exists(cli))
        return {};

    QProcess process;
    process.setProcessEnvironment(helper_environment(build_dir));
    process.start(cli, QStringList{QStringLiteral("-R"), QStringLiteral("list")});
    if (!process.waitForStarted(10000) || !process.waitForFinished(60000))
    {
        process.kill();
        return {};
    }

    QStringList reports;
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    for (const QString& line : output.split(QLatin1Char('\n')))
    {
        QString trimmed = line.trimmed();
        if (!trimmed.startsWith(QLatin1Char('*')))
            continue;
        trimmed.remove(0, 1);
        trimmed = trimmed.trimmed();
        if (!trimmed.isEmpty())
            reports.append(trimmed);
    }
    return reports;
}

QProcessEnvironment ReportRunner::helper_environment(const QString& build_dir)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LD_LIBRARY_PATH"),
               build_dir + QStringLiteral("/lib:") + build_dir
                   + QStringLiteral("/lib/gnucash"));
    env.insert(QStringLiteral("GNC_MODULE_PATH"),
               build_dir + QStringLiteral("/lib/gnucash"));
    env.insert(QStringLiteral("GUILE_LOAD_PATH"),
               build_dir + QStringLiteral("/share/guile/site/3.0"));
    env.insert(QStringLiteral("GSETTINGS_SCHEMA_DIR"),
               build_dir + QStringLiteral("/share/glib-2.0/schemas"));
    const QString ccache = guile_ccache_directory(build_dir);
    if (!ccache.isEmpty())
        env.insert(QStringLiteral("GUILE_LOAD_COMPILED_PATH"), ccache);
    return env;
}

bool ReportRunner::render(const QString& book_path, const QString& report_name,
                          const QString& output_path) const
{
    const QString build_dir = build_directory();
    const QString cli = build_dir + QStringLiteral("/bin/gnucash-cli");
    if (!QFileInfo::exists(cli))
    {
        std::fprintf(stderr, "gnucash-cli not found at %s\n", qPrintable(cli));
        return false;
    }

    QProcess process;
    process.setProcessEnvironment(helper_environment(build_dir));
    process.start(cli, QStringList{QStringLiteral("-R"), QStringLiteral("run"),
                                   QStringLiteral("--name"), report_name,
                                   QStringLiteral("--output-file"), output_path,
                                   book_path});
    if (!process.waitForStarted(10000))
    {
        std::fprintf(stderr, "gnucash-cli failed to start\n");
        return false;
    }
    if (!process.waitForFinished(180000))
    {
        process.kill();
        std::fprintf(stderr, "gnucash-cli timed out\n");
        return false;
    }

    const QFileInfo output(output_path);
    return process.exitStatus() == QProcess::NormalExit && output.exists()
           && output.size() > 0;
}

} // namespace gncqt

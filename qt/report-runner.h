// The only place that knows how report HTML is produced (ADR 0015).
//
// Today it drives gnucash-cli, because gnc-report links GTK3 through gnc-html
// and must not enter this process. When the report layer is GTK-free (Phase 6)
// this class gains an in-process implementation and the rest of the shell does
// not change.

#ifndef GNC_QT_REPORT_RUNNER_H
#define GNC_QT_REPORT_RUNNER_H

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace gncqt
{

class ReportRunner
{
public:
    // Renders `report_name` for `book_path` into `output_path`. Returns false
    // and leaves `output_path` absent or empty if the report could not be
    // produced; the caller owns the error message.
    bool render(const QString& book_path, const QString& report_name,
                const QString& output_path) const;

    // The report names this build can produce. Empty if the helper cannot be
    // asked, which the caller should treat as "no report menu".
    QStringList list_reports() const;

private:
    static QString build_directory();
    static QString guile_ccache_directory(const QString& build_dir);
    static QProcessEnvironment helper_environment(const QString& build_dir);
};

} // namespace gncqt

#endif

// A small preferences dialog over existing GSettings keys (ADR 0013).
//
// Every control writes a key that already ships in the schemas; there is no
// Qt-only preference store. Controls the dialog cannot honour are shown
// disabled rather than silently overwritten: the accounting period may be set
// to absolute dates in GnuCash's own preferences, and this dialog only edits
// the relative form.

#ifndef GNC_QT_PREFERENCES_DIALOG_H
#define GNC_QT_PREFERENCES_DIALOG_H

#include <QDialog>

class QCheckBox;
class QComboBox;

namespace gncqt
{

class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PreferencesDialog(QWidget* parent = nullptr);

    bool negative_in_red() const;
    void set_negative_in_red(bool enabled);

    int start_period() const;
    void set_start_period(int period);
    int end_period() const;
    void set_end_period(int period);

    bool start_is_absolute() const { return start_absolute_; }
    bool end_is_absolute() const { return end_absolute_; }

    // Writes the enabled controls to GSettings.
    // @return true if at least one key changed.
    bool apply();

private:
    void load();

    QCheckBox* negative_in_red_ = nullptr;
    QComboBox* start_period_ = nullptr;
    QComboBox* end_period_ = nullptr;
    bool start_absolute_ = false;
    bool end_absolute_ = false;
};

} // namespace gncqt

#endif

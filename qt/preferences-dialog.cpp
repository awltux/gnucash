#include "preferences-dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>

extern "C" {
#include <gnc-gsettings.h>
#include <gnc-prefs.h>
}

namespace
{

// The labels GnuCash uses for the relative accounting periods
// (gnc-period-select.c). Index is the GNC_ACCOUNTING_PERIOD_* value.
const char* const kStartLabels[] = {
    "Today",
    "Start of this month",
    "Start of previous month",
    "Start of this quarter",
    "Start of previous quarter",
    "Start of this year",
    "Start of previous year",
    "Start of this accounting period",
    "Start of previous accounting period"
};

const char* const kEndLabels[] = {
    "Today",
    "End of this month",
    "End of previous month",
    "End of this quarter",
    "End of previous quarter",
    "End of this year",
    "End of previous year",
    "End of this accounting period",
    "End of previous accounting period"
};

constexpr int kPeriodCount = static_cast<int>(sizeof(kStartLabels) / sizeof(kStartLabels[0]));

int clamp_period(int period)
{
    if (period < 0)
        return 0;
    if (period >= kPeriodCount)
        return kPeriodCount - 1;
    return period;
}

} // namespace

namespace gncqt
{

PreferencesDialog::PreferencesDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Preferences"));

    negative_in_red_ = new QCheckBox(QStringLiteral("Show negative totals in red"));

    start_period_ = new QComboBox;
    end_period_ = new QComboBox;
    for (int i = 0; i < kPeriodCount; ++i)
    {
        start_period_->addItem(QString::fromUtf8(kStartLabels[i]));
        end_period_->addItem(QString::fromUtf8(kEndLabels[i]));
    }

    auto* form = new QFormLayout;
    form->addRow(negative_in_red_);
    form->addRow(QStringLiteral("Accounting period starts"), start_period_);
    form->addRow(QStringLiteral("Accounting period ends"), end_period_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                         | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        apply();
        accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    load();
}

void PreferencesDialog::load()
{
    negative_in_red_->setChecked(
        gnc_gsettings_get_bool(GNC_PREFS_GROUP_GENERAL, GNC_PREF_NEGATIVE_IN_RED));

    start_absolute_ = gnc_gsettings_get_bool(GNC_PREFS_GROUP_ACCT_SUMMARY,
                                             GNC_PREF_START_CHOICE_ABS);
    end_absolute_ = gnc_gsettings_get_bool(GNC_PREFS_GROUP_ACCT_SUMMARY,
                                           GNC_PREF_END_CHOICE_ABS);

    start_period_->setCurrentIndex(clamp_period(
        gnc_gsettings_get_int(GNC_PREFS_GROUP_ACCT_SUMMARY, GNC_PREF_START_PERIOD)));
    end_period_->setCurrentIndex(clamp_period(
        gnc_gsettings_get_int(GNC_PREFS_GROUP_ACCT_SUMMARY, GNC_PREF_END_PERIOD)));

    // An absolute period belongs to GnuCash's own preferences; do not let this
    // dialog quietly turn it into a relative one.
    start_period_->setEnabled(!start_absolute_);
    end_period_->setEnabled(!end_absolute_);
    if (start_absolute_ || end_absolute_)
    {
        const QString note = QStringLiteral(
            "The accounting period is set to absolute dates in GnuCash's "
            "preferences, so those choices are disabled here.");
        start_period_->setToolTip(note);
        end_period_->setToolTip(note);
    }
}

bool PreferencesDialog::negative_in_red() const
{
    return negative_in_red_->isChecked();
}

void PreferencesDialog::set_negative_in_red(bool enabled)
{
    negative_in_red_->setChecked(enabled);
}

int PreferencesDialog::start_period() const
{
    return start_period_->currentIndex();
}

void PreferencesDialog::set_start_period(int period)
{
    start_period_->setCurrentIndex(clamp_period(period));
}

int PreferencesDialog::end_period() const
{
    return end_period_->currentIndex();
}

void PreferencesDialog::set_end_period(int period)
{
    end_period_->setCurrentIndex(clamp_period(period));
}

bool PreferencesDialog::apply()
{
    bool wrote = false;

    const bool red = negative_in_red_->isChecked();
    if (red != gnc_gsettings_get_bool(GNC_PREFS_GROUP_GENERAL,
                                      GNC_PREF_NEGATIVE_IN_RED))
    {
        gnc_gsettings_set_bool(GNC_PREFS_GROUP_GENERAL, GNC_PREF_NEGATIVE_IN_RED, red);
        wrote = true;
    }

    if (!start_absolute_)
    {
        const int period = start_period_->currentIndex();
        if (period != gnc_gsettings_get_int(GNC_PREFS_GROUP_ACCT_SUMMARY,
                                            GNC_PREF_START_PERIOD))
        {
            gnc_gsettings_set_int(GNC_PREFS_GROUP_ACCT_SUMMARY,
                                  GNC_PREF_START_PERIOD, period);
            wrote = true;
        }
    }

    if (!end_absolute_)
    {
        const int period = end_period_->currentIndex();
        if (period != gnc_gsettings_get_int(GNC_PREFS_GROUP_ACCT_SUMMARY,
                                            GNC_PREF_END_PERIOD))
        {
            gnc_gsettings_set_int(GNC_PREFS_GROUP_ACCT_SUMMARY,
                                  GNC_PREF_END_PERIOD, period);
            wrote = true;
        }
    }

    return wrote;
}

} // namespace gncqt

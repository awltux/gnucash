// Account tree as a model over engine objects (ADR 0014).
//
// Items are references to accounts resolved through the engine; children are
// fetched lazily. Edits produce targeted signals — dataChanged for a rename,
// beginInsertRows/beginRemoveRows for structural changes — never a model reset.
//
// Accounts can be destroyed and their memory reused, so the caches are purged
// per removed subtree rather than left to go stale.

#ifndef GNC_QT_ACCOUNT_MODEL_H
#define GNC_QT_ACCOUNT_MODEL_H

#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include <QString>

extern "C" {
#include <Account.h>
}

namespace gncqt
{

class AccountModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Column
    {
        NameColumn = 0,
        TotalColumn = 1
    };

    explicit AccountModel(QObject* parent = nullptr);

    // The root account whose children form the visible top level.
    void set_root_account(Account* root);

    // The account behind an index, or nullptr for the invisible root.
    Account* account_for(const QModelIndex& index) const;

    // Whether negative totals are drawn in red. The application reads the
    // preference (ADR 0013 rule 1) and tells the model, which keeps policy out
    // of the model.
    void set_negative_in_red(bool enabled);
    bool negative_in_red() const { return negative_in_red_; }

    // Engine notifications. Each emits the narrowest signal that is correct.
    void account_changed(Account* account);
    void account_added(Account* account);
    void account_removed(Account* account);

    // Counters for tests: how many structural signals have been emitted.
    int inserted_rows() const { return inserted_; }
    int removed_rows() const { return removed_; }

    QModelIndex index(int row, int column,
                      const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;

private:
    QList<Account*> children_of(Account* account) const;
    Account* parent_of(Account* account) const;
    QModelIndex index_of(Account* account) const;
    void forget_subtree(Account* account);

    // The account's total, formatted the way the GTK account tree formats it
    // (including children, in the account's commodity). Cached because the
    // view asks for it once per painted cell; the cache is dropped on any
    // account event.
    struct Total
    {
        QString text;
        bool negative = false;
    };

    Total total_for(Account* account) const;

    Account* root_ = nullptr;
    mutable QHash<Account*, QList<Account*>> children_;
    mutable QHash<Account*, Account*> parents_;
    mutable QHash<Account*, Total> totals_;
    bool negative_in_red_ = false;
    int inserted_ = 0;
    int removed_ = 0;
};

} // namespace gncqt

#endif

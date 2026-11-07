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

extern "C" {
#include <Account.h>
}

namespace gncqt
{

class AccountModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    explicit AccountModel(QObject* parent = nullptr);

    // The root account whose children form the visible top level.
    void set_root_account(Account* root);

    // The account behind an index, or nullptr for the invisible root.
    Account* account_for(const QModelIndex& index) const;

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

    Account* root_ = nullptr;
    mutable QHash<Account*, QList<Account*>> children_;
    mutable QHash<Account*, Account*> parents_;
    int inserted_ = 0;
    int removed_ = 0;
};

} // namespace gncqt

#endif

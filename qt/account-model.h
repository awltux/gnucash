// Account tree as a model over engine objects (ADR 0014).
//
// Items are references to accounts resolved through the engine; children are
// fetched lazily, and an event produces a targeted dataChanged rather than a
// model reset. Accounts can be destroyed, so a pointer is never trusted beyond
// the call that produced it without a parent lookup.

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

    // Emits dataChanged for one account (a rename), and nothing else.
    void account_changed(Account* account);

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

    Account* root_ = nullptr;
    mutable QHash<Account*, QList<Account*>> children_;
    mutable QHash<Account*, Account*> parents_;
};

} // namespace gncqt

#endif

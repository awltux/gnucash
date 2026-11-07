#include "account-model.h"

#include <QVariant>

extern "C" {
#include <gnc-engine.h>
}

namespace gncqt
{

AccountModel::AccountModel(QObject* parent) : QAbstractItemModel(parent) {}

void AccountModel::set_root_account(Account* root)
{
    beginResetModel();
    root_ = root;
    children_.clear();
    parents_.clear();
    inserted_ = 0;
    removed_ = 0;
    endResetModel();
}

Account* AccountModel::account_for(const QModelIndex& index) const
{
    if (!index.isValid())
        return root_;
    return static_cast<Account*>(index.internalPointer());
}

QList<Account*> AccountModel::children_of(Account* account) const
{
    if (account == nullptr)
        return {};

    const auto cached = children_.constFind(account);
    if (cached != children_.constEnd())
        return *cached;

    QList<Account*> result;
    GList* children = gnc_account_get_children(account);
    for (GList* node = children; node != nullptr; node = node->next)
    {
        auto* child = static_cast<Account*>(node->data);
        result.append(child);
        parents_.insert(child, account);
    }
    g_list_free(children);

    children_.insert(account, result);
    return result;
}

Account* AccountModel::parent_of(Account* account) const
{
    // Every index this model hands out was produced by enumerating its parent's
    // children, so the reverse map already knows the answer. Asking the engine
    // would also be wrong during a destroy, when the object is no longer valid.
    return parents_.value(account, nullptr);
}

QModelIndex AccountModel::index_of(Account* account) const
{
    if (account == nullptr || account == root_)
        return {};

    Account* parent = parent_of(account);
    if (parent == nullptr)
        return {};

    const auto siblings = children_of(parent);
    const int row = siblings.indexOf(account);
    if (row < 0)
        return {};

    return createIndex(row, 0, account);
}

QModelIndex AccountModel::index(int row, int column,
                                const QModelIndex& parent) const
{
    if (!hasIndex(row, column, parent))
        return {};

    Account* parent_account = account_for(parent);
    const auto siblings = children_of(parent_account);
    if (row < 0 || row >= siblings.size())
        return {};

    return createIndex(row, column, siblings.at(row));
}

QModelIndex AccountModel::parent(const QModelIndex& index) const
{
    if (!index.isValid())
        return {};

    return index_of(static_cast<Account*>(index.internalPointer()));
}

int AccountModel::rowCount(const QModelIndex& parent) const
{
    if (parent.column() > 0)
        return 0;
    return static_cast<int>(children_of(account_for(parent)).size());
}

int AccountModel::columnCount(const QModelIndex&) const
{
    return 1;
}

QVariant AccountModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || role != Qt::DisplayRole)
        return {};

    auto* account = static_cast<Account*>(index.internalPointer());
    if (account == nullptr)
        return {};

    const char* name = xaccAccountGetName(account);
    return QString::fromUtf8(name ? name : "");
}

QVariant AccountModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section != 0)
        return {};
    return QStringLiteral("Account");
}

void AccountModel::account_changed(Account* account)
{
    const QModelIndex changed = index_of(account);
    if (!changed.isValid())
        return;
    emit dataChanged(changed, changed, {Qt::DisplayRole});
}

void AccountModel::account_added(Account* account)
{
    if (account == nullptr)
        return;

    // The event fires after the child was linked, so the engine can still be
    // asked for the parent here.
    Account* parent = gnc_account_get_parent(account);
    if (parent == nullptr)
        return;

    // Ignore branches that have not been materialised: the view has no parent
    // index for them, and the next fetch will pick the child up.
    const auto known = children_.constFind(parent);
    if (known == children_.constEnd())
        return;

    const int row = static_cast<int>(known->size());
    const QModelIndex parent_index = index_of(parent);
    beginInsertRows(parent_index, row, row);
    children_[parent].append(account);
    parents_.insert(account, parent);
    ++inserted_;
    endInsertRows();
}

void AccountModel::account_removed(Account* account)
{
    if (account == nullptr)
        return;

    Account* parent = parent_of(account);
    if (parent == nullptr)
        return;

    const auto known = children_.constFind(parent);
    if (known == children_.constEnd())
        return;

    const int row = static_cast<int>(known->indexOf(account));
    if (row < 0)
        return;

    const QModelIndex parent_index = index_of(parent);
    beginRemoveRows(parent_index, row, row);
    children_[parent].removeAt(row);
    forget_subtree(account);
    ++removed_;
    endRemoveRows();
}

void AccountModel::forget_subtree(Account* account)
{
    const auto known = children_.constFind(account);
    if (known != children_.constEnd())
    {
        const auto descendants = *known;
        for (Account* child : descendants)
            forget_subtree(child);
        children_.remove(account);
    }
    parents_.remove(account);
}

} // namespace gncqt

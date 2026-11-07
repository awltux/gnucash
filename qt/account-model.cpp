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
    // A miss means the account has not been visited yet; fetch its parent's
    // children so the reverse map is populated for this branch.
    if (!parents_.contains(account) && account != nullptr)
    {
        Account* parent = gnc_account_get_parent(account);
        if (parent != nullptr)
        {
            children_of(parent);
            if (!parents_.contains(account))
                parents_.insert(account, parent);
        }
    }
    return parents_.value(account, nullptr);
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

    Account* account = account_for(index);
    Account* parent = parent_of(account);
    if (parent == nullptr || parent == root_)
        return {};

    Account* grandparent = parent_of(parent);
    const auto siblings = children_of(grandparent);
    const int row = siblings.indexOf(parent);
    if (row < 0)
        return {};

    return createIndex(row, 0, parent);
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
    if (!index.isValid())
        return {};

    if (role != Qt::DisplayRole)
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
    if (account == nullptr)
        return;

    Account* parent = parent_of(account);
    if (parent == nullptr)
        return;

    const auto siblings = children_of(parent);
    const int row = siblings.indexOf(account);
    if (row < 0)
        return;

    const QModelIndex changed = createIndex(row, 0, account);
    emit dataChanged(changed, changed, {Qt::DisplayRole});
}

} // namespace gncqt

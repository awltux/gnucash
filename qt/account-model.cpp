#include "account-model.h"

#include <QBrush>
#include <QColor>
#include <QVariant>

extern "C" {
#include <gnc-accounting-period.h>
#include <gnc-engine.h>
#include <gnc-ui-balances.h>
#include <gnc-ui-util.h>
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
    totals_.clear();
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

void AccountModel::set_negative_in_red(bool enabled)
{
    negative_in_red_ = enabled;
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
    return 3;
}

AccountModel::Total AccountModel::total_for(Account* account) const
{
    Total total;
    if (account == nullptr)
        return total;

    const auto cached = totals_.constFind(account);
    if (cached != totals_.constEnd())
        return *cached;

    // The same helpers the GTK account tree uses, so the strings match: the
    // present balance including children, and the change over the current
    // accounting period, both in the account's own commodity.
    gboolean negative = FALSE;
    gchar* text = gnc_ui_account_get_print_balance(
        xaccAccountGetPresentBalanceInCurrency, account, TRUE, &negative);

    total.present = QString::fromUtf8(text != nullptr ? text : "");
    total.present_negative = negative;
    g_free(text);

    const time64 start = gnc_accounting_period_fiscal_start();
    const time64 end = gnc_accounting_period_fiscal_end();
    if (start <= end)
    {
        gnc_numeric change =
            xaccAccountGetBalanceChangeForPeriod(account, start, end, TRUE);
        if (gnc_reverse_balance(account))
            change = gnc_numeric_neg(change);

        total.period_negative = gnc_numeric_negative_p(change);
        total.period = QString::fromUtf8(gnc_print_amount_with_bidi_ltr_isolate(
            change, gnc_account_print_info(account, TRUE)));
    }

    totals_.insert(account, total);
    return total;
}

QVariant AccountModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return {};

    auto* account = static_cast<Account*>(index.internalPointer());
    if (account == nullptr)
        return {};

    if (index.column() == TotalColumn || index.column() == PeriodTotalColumn)
    {
        const bool period = index.column() == PeriodTotalColumn;
        const Total total = total_for(account);

        switch (role)
        {
        case Qt::DisplayRole:
            return period ? total.period : total.present;
        case Qt::ForegroundRole:
            if (negative_in_red_
                && (period ? total.period_negative : total.present_negative))
                return QBrush(QColor(0xc0, 0x00, 0x00));
            return {};
        case Qt::TextAlignmentRole:
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return {};
        }
    }

    if (role != Qt::DisplayRole)
        return {};

    const char* name = xaccAccountGetName(account);
    return QString::fromUtf8(name ? name : "");
}

QVariant AccountModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section)
    {
    case NameColumn:
        return QStringLiteral("Account");
    case TotalColumn:
        return QStringLiteral("Total");
    case PeriodTotalColumn:
        return QStringLiteral("Total (Period)");
    default:
        return {};
    }
}

void AccountModel::account_changed(Account* account)
{
    if (account == nullptr)
        return;

    // Any account change can move a total: this account's, and every
    // ancestor's, because a total includes its children. The cache is blunt
    // on purpose — the next paint recomputes what it needs.
    totals_.clear();

    const QModelIndex changed = index_of(account);
    if (changed.isValid())
    {
        emit dataChanged(createIndex(changed.row(), NameColumn, account),
                         createIndex(changed.row(), TotalColumn, account),
                         {Qt::DisplayRole, Qt::ForegroundRole});
    }

    for (Account* ancestor = parent_of(account);
         ancestor != nullptr && ancestor != root_;
         ancestor = parent_of(ancestor))
    {
        const QModelIndex row = index_of(ancestor);
        if (!row.isValid())
            continue;

        const QModelIndex total = createIndex(row.row(), TotalColumn, ancestor);
        emit dataChanged(total, total, {Qt::DisplayRole, Qt::ForegroundRole});
    }
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
    totals_.clear();
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
    totals_.clear();
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

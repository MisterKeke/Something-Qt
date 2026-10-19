#include "currency_rates_model.h"

#include <algorithm>
#include <utility>

namespace something::models {

CurrencyRatesModel::CurrencyRatesModel(QObject *parent) : QAbstractListModel(parent) {}
int CurrencyRatesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_visibleRows.size();
}
QVariant CurrencyRatesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visibleRows.size()) return {};
    const QVariantMap row = m_visibleRows.at(index.row()).toMap();
    switch (role) {
    case Qt::DisplayRole:
    case CodeRole: return row.value(QStringLiteral("code"));
    case RateRole: return row.value(QStringLiteral("rate"));
    case DateRole: return row.value(QStringLiteral("date"));
    default: return {};
    }
}
QHash<int, QByteArray> CurrencyRatesModel::roleNames() const
{
    return {{CodeRole, "code"}, {RateRole, "rate"}, {DateRole, "date"}};
}
void CurrencyRatesModel::setFilter(QString filter)
{
    filter = filter.trimmed().toUpper();
    if (m_filter == filter) return;
    m_filter = std::move(filter);
    rebuild();
    emit filterChanged();
}
void CurrencyRatesModel::replace(QVariantList rows)
{
    std::sort(rows.begin(), rows.end(), [](const QVariant &left, const QVariant &right) {
        return left.toMap().value(QStringLiteral("code")).toString()
            < right.toMap().value(QStringLiteral("code")).toString();
    });
    if (rows == m_allRows) return;
    const int oldCount = m_visibleRows.size();
    const int oldTotalCount = m_allRows.size();
    beginResetModel();
    m_allRows = std::move(rows);
    m_visibleRows.clear();
    for (const QVariant &row : std::as_const(m_allRows)) {
        const QString code = row.toMap().value(QStringLiteral("code")).toString();
        if (m_filter.isEmpty() || code.contains(m_filter)) m_visibleRows.push_back(row);
    }
    endResetModel();
    if (oldCount != m_visibleRows.size() || oldTotalCount != m_allRows.size())
        emit countChanged();
}
void CurrencyRatesModel::clear()
{
    if (m_allRows.isEmpty() && m_visibleRows.isEmpty()) return;
    beginResetModel();
    m_allRows.clear();
    m_visibleRows.clear();
    endResetModel();
    emit countChanged();
}
void CurrencyRatesModel::rebuild()
{
    const int oldCount = m_visibleRows.size();
    beginResetModel();
    m_visibleRows.clear();
    for (const QVariant &row : std::as_const(m_allRows)) {
        const QString code = row.toMap().value(QStringLiteral("code")).toString();
        if (m_filter.isEmpty() || code.contains(m_filter)) m_visibleRows.push_back(row);
    }
    endResetModel();
    if (oldCount != m_visibleRows.size()) emit countChanged();
}

CurrencyFavoritesModel::CurrencyFavoritesModel(QObject *parent) : QAbstractListModel(parent) {}
int CurrencyFavoritesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}
QVariant CurrencyFavoritesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return {};
    const QVariantMap row = m_rows.at(index.row()).toMap();
    switch (role) {
    case Qt::DisplayRole:
    case PairCodeRole: return row.value(QStringLiteral("code"));
    case BaseCodeRole: return row.value(QStringLiteral("baseCode"));
    case QuoteCodeRole: return row.value(QStringLiteral("quoteCode"));
    case RateFoundRole: return row.value(QStringLiteral("found"));
    case RateRole: return row.value(QStringLiteral("rate"));
    case RateDateRole: return row.value(QStringLiteral("date"));
    case RateStatusRole: return row.value(QStringLiteral("rateStatus"));
    case ErrorMessageRole: return row.value(QStringLiteral("rateError"));
    default: return {};
    }
}
QHash<int, QByteArray> CurrencyFavoritesModel::roleNames() const
{
    return {{BaseCodeRole, "baseCode"}, {QuoteCodeRole, "quoteCode"},
            {PairCodeRole, "code"}, {RateFoundRole, "found"}, {RateRole, "rate"},
            {RateDateRole, "date"}, {RateStatusRole, "rateStatus"},
            {ErrorMessageRole, "rateError"}};
}
QVariantMap CurrencyFavoritesModel::rowAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row).toMap() : QVariantMap{};
}
bool CurrencyFavoritesModel::contains(const QString &base, const QString &quote) const
{
    return std::any_of(m_rows.cbegin(), m_rows.cend(), [&](const QVariant &item) {
        const QVariantMap row = item.toMap();
        return row.value(QStringLiteral("baseCode")).toString() == base
            && row.value(QStringLiteral("quoteCode")).toString() == quote;
    });
}
void CurrencyFavoritesModel::replace(QVariantList rows)
{
    std::sort(rows.begin(), rows.end(), [](const QVariant &left, const QVariant &right) {
        const QVariantMap a = left.toMap(), b = right.toMap();
        const QString aBase = a.value(QStringLiteral("baseCode")).toString();
        const QString bBase = b.value(QStringLiteral("baseCode")).toString();
        if (aBase != bBase) return aBase < bBase;
        return a.value(QStringLiteral("quoteCode")).toString()
            < b.value(QStringLiteral("quoteCode")).toString();
    });
    if (rows == m_rows) return;
    const int oldCount = m_rows.size();
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    if (oldCount != m_rows.size()) emit countChanged();
}
void CurrencyFavoritesModel::setRate(const QString &base, const QString &quote, bool found,
                                    double rate, QString date, QString status,
                                    QString errorMessage)
{
    for (int row = 0; row < m_rows.size(); ++row) {
        QVariantMap values = m_rows.at(row).toMap();
        if (values.value(QStringLiteral("baseCode")).toString() != base
            || values.value(QStringLiteral("quoteCode")).toString() != quote) continue;
        values.insert(QStringLiteral("found"), found);
        values.remove(QStringLiteral("rate"));
        if (found) values.insert(QStringLiteral("rate"), rate);
        values.insert(QStringLiteral("date"), std::move(date));
        values.insert(QStringLiteral("rateStatus"), std::move(status));
        values.insert(QStringLiteral("rateError"), std::move(errorMessage));
        m_rows[row] = values;
        emit dataChanged(index(row), index(row),
                         {RateFoundRole, RateRole, RateDateRole, RateStatusRole, ErrorMessageRole});
        return;
    }
}
void CurrencyFavoritesModel::markAll(QString status, QString errorMessage)
{
    for (int row = 0; row < m_rows.size(); ++row) {
        QVariantMap values = m_rows.at(row).toMap();
        values.insert(QStringLiteral("found"), false);
        values.remove(QStringLiteral("rate"));
        values.remove(QStringLiteral("date"));
        values.insert(QStringLiteral("rateStatus"), status);
        if (status == QStringLiteral("error") && !errorMessage.isEmpty())
            values.insert(QStringLiteral("rateError"), errorMessage);
        else
            values.remove(QStringLiteral("rateError"));
        m_rows[row] = values;
        emit dataChanged(index(row), index(row),
                         {RateFoundRole, RateRole, RateDateRole, RateStatusRole, ErrorMessageRole});
    }
}

} // namespace something::models

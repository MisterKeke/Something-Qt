#include "steam_games_model.h"

#include "../domain/steam_game.h"

#include <algorithm>
#include <utility>

namespace something::models {

SteamGamesModel::SteamGamesModel(QObject *parent) : QAbstractListModel(parent) {}

int SteamGamesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_games.size();
}

QVariant SteamGamesModel::roleValue(const QVariantMap &game, int role)
{
    switch (role) {
    case GameIdRole: return game.value(QStringLiteral("id"));
    case SteamAppIdRole: return game.value(QStringLiteral("steamAppId"));
    case StoreUrlRole: return game.value(QStringLiteral("storeUrl"));
    case NameRole: return game.value(QStringLiteral("name"));
    case ArtworkSourceRole: return game.value(QStringLiteral("artworkSource"));
    case PriceStatusRole: return game.value(QStringLiteral("priceStatus"));
    case CurrencyRole: return game.value(QStringLiteral("currency"));
    case HasRegularPriceRole: return game.contains(QStringLiteral("regularPriceMinor"));
    case RegularPriceMinorRole: return game.value(QStringLiteral("regularPriceMinor"));
    case HasCurrentPriceRole: return game.contains(QStringLiteral("currentPriceMinor"));
    case CurrentPriceMinorRole: return game.value(QStringLiteral("currentPriceMinor"));
    case PriceCountryCodeRole: return game.value(QStringLiteral("priceCountryCode"));
    case LastCheckedAtRole: return game.value(QStringLiteral("lastCheckedAt"));
    case LastAttemptedAtRole: return game.value(QStringLiteral("lastAttemptedAt"));
    case LastRefreshErrorRole: return game.value(QStringLiteral("lastRefreshError"));
    case UpdatedAtRole: return game.value(QStringLiteral("updatedAt"));
    case PriceStateLabelRole: {
        const QString status = game.value(QStringLiteral("priceStatus")).toString();
        if (status == QStringLiteral("free")) return QStringLiteral("Free");
        if (status == QStringLiteral("unavailable")) return QStringLiteral("Unavailable");
        if (status == QStringLiteral("unsupported"))
            return QStringLiteral("No supported pricing");
        if (status != QStringLiteral("priced")
            || !game.contains(QStringLiteral("currentPriceMinor"))
            || game.value(QStringLiteral("currency")).toString().isEmpty())
            return QStringLiteral("Price missing");
        return QStringLiteral("Paid price");
    }
    case PriceDisplayRole: {
        if (game.value(QStringLiteral("priceStatus")).toString() == QStringLiteral("free"))
            return QStringLiteral("Free");
        if (game.value(QStringLiteral("priceStatus")).toString() != QStringLiteral("priced")
            || !game.contains(QStringLiteral("currentPriceMinor"))) return {};
        const auto amount = game.value(QStringLiteral("currentPriceMinor")).toLongLong();
        const QString currency = game.value(QStringLiteral("currency")).toString();
        return domain::formatSteamPrice(amount, currency);
    }
    case RegularPriceDisplayRole: {
        if (!game.contains(QStringLiteral("regularPriceMinor"))) return {};
        return domain::formatSteamPrice(
            game.value(QStringLiteral("regularPriceMinor")).toLongLong(),
            game.value(QStringLiteral("currency")).toString());
    }
    case DiscountPercentRole: {
        if (game.value(QStringLiteral("priceStatus")).toString() != QStringLiteral("priced")
            || !game.contains(QStringLiteral("regularPriceMinor"))
            || !game.contains(QStringLiteral("currentPriceMinor"))) return 0;
        return domain::effectiveSteamDiscount(
            game.value(QStringLiteral("regularPriceMinor")).toLongLong(),
            game.value(QStringLiteral("currentPriceMinor")).toLongLong(),
            game.value(QStringLiteral("discountPercent")).toInt());
    }
    default: return {};
    }
}

QVariant SteamGamesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_games.size()) return {};
    return roleValue(m_games.at(index.row()), role);
}

QHash<int, QByteArray> SteamGamesModel::roleNames() const
{
    return {{GameIdRole, "gameId"}, {SteamAppIdRole, "steamAppId"},
            {StoreUrlRole, "storeUrl"}, {NameRole, "name"},
            {ArtworkSourceRole, "artworkSource"}, {PriceStatusRole, "priceStatus"},
            {PriceStateLabelRole, "priceStateLabel"}, {CurrencyRole, "currency"},
            {HasRegularPriceRole, "hasRegularPrice"},
            {RegularPriceMinorRole, "regularPriceMinor"},
            {HasCurrentPriceRole, "hasCurrentPrice"},
            {CurrentPriceMinorRole, "currentPriceMinor"},
            {PriceDisplayRole, "priceDisplay"},
            {RegularPriceDisplayRole, "regularPriceDisplay"},
            {DiscountPercentRole, "discountPercent"},
            {PriceCountryCodeRole, "priceCountryCode"},
            {LastCheckedAtRole, "lastCheckedAt"},
            {LastAttemptedAtRole, "lastAttemptedAt"},
            {LastRefreshErrorRole, "lastRefreshError"}, {UpdatedAtRole, "updatedAt"}};
}

QVariantMap SteamGamesModel::rowAt(int row) const
{
    return row >= 0 && row < m_games.size() ? m_games.at(row) : QVariantMap{};
}

int SteamGamesModel::rowForId(qint64 id) const
{
    for (int row = 0; row < m_games.size(); ++row)
        if (m_games.at(row).value(QStringLiteral("id")).toLongLong() == id) return row;
    return -1;
}

void SteamGamesModel::replace(QVariantList games)
{
    QVector<QVariantMap> rows;
    rows.reserve(games.size());
    for (const QVariant &game : games) rows.push_back(game.toMap());
    std::sort(rows.begin(), rows.end(), [](const QVariantMap &left, const QVariantMap &right) {
        const int byName = QString::compare(left.value(QStringLiteral("name")).toString(),
                                            right.value(QStringLiteral("name")).toString(),
                                            Qt::CaseInsensitive);
        if (byName != 0) return byName < 0;
        return left.value(QStringLiteral("id")).toLongLong()
            < right.value(QStringLiteral("id")).toLongLong();
    });
    if (rows.size() == m_games.size()) {
        bool sameIdentityOrder = true;
        for (int row = 0; row < rows.size(); ++row) {
            if (rows.at(row).value(QStringLiteral("id"))
                != m_games.at(row).value(QStringLiteral("id"))) {
                sameIdentityOrder = false;
                break;
            }
        }
        if (sameIdentityOrder) {
            for (int row = 0; row < rows.size(); ++row) {
                if (rows.at(row) == m_games.at(row)) continue;
                m_games[row] = std::move(rows[row]);
                emit dataChanged(index(row), index(row));
            }
            return;
        }
    }
    const int oldCount = m_games.size();
    beginResetModel();
    m_games = std::move(rows);
    endResetModel();
    if (oldCount != m_games.size()) emit countChanged();
}

void SteamGamesModel::append(QVariantMap game)
{
    const qint64 id = game.value(QStringLiteral("id")).toLongLong();
    if (rowForId(id) >= 0) {
        update(std::move(game));
        return;
    }
    int row = 0;
    while (row < m_games.size()) {
        const QVariantMap &existing = m_games.at(row);
        const int byName = QString::compare(existing.value(QStringLiteral("name")).toString(),
                                            game.value(QStringLiteral("name")).toString(),
                                            Qt::CaseInsensitive);
        if (byName > 0 || (byName == 0
            && existing.value(QStringLiteral("id")).toLongLong() > id)) break;
        ++row;
    }
    beginInsertRows({}, row, row);
    m_games.insert(row, std::move(game));
    endInsertRows();
    emit countChanged();
}

void SteamGamesModel::removeById(qint64 id)
{
    const int row = rowForId(id);
    if (row < 0) return;
    beginRemoveRows({}, row, row);
    m_games.removeAt(row);
    endRemoveRows();
    emit countChanged();
}

void SteamGamesModel::update(QVariantMap game)
{
    const int row = rowForId(game.value(QStringLiteral("id")).toLongLong());
    if (row < 0) {
        append(std::move(game));
        return;
    }
    if (m_games.at(row) == game) return;
    m_games[row] = std::move(game);
    emit dataChanged(index(row), index(row));
}

} // namespace something::models

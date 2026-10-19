#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QVector>
#include <QVariantMap>

namespace something::models {

class SteamGamesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { GameIdRole = Qt::UserRole + 1, SteamAppIdRole, StoreUrlRole, NameRole,
                ArtworkSourceRole, PriceStatusRole, PriceStateLabelRole, CurrencyRole,
                HasRegularPriceRole, RegularPriceMinorRole, HasCurrentPriceRole,
                CurrentPriceMinorRole, PriceDisplayRole, RegularPriceDisplayRole,
                DiscountPercentRole, PriceCountryCodeRole, LastCheckedAtRole,
                LastAttemptedAtRole, LastRefreshErrorRole, UpdatedAtRole };
    Q_ENUM(Role)

    explicit SteamGamesModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_games.size(); }
    Q_INVOKABLE QVariantMap rowAt(int row) const;
    Q_INVOKABLE int rowForId(qint64 id) const;
    Q_INVOKABLE void replace(QVariantList games);
    Q_INVOKABLE void append(QVariantMap game);
    Q_INVOKABLE void removeById(qint64 id);
    Q_INVOKABLE void update(QVariantMap game);

signals:
    void countChanged();

private:
    static QVariant roleValue(const QVariantMap &game, int role);
    QVector<QVariantMap> m_games;
};

} // namespace something::models

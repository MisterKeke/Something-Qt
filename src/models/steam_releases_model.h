#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QVariantMap>
#include <QVector>

namespace something::models {

class SteamReleasesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY visibleCountChanged)

public:
    enum Role { AppIdRole = Qt::UserRole + 1, StoreUrlRole, TitleRole,
                ArtworkUrlRole, ArtworkSourceRole, ReleaseDateTextRole,
                DatePrecisionRole, DatePrecisionLabelRole, HasExactDateRole,
                ExactDateRole, WindowStartRole, WindowEndRole, StatusRole,
                StatusLabelRole, FollowedAtUtcRole, LastSuccessfulUpdateAtUtcRole,
                LastAttemptedUpdateAtUtcRole, RefreshStateRole, RefreshErrorRole,
                DateChangeUnacknowledgedRole };
    Q_ENUM(Role)

    explicit SteamReleasesModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const noexcept { return m_all.size(); }
    int visibleCount() const noexcept { return m_visible.size(); }
    QVariantMap rowAt(int row) const;
    Q_INVOKABLE int rowForAppId(quint32 appId) const;
    Q_INVOKABLE int visibleRowForAppId(quint32 appId) const;
    QVector<QVariantMap> allRows() const { return m_all; }

    void replace(QVariantList releases);
    void append(QVariantMap release);
    void update(QVariantMap release);
    void removeByAppId(quint32 appId);
    void setSearchText(QString text);
    void setStatusFilter(QString filter);
    void setSortOrder(QString order);

signals:
    void countChanged();
    void visibleCountChanged();

private:
    static QVariant roleValue(const QVariantMap &release, int role);
    static QVariantMap normalizedRelease(QVariantMap release);
    void rebuildVisible();

    QVector<QVariantMap> m_all;
    QVector<QVariantMap> m_visible;
    QString m_searchText;
    QString m_statusFilter{QStringLiteral("all")};
    QString m_sortOrder{QStringLiteral("release_asc")};
};

} // namespace something::models

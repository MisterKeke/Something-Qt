#pragma once

#include <QAbstractListModel>
#include <QVariantMap>

namespace something::models {

class ProviderItemsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        ItemIdRole = Qt::UserRole + 1, TitleRole, TextRole, PreviewRole,
        PublishedAtRole, ViewsRole, ImagesRole, ImageSourcesRole,
        ExternalUrlRole, ChannelTitleRole, SourceIdRole
    };
    Q_ENUM(Role)

    explicit ProviderItemsModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_items.size(); }
    QVariantMap itemAt(int row) const;
    void replace(QVariantList items);
    void appendUnique(QVariantList items);
    void prependUnique(QVariantList items);
    void updateImageSource(const QString &itemId, int imageIndex, const QString &source);

signals:
    void countChanged();

private:
    QVariantList m_items;
};

class FavoriteChannelsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString categoryFilter READ categoryFilter WRITE setCategoryFilter NOTIFY categoryFilterChanged)

public:
    enum Role { SourceIdRole = Qt::UserRole + 1, LabelRole, CategoryIdRole,
                CategoryNameRole, CategoryColorRole };
    Q_ENUM(Role)
    explicit FavoriteChannelsModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_visibleRows.size(); }
    QString categoryFilter() const { return m_categoryFilter; }
    void setCategoryFilter(QString filter);
    void replace(QVariantList rows);

signals:
    void countChanged();
    void categoryFilterChanged();

private:
    void rebuild();
    QVariantList m_allRows;
    QVariantList m_visibleRows;
    QString m_categoryFilter{QStringLiteral("all")};
};

class FavoriteCategoriesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { CategoryIdRole = Qt::UserRole + 1, NameRole, ColorRole, DisplayOrderRole };
    Q_ENUM(Role)
    explicit FavoriteCategoriesModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_rows.size(); }
    Q_INVOKABLE QVariantMap rowAt(int row) const;
    void replace(QVariantList rows);

signals:
    void countChanged();

private:
    QVariantList m_rows;
};

} // namespace something::models

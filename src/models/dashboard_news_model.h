#pragma once

#include <QAbstractListModel>
#include <QVariantList>
#include <QVector>

namespace something::models {

class DashboardNewsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        SourceRole = Qt::UserRole + 1, TitleRole, PreviewRole, PublishedAtRole,
        SourceIdRole, ItemIdRole, ImagesRole, ImageSourcesRole, ExternalUrlRole, ViewsRole
    };
    Q_ENUM(Role)

    explicit DashboardNewsModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_items.size(); }
    void replace(QVariantList items);
    QVariantMap itemAt(int row) const;
    void updateImageSource(const QString &itemId, int imageIndex, const QString &source);

signals:
    void countChanged();

private:
    struct Item {
        QString source;
        QString title;
        QString preview;
        QString publishedAt;
        QString sourceId;
        QString itemId;
        QVariantList images;
        QVariantList imageSources;
        QString externalUrl;
        QString views;
    };
    QVector<Item> m_items;
};

} // namespace something::models

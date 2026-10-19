#pragma once

#include <QAbstractListModel>
#include <QVariantMap>

namespace something::models {

class SetupsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Role { IdRole = Qt::UserRole + 1, NameRole, DescriptionRole,
                ApplicationIdsRole, ApplicationSummaryRole, IconSourceRole, IconFilenameRole };
    explicit SetupsModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_rows.size(); }
    void replace(QVariantList rows, QVariantList applications);
    void upsert(QVariantMap row, QVariantList applications);
    bool removeById(qint64 id);
    QVariantMap rowForId(qint64 id) const;
signals:
    void countChanged();
private:
    static bool less(const QVariantMap &left, const QVariantMap &right);
    QString summary(const QVariantMap &row) const;
    QString iconSource(const QVariantMap &row) const;
    void apply(int row, const QVariantMap &value);
    QVector<QVariantMap> m_rows;
    QHash<qint64, QString> m_applicationNames;
};

} // namespace something::models

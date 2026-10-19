#pragma once

#include <QAbstractListModel>
#include <QVariantMap>

namespace something::models {

class DesktopApplicationsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { IdRole = Qt::UserRole + 1, DisplayNameRole, ExecutablePathRole,
                IconFilenameRole, IconSourceRole, NativeIconDataUrlRole, AvailableRole };
    explicit DesktopApplicationsModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int count() const noexcept { return m_rows.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void replace(QVariantList rows);
    void upsert(QVariantMap row);
    bool removeById(qint64 id);
    QVariantMap rowForId(qint64 id) const;

signals:
    void countChanged();

private:
    static bool less(const QVariantMap &left, const QVariantMap &right);
    static QString iconSource(const QVariantMap &row);
    void applyRowChange(int row, const QVariantMap &value);
    QVector<QVariantMap> m_rows;
};

} // namespace something::models

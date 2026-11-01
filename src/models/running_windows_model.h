#pragma once

#include "../platform/desktop_applications_platform.h"

#include <QAbstractListModel>
#include <QVector>

namespace something::models {

class RunningWindowsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { WindowIdRole = Qt::UserRole + 1, TitleRole, ProcessNameRole,
                IconDataUrlRole, MinimizedRole, ActiveRole };
    Q_ENUM(Role)

    explicit RunningWindowsModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_windows.size(); }
    void replace(QVector<platform::RunningWindow> windows);
    Q_INVOKABLE int rowForId(const QString &id) const;
    QString idAt(int row) const;

signals:
    void countChanged();

private:
    static bool less(const platform::RunningWindow &left,
                     const platform::RunningWindow &right);
    void applyRowChange(int row, const platform::RunningWindow &window);
    QVector<platform::RunningWindow> m_windows;
};

} // namespace something::models

#include "desktop_applications_model.h"

#include <algorithm>

namespace something::models {

DesktopApplicationsModel::DesktopApplicationsModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int DesktopApplicationsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant DesktopApplicationsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return {};
    const QVariantMap &row = m_rows.at(index.row());
    switch (role) {
    case IdRole: return row.value(QStringLiteral("id"));
    case DisplayNameRole: return row.value(QStringLiteral("displayName"));
    case ExecutablePathRole: return row.value(QStringLiteral("executablePath"));
    case IconFilenameRole: return row.value(QStringLiteral("iconFilename"));
    case IconSourceRole: return iconSource(row);
    case NativeIconDataUrlRole: return row.value(QStringLiteral("nativeIconDataUrl"));
    case AvailableRole: return row.value(QStringLiteral("available"), false);
    default: return {};
    }
}

QHash<int, QByteArray> DesktopApplicationsModel::roleNames() const
{
    return {{IdRole, "applicationId"}, {DisplayNameRole, "displayName"},
            {ExecutablePathRole, "executablePath"}, {IconFilenameRole, "iconFilename"},
            {IconSourceRole, "iconSource"}, {NativeIconDataUrlRole, "nativeIconDataUrl"},
            {AvailableRole, "available"}};
}

bool DesktopApplicationsModel::less(const QVariantMap &left, const QVariantMap &right)
{
    const int comparison = QString::compare(left.value(QStringLiteral("displayName")).toString(),
        right.value(QStringLiteral("displayName")).toString(), Qt::CaseInsensitive);
    if (comparison != 0) return comparison < 0;
    return left.value(QStringLiteral("id")).toLongLong() < right.value(QStringLiteral("id")).toLongLong();
}

QString DesktopApplicationsModel::iconSource(const QVariantMap &row)
{
    const QString filename = row.value(QStringLiteral("iconFilename")).toString();
    return filename.isEmpty() ? row.value(QStringLiteral("nativeIconDataUrl")).toString()
        : QStringLiteral("image://application-icons/%1").arg(filename);
}

void DesktopApplicationsModel::applyRowChange(int row, const QVariantMap &value)
{
    if (m_rows.at(row) == value) return;
    const QVariantMap old = m_rows.at(row);
    m_rows[row] = value;
    QVector<int> changed;
    for (const auto &[role, key] : {
        std::pair{IdRole, QStringLiteral("id")},
        std::pair{DisplayNameRole, QStringLiteral("displayName")},
        std::pair{ExecutablePathRole, QStringLiteral("executablePath")},
        std::pair{IconFilenameRole, QStringLiteral("iconFilename")},
        std::pair{NativeIconDataUrlRole, QStringLiteral("nativeIconDataUrl")},
        std::pair{AvailableRole, QStringLiteral("available")}}) {
        if (old.value(key) != value.value(key)) changed.push_back(role);
    }
    if (old.value(QStringLiteral("iconFilename")) != value.value(QStringLiteral("iconFilename"))
        || old.value(QStringLiteral("nativeIconDataUrl")) != value.value(QStringLiteral("nativeIconDataUrl")))
        changed.push_back(IconSourceRole);
    if (!changed.isEmpty()) emit dataChanged(index(row), index(row), changed);
}

void DesktopApplicationsModel::replace(QVariantList rows)
{
    QVector<QVariantMap> desired;
    desired.reserve(rows.size());
    for (const QVariant &value : rows) desired.push_back(value.toMap());
    std::sort(desired.begin(), desired.end(), less);
    const int oldCount = m_rows.size();
    for (int target = 0; target < desired.size(); ++target) {
        const qint64 wantedId = desired.at(target).value(QStringLiteral("id")).toLongLong();
        if (target < m_rows.size()
            && m_rows.at(target).value(QStringLiteral("id")).toLongLong() == wantedId) {
            applyRowChange(target, desired.at(target));
            continue;
        }
        int source = -1;
        for (int i = target + 1; i < m_rows.size(); ++i) {
            if (m_rows.at(i).value(QStringLiteral("id")).toLongLong() == wantedId) {
                source = i;
                break;
            }
        }
        if (source >= 0) {
            beginMoveRows({}, source, source, {}, target);
            m_rows.move(source, target);
            endMoveRows();
            applyRowChange(target, desired.at(target));
        } else {
            beginInsertRows({}, target, target);
            m_rows.insert(target, desired.at(target));
            endInsertRows();
        }
    }
    while (m_rows.size() > desired.size()) {
        const int last = m_rows.size() - 1;
        beginRemoveRows({}, last, last);
        m_rows.removeAt(last);
        endRemoveRows();
    }
    if (oldCount != m_rows.size()) emit countChanged();
}

void DesktopApplicationsModel::upsert(QVariantMap row)
{
    const qint64 id = row.value(QStringLiteral("id")).toLongLong();
    if (id <= 0) return;
    int existing = -1;
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).value(QStringLiteral("id")).toLongLong() == id) { existing = i; break; }
    if (existing >= 0) {
        QVector<QVariantMap> without = m_rows;
        without.removeAt(existing);
        const auto it = std::lower_bound(without.begin(), without.end(), row, less);
        const int destination = static_cast<int>(std::distance(without.begin(), it));
        if (destination == existing) {
            applyRowChange(existing, row);
            return;
        }
        const int destinationChild = destination > existing ? destination + 1 : destination;
        beginMoveRows({}, existing, existing, {}, destinationChild);
        m_rows.move(existing, destination);
        endMoveRows();
        applyRowChange(destination, row);
        return;
    }
    const auto it = std::lower_bound(m_rows.begin(), m_rows.end(), row, less);
    const int destination = static_cast<int>(std::distance(m_rows.begin(), it));
    beginInsertRows({}, destination, destination);
    m_rows.insert(destination, row);
    endInsertRows();
    emit countChanged();
}

bool DesktopApplicationsModel::removeById(qint64 id)
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).value(QStringLiteral("id")).toLongLong() != id) continue;
        beginRemoveRows({}, i, i);
        m_rows.removeAt(i);
        endRemoveRows();
        emit countChanged();
        return true;
    }
    return false;
}

QVariantMap DesktopApplicationsModel::rowForId(qint64 id) const
{
    for (const QVariantMap &row : m_rows)
        if (row.value(QStringLiteral("id")).toLongLong() == id) return row;
    return {};
}

} // namespace something::models

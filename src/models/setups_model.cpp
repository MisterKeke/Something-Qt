#include "setups_model.h"

#include <QStringList>
#include <algorithm>

namespace something::models {

SetupsModel::SetupsModel(QObject *parent) : QAbstractListModel(parent) {}
int SetupsModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_rows.size(); }

QVariant SetupsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return {};
    const QVariantMap &row = m_rows.at(index.row());
    switch (role) {
    case IdRole: return row.value(QStringLiteral("id"));
    case NameRole: return row.value(QStringLiteral("name"));
    case DescriptionRole: return row.value(QStringLiteral("description"));
    case ApplicationIdsRole: return row.value(QStringLiteral("applicationIds"));
    case ApplicationSummaryRole: return summary(row);
    case IconFilenameRole: return row.value(QStringLiteral("iconFilename"));
    case IconSourceRole: return iconSource(row);
    default: return {};
    }
}

QHash<int, QByteArray> SetupsModel::roleNames() const
{
    return {{IdRole, "setupId"}, {NameRole, "name"}, {DescriptionRole, "description"},
        {ApplicationIdsRole, "applicationIds"}, {ApplicationSummaryRole, "applicationSummary"},
        {IconSourceRole, "iconSource"}, {IconFilenameRole, "iconFilename"}};
}

QString SetupsModel::summary(const QVariantMap &row) const
{
    QStringList names;
    for (const QVariant &value : row.value(QStringLiteral("applicationIds")).toList()) {
        const qint64 id = value.toLongLong();
        names.push_back(m_applicationNames.value(id, QStringLiteral("Removed application")));
    }
    return names.join(QStringLiteral(" · "));
}

QString SetupsModel::iconSource(const QVariantMap &row) const
{
    const QString filename = row.value(QStringLiteral("iconFilename")).toString();
    return filename.isEmpty() ? QString() : QStringLiteral("image://setup-icons/%1").arg(filename);
}

bool SetupsModel::less(const QVariantMap &left, const QVariantMap &right)
{
    const int cmp = QString::compare(left.value(QStringLiteral("name")).toString(),
        right.value(QStringLiteral("name")).toString(), Qt::CaseInsensitive);
    if (cmp != 0) return cmp < 0;
    return left.value(QStringLiteral("id")).toLongLong()
        < right.value(QStringLiteral("id")).toLongLong();
}

void SetupsModel::apply(int row, const QVariantMap &value)
{
    if (m_rows.at(row) == value) return;
    const QVariantMap old = m_rows.at(row);
    m_rows[row] = value;
    QVector<int> changed;
    for (const auto &[role, key] : {std::pair{IdRole, QStringLiteral("id")},
            std::pair{NameRole, QStringLiteral("name")},
            std::pair{DescriptionRole, QStringLiteral("description")},
            std::pair{ApplicationIdsRole, QStringLiteral("applicationIds")},
            std::pair{IconFilenameRole, QStringLiteral("iconFilename")}})
        if (old.value(key) != value.value(key)) changed.push_back(role);
    if (old.value(QStringLiteral("applicationIds")) != value.value(QStringLiteral("applicationIds")))
        changed.push_back(ApplicationSummaryRole);
    if (old.value(QStringLiteral("iconFilename")) != value.value(QStringLiteral("iconFilename")))
        changed.push_back(IconSourceRole);
    if (!changed.isEmpty()) emit dataChanged(index(row), index(row), changed);
}

void SetupsModel::replace(QVariantList rows, QVariantList applications)
{
    m_applicationNames.clear();
    for (const QVariant &value : applications) {
        const QVariantMap app = value.toMap();
        m_applicationNames.insert(app.value(QStringLiteral("id")).toLongLong(),
                                  app.value(QStringLiteral("displayName")).toString());
    }
    QVector<QVariantMap> desired;
    desired.reserve(rows.size());
    for (const QVariant &value : rows) desired.push_back(value.toMap());
    std::sort(desired.begin(), desired.end(), less);
    beginResetModel();
    m_rows = std::move(desired);
    endResetModel();
    emit countChanged();
}

void SetupsModel::upsert(QVariantMap row, QVariantList applications)
{
    m_applicationNames.clear();
    for (const QVariant &value : applications) {
        const QVariantMap app = value.toMap();
        m_applicationNames.insert(app.value(QStringLiteral("id")).toLongLong(),
                                  app.value(QStringLiteral("displayName")).toString());
    }
    const qint64 id = row.value(QStringLiteral("id")).toLongLong();
    if (id <= 0) return;
    QVector<QVariantMap> desired = m_rows;
    int existing = -1;
    for (int i = 0; i < desired.size(); ++i)
        if (desired.at(i).value(QStringLiteral("id")).toLongLong() == id) { existing = i; break; }
    const bool inserting = existing < 0;
    if (inserting) desired.push_back(std::move(row));
    else desired[existing] = std::move(row);
    std::sort(desired.begin(), desired.end(), less);
    for (int target = 0; target < desired.size(); ++target) {
        const qint64 wanted = desired.at(target).value(QStringLiteral("id")).toLongLong();
        if (target < m_rows.size()
            && m_rows.at(target).value(QStringLiteral("id")).toLongLong() == wanted) {
            apply(target, desired.at(target));
            continue;
        }
        int source = -1;
        for (int i = target + 1; i < m_rows.size(); ++i)
            if (m_rows.at(i).value(QStringLiteral("id")).toLongLong() == wanted) { source = i; break; }
        if (source >= 0) {
            beginMoveRows({}, source, source, {}, target);
            m_rows.move(source, target);
            endMoveRows();
            apply(target, desired.at(target));
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
    if (inserting) emit countChanged();
}

bool SetupsModel::removeById(qint64 id)
{
    for (int row = 0; row < m_rows.size(); ++row) {
        if (m_rows.at(row).value(QStringLiteral("id")).toLongLong() != id) continue;
        beginRemoveRows({}, row, row);
        m_rows.removeAt(row);
        endRemoveRows();
        emit countChanged();
        return true;
    }
    return false;
}

QVariantMap SetupsModel::rowForId(qint64 id) const
{
    for (const auto &row : m_rows)
        if (row.value(QStringLiteral("id")).toLongLong() == id) return row;
    return {};
}

} // namespace something::models

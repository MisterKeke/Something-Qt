#include "universal_search_model.h"

#include <utility>

namespace something::models {

UniversalSearchModel::UniversalSearchModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int UniversalSearchModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_results.size();
}

QVariant UniversalSearchModel::data(const QModelIndex &index, int role) const
{
    const QVariantMap result = rowAt(index.row());
    if (result.isEmpty()) return {};
    switch (role) {
    case ResultIdRole: return result.value(QStringLiteral("id"));
    case TypeRole: return result.value(QStringLiteral("type"));
    case GroupRole: return result.value(QStringLiteral("group"));
    case TitleRole: return result.value(QStringLiteral("title"));
    case SubtitleRole: return result.value(QStringLiteral("subtitle"));
    case SourceRole: return result.value(QStringLiteral("source"));
    case RouteIdRole: return result.value(QStringLiteral("routeId"));
    case TargetIdRole: return result.value(QStringLiteral("targetId"));
    case SecondaryIdRole: return result.value(QStringLiteral("secondaryId"));
    case IconRole: return result.value(QStringLiteral("icon"));
    case ActionsRole: return result.value(QStringLiteral("actions"));
    case ScoreRole: return result.value(QStringLiteral("score"));
    case PayloadRole: return result.value(QStringLiteral("payload"));
    default: return {};
    }
}

QHash<int, QByteArray> UniversalSearchModel::roleNames() const
{
    return {{ResultIdRole, "resultId"}, {TypeRole, "type"}, {GroupRole, "group"},
            {TitleRole, "title"}, {SubtitleRole, "subtitle"}, {SourceRole, "source"},
            {RouteIdRole, "routeId"}, {TargetIdRole, "targetId"},
            {SecondaryIdRole, "secondaryId"}, {IconRole, "icon"},
            {ActionsRole, "actions"}, {ScoreRole, "score"}, {PayloadRole, "payload"}};
}

QVariantMap UniversalSearchModel::rowAt(int row) const
{
    return row >= 0 && row < m_results.size() ? m_results.at(row) : QVariantMap{};
}

void UniversalSearchModel::replace(QVector<QVariantMap> results)
{
    if (m_results == results) return;
    const int oldCount = m_results.size();
    beginResetModel();
    m_results = std::move(results);
    endResetModel();
    if (oldCount != m_results.size()) emit countChanged();
}

void UniversalSearchModel::clear()
{
    replace({});
}

} // namespace something::models

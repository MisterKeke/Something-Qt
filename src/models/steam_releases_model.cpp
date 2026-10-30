#include "steam_releases_model.h"

#include <QDate>

#include <algorithm>
#include <utility>

namespace something::models {
namespace {

QString statusLabel(const QString &status)
{
    if (status == QStringLiteral("exact_date_announced"))
        return QStringLiteral("Exact date announced");
    if (status == QStringLiteral("approximate_date_announced"))
        return QStringLiteral("Approximate release window");
    if (status == QStringLiteral("coming_soon")) return QStringLiteral("Coming soon");
    if (status == QStringLiteral("released")) return QStringLiteral("Released");
    return QStringLiteral("Release information unavailable");
}

QString precisionLabel(const QString &precision)
{
    if (precision == QStringLiteral("exact_day")) return QStringLiteral("Exact date");
    if (precision == QStringLiteral("month")) return QStringLiteral("Month");
    if (precision == QStringLiteral("year")) return QStringLiteral("Year");
    if (precision == QStringLiteral("quarter")) return QStringLiteral("Quarter");
    if (precision == QStringLiteral("season")) return QStringLiteral("Season");
    if (precision == QStringLiteral("range")) return QStringLiteral("Date range");
    return QStringLiteral("Date not announced");
}

bool matchesFilter(const QVariantMap &release, const QString &filter)
{
    const QString status = release.value(QStringLiteral("status")).toString();
    if (filter == QStringLiteral("upcoming")) {
        if (status == QStringLiteral("exact_date_announced")) {
            const QDate exactDate = QDate::fromString(
                release.value(QStringLiteral("exactDate")).toString(), Qt::ISODate);
            return exactDate.isValid() && exactDate >= QDate::currentDate();
        }
        if (status == QStringLiteral("approximate_date_announced")) {
            const QString lastKnownDay = release.value(QStringLiteral("windowEnd")).toString()
                .isEmpty() ? release.value(QStringLiteral("windowStart")).toString()
                           : release.value(QStringLiteral("windowEnd")).toString();
            if (lastKnownDay.isEmpty()) return true;
            const QDate endDate = QDate::fromString(lastKnownDay, Qt::ISODate);
            return !endDate.isValid() || endDate >= QDate::currentDate();
        }
        return false;
    }
    if (filter == QStringLiteral("released"))
        return status == QStringLiteral("released");
    if (filter == QStringLiteral("unknown_date"))
        return status == QStringLiteral("coming_soon");
    if (filter == QStringLiteral("unavailable"))
        return status == QStringLiteral("information_unavailable");
    return true;
}

QString dateSortKey(const QVariantMap &release)
{
    const QString exact = release.value(QStringLiteral("exactDate")).toString();
    if (!exact.isEmpty()) return exact;
    const QString start = release.value(QStringLiteral("windowStart")).toString();
    return start.isEmpty() ? release.value(QStringLiteral("windowEnd")).toString() : start;
}

bool releaseLess(const QVariantMap &left, const QVariantMap &right, const QString &order)
{
    const QString leftTitle = left.value(QStringLiteral("title")).toString();
    const QString rightTitle = right.value(QStringLiteral("title")).toString();
    if (order == QStringLiteral("title")) {
        const int comparison = QString::compare(leftTitle, rightTitle, Qt::CaseInsensitive);
        if (comparison != 0) return comparison < 0;
        return left.value(QStringLiteral("appId")).toUInt()
            < right.value(QStringLiteral("appId")).toUInt();
    }

    const QString leftDate = dateSortKey(left);
    const QString rightDate = dateSortKey(right);
    if (leftDate.isEmpty() != rightDate.isEmpty()) return !leftDate.isEmpty();
    if (!leftDate.isEmpty() && leftDate != rightDate) {
        const bool descending = order == QStringLiteral("release_desc");
        return descending ? leftDate > rightDate : leftDate < rightDate;
    }
    const int comparison = QString::compare(leftTitle, rightTitle, Qt::CaseInsensitive);
    if (comparison != 0) return comparison < 0;
    return left.value(QStringLiteral("appId")).toUInt()
        < right.value(QStringLiteral("appId")).toUInt();
}

} // namespace

SteamReleasesModel::SteamReleasesModel(QObject *parent) : QAbstractListModel(parent) {}

int SteamReleasesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_visible.size();
}

QVariant SteamReleasesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size()) return {};
    return roleValue(m_visible.at(index.row()), role);
}

QHash<int, QByteArray> SteamReleasesModel::roleNames() const
{
    return {{AppIdRole, "appId"}, {StoreUrlRole, "storeUrl"}, {TitleRole, "title"},
            {ArtworkUrlRole, "artworkUrl"}, {ArtworkSourceRole, "artworkSource"},
            {ReleaseDateTextRole, "releaseDateText"}, {DatePrecisionRole, "datePrecision"},
            {DatePrecisionLabelRole, "datePrecisionLabel"}, {HasExactDateRole, "hasExactDate"},
            {ExactDateRole, "exactDate"}, {WindowStartRole, "windowStart"},
            {WindowEndRole, "windowEnd"}, {StatusRole, "status"},
            {StatusLabelRole, "statusLabel"}, {FollowedAtUtcRole, "followedAtUtc"},
            {LastSuccessfulUpdateAtUtcRole, "lastSuccessfulUpdateAtUtc"},
            {LastAttemptedUpdateAtUtcRole, "lastAttemptedUpdateAtUtc"},
            {RefreshStateRole, "refreshState"}, {RefreshErrorRole, "refreshError"}};
}

QVariant SteamReleasesModel::roleValue(const QVariantMap &release, int role)
{
    switch (role) {
    case AppIdRole: return release.value(QStringLiteral("appId"));
    case StoreUrlRole: return release.value(QStringLiteral("storeUrl"));
    case TitleRole: return release.value(QStringLiteral("title"));
    case ArtworkUrlRole: return release.value(QStringLiteral("artworkUrl"));
    case ArtworkSourceRole: return release.value(QStringLiteral("artworkSource"));
    case ReleaseDateTextRole: return release.value(QStringLiteral("releaseDateText"));
    case DatePrecisionRole: return release.value(QStringLiteral("datePrecision"));
    case DatePrecisionLabelRole: return release.value(QStringLiteral("datePrecisionLabel"));
    case HasExactDateRole: return release.value(QStringLiteral("hasExactDate"));
    case ExactDateRole: return release.value(QStringLiteral("exactDate"));
    case WindowStartRole: return release.value(QStringLiteral("windowStart"));
    case WindowEndRole: return release.value(QStringLiteral("windowEnd"));
    case StatusRole: return release.value(QStringLiteral("status"));
    case StatusLabelRole: return release.value(QStringLiteral("statusLabel"));
    case FollowedAtUtcRole: return release.value(QStringLiteral("followedAtUtc"));
    case LastSuccessfulUpdateAtUtcRole:
        return release.value(QStringLiteral("lastSuccessfulUpdateAtUtc"));
    case LastAttemptedUpdateAtUtcRole:
        return release.value(QStringLiteral("lastAttemptedUpdateAtUtc"));
    case RefreshStateRole: return release.value(QStringLiteral("refreshState"));
    case RefreshErrorRole: return release.value(QStringLiteral("refreshError"));
    default: return {};
    }
}

QVariantMap SteamReleasesModel::normalizedRelease(QVariantMap release)
{
    const QString status = release.value(QStringLiteral("status")).toString();
    const QString precision = release.value(QStringLiteral("datePrecision")).toString();
    release.insert(QStringLiteral("statusLabel"), statusLabel(status));
    release.insert(QStringLiteral("datePrecisionLabel"), precisionLabel(precision));
    return release;
}

QVariantMap SteamReleasesModel::rowAt(int row) const
{
    return row >= 0 && row < m_visible.size() ? m_visible.at(row) : QVariantMap{};
}

int SteamReleasesModel::rowForAppId(quint32 appId) const
{
    for (int row = 0; row < m_all.size(); ++row)
        if (m_all.at(row).value(QStringLiteral("appId")).toUInt() == appId) return row;
    return -1;
}

void SteamReleasesModel::replace(QVariantList releases)
{
    const int oldCount = m_all.size();
    QVector<QVariantMap> next;
    next.reserve(releases.size());
    for (const QVariant &release : releases) next.push_back(normalizedRelease(release.toMap()));
    m_all = std::move(next);
    rebuildVisible();
    if (oldCount != m_all.size()) emit countChanged();
}

void SteamReleasesModel::append(QVariantMap release)
{
    const quint32 appId = release.value(QStringLiteral("appId")).toUInt();
    const int existing = rowForAppId(appId);
    if (existing >= 0) {
        m_all[existing] = normalizedRelease(std::move(release));
        rebuildVisible();
        return;
    }
    const int oldCount = m_all.size();
    m_all.push_back(normalizedRelease(std::move(release)));
    rebuildVisible();
    if (oldCount != m_all.size()) emit countChanged();
}

void SteamReleasesModel::update(QVariantMap release)
{
    const quint32 appId = release.value(QStringLiteral("appId")).toUInt();
    const int row = rowForAppId(appId);
    if (row < 0) {
        append(std::move(release));
        return;
    }
    m_all[row] = normalizedRelease(std::move(release));
    rebuildVisible();
}

void SteamReleasesModel::removeByAppId(quint32 appId)
{
    const int row = rowForAppId(appId);
    if (row < 0) return;
    m_all.removeAt(row);
    rebuildVisible();
    emit countChanged();
}

void SteamReleasesModel::setSearchText(QString text)
{
    text = text.trimmed();
    if (m_searchText == text) return;
    m_searchText = std::move(text);
    rebuildVisible();
}

void SteamReleasesModel::setStatusFilter(QString filter)
{
    if (filter != QStringLiteral("upcoming") && filter != QStringLiteral("released")
        && filter != QStringLiteral("unknown_date") && filter != QStringLiteral("unavailable"))
        filter = QStringLiteral("all");
    if (m_statusFilter == filter) return;
    m_statusFilter = std::move(filter);
    rebuildVisible();
}

void SteamReleasesModel::setSortOrder(QString order)
{
    if (order != QStringLiteral("release_desc") && order != QStringLiteral("title"))
        order = QStringLiteral("release_asc");
    if (m_sortOrder == order) return;
    m_sortOrder = std::move(order);
    rebuildVisible();
}

void SteamReleasesModel::rebuildVisible()
{
    QVector<QVariantMap> next;
    next.reserve(m_all.size());
    const QString needle = m_searchText.toCaseFolded();
    for (const QVariantMap &release : m_all) {
        if (!matchesFilter(release, m_statusFilter)) continue;
        if (!needle.isEmpty()) {
            const QString haystack = release.value(QStringLiteral("title")).toString()
                + QLatin1Char(' ') + release.value(QStringLiteral("releaseDateText")).toString()
                + QLatin1Char(' ') + release.value(QStringLiteral("statusLabel")).toString();
            if (!haystack.toCaseFolded().contains(needle)) continue;
        }
        next.push_back(release);
    }
    std::sort(next.begin(), next.end(), [this](const QVariantMap &left,
                                               const QVariantMap &right) {
        return releaseLess(left, right, m_sortOrder);
    });
    const int oldVisibleCount = m_visible.size();
    beginResetModel();
    m_visible = std::move(next);
    endResetModel();
    if (oldVisibleCount != m_visible.size()) emit visibleCountChanged();
}

} // namespace something::models

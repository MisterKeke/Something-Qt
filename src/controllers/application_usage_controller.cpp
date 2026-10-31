#include "application_usage_controller.h"

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QSet>
#include <QTime>

#include <algorithm>

namespace something::controllers {

ApplicationUsageController::ApplicationUsageController(
    application::ApplicationServiceGraph *graph,
    services::ApplicationUsageService *service, QObject *parent)
    : QObject(parent), m_graph(graph), m_service(service)
{
    updateDateRange();
    m_searchTimer.setSingleShot(true);
    m_searchTimer.setInterval(250);
    connect(&m_searchTimer, &QTimer::timeout, this, &ApplicationUsageController::refresh);
    m_refreshTimer.setInterval(30000);
    m_refreshTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_refreshTimer, &QTimer::timeout, this, &ApplicationUsageController::refresh);
    if (m_graph) {
        connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
                this, &ApplicationUsageController::onStorageFinished);
    }
    if (m_service) {
        connect(m_service, &services::ApplicationUsageService::stateChanged,
                this, [this] { applyLiveStates(); emit stateChanged(); });
        connect(m_service, &services::ApplicationUsageService::liveApplicationsChanged,
                this, &ApplicationUsageController::applyLiveStates);
        connect(m_service, &services::ApplicationUsageService::statisticsChanged,
                this, [this] { if (m_pageVisible) refresh(); });
    }
}

bool ApplicationUsageController::ready() const noexcept
{
    return m_service && m_service->ready();
}
bool ApplicationUsageController::trackingEnabled() const noexcept
{
    return m_service && m_service->trackingEnabled();
}
bool ApplicationUsageController::trackingActive() const noexcept
{
    return m_service && m_service->trackingActive();
}
bool ApplicationUsageController::supported() const noexcept
{
    return m_service && m_service->supported();
}
int ApplicationUsageController::retentionDays() const noexcept
{
    return m_service ? m_service->retentionDays() : 365;
}
QString ApplicationUsageController::trackingStatus() const
{
    return m_service ? m_service->trackingStatus() : tr("Tracking service unavailable.");
}
QVariantList ApplicationUsageController::exclusions() const
{
    return m_service ? m_service->exclusions() : QVariantList{};
}
QVariantList ApplicationUsageController::applications() const
{
    return m_overview.value(QStringLiteral("applications")).toList();
}

void ApplicationUsageController::setPageVisible(bool visible)
{
    if (m_pageVisible == visible) return;
    m_pageVisible = visible;
    if (visible) {
        m_refreshTimer.start();
        refresh();
    } else {
        m_refreshTimer.stop();
        m_searchTimer.stop();
    }
}

void ApplicationUsageController::refresh()
{
    if (!ready() || !m_graph || !m_pageVisible) return;
    updateDateRange();
    const quint64 generation = ++m_generation;
    m_errorMessage.clear();
    m_loading = true;
    requestOverview(generation);
    if (!m_selectedApplicationId.isEmpty()) requestDetail(generation);
    updateLoading();
    emit stateChanged();
}

void ApplicationUsageController::setRangePreset(const QString &preset)
{
    static const QStringList valid{QStringLiteral("today"), QStringLiteral("yesterday"),
        QStringLiteral("last7Days"), QStringLiteral("last30Days"),
        QStringLiteral("currentMonth"), QStringLiteral("custom")};
    if (!valid.contains(preset)) return;
    m_rangePreset = preset;
    if (preset == QStringLiteral("custom") && m_customFirstDay.isEmpty()) {
        const QDate today = QDate::currentDate();
        m_customFirstDay = today.addDays(-6).toString(Qt::ISODate);
        m_customLastDay = today.toString(Qt::ISODate);
    }
    refresh();
    emit stateChanged();
}

void ApplicationUsageController::setCustomRange(const QString &firstDay,
                                                  const QString &lastDay)
{
    const QDate first = QDate::fromString(firstDay, Qt::ISODate);
    const QDate last = QDate::fromString(lastDay, Qt::ISODate);
    if (!first.isValid() || !last.isValid() || first > last) return;
    m_customFirstDay = first.toString(Qt::ISODate);
    m_customLastDay = last.toString(Qt::ISODate);
    m_rangePreset = QStringLiteral("custom");
    refresh();
    emit stateChanged();
}

void ApplicationUsageController::setSearchText(const QString &text)
{
    if (m_searchText == text) return;
    m_searchText = text;
    if (m_pageVisible) m_searchTimer.start();
    emit stateChanged();
}

void ApplicationUsageController::setSortKey(const QString &sortKey)
{
    static const QStringList valid{QStringLiteral("total"), QStringLiteral("foreground"),
        QStringLiteral("fullscreen"), QStringLiteral("name"), QStringLiteral("lastActivity")};
    if (!valid.contains(sortKey) || m_sortKey == sortKey) return;
    m_sortKey = sortKey;
    refresh();
    emit stateChanged();
}

void ApplicationUsageController::selectApplication(const QString &applicationId)
{
    if (m_selectedApplicationId == applicationId) return;
    m_selectedApplicationId = applicationId;
    m_detail.clear();
    if (m_pageVisible && !applicationId.isEmpty()) {
        const quint64 generation = ++m_generation;
        requestDetail(generation);
        updateLoading();
    }
    emit stateChanged();
}

void ApplicationUsageController::clearSelection()
{
    if (m_selectedApplicationId.isEmpty()) return;
    m_selectedApplicationId.clear();
    m_detail.clear();
    emit stateChanged();
}

void ApplicationUsageController::setTrackingEnabled(bool enabled)
{
    if (m_service) m_service->setTrackingEnabled(enabled);
}
void ApplicationUsageController::setRetentionDays(int days)
{
    if (m_service) m_service->setRetentionDays(days);
}
void ApplicationUsageController::addApplicationExclusion(const QString &applicationId,
                                                           const QString &canonicalPath,
                                                           const QString &executableName)
{
    if (m_service) m_service->addApplicationExclusion(applicationId, canonicalPath, executableName);
}
void ApplicationUsageController::addFilenameExclusion(const QString &executableName)
{
    if (m_service) m_service->addFilenameExclusion(executableName);
}
void ApplicationUsageController::removeExclusion(const QString &applicationId)
{
    if (m_service) m_service->removeExclusion(applicationId);
}
void ApplicationUsageController::deleteHistory(const QString &applicationId)
{
    if (m_service) m_service->deleteHistory(applicationId);
}

QString ApplicationUsageController::formatDuration(qint64 durationMs) const
{
    durationMs = std::max<qint64>(0, durationMs);
    const qint64 totalMinutes = durationMs / 60000;
    const qint64 days = totalMinutes / (24 * 60);
    const qint64 hours = (totalMinutes / 60) % 24;
    const qint64 minutes = totalMinutes % 60;
    if (days > 0) return tr("%1d %2h").arg(days).arg(hours);
    if (hours > 0) return tr("%1h %2m").arg(hours).arg(minutes);
    return tr("%1m").arg(minutes);
}

QString ApplicationUsageController::formatTimestamp(const QString &timestampUtc) const
{
    QDateTime timestamp = QDateTime::fromString(timestampUtc, Qt::ISODateWithMs);
    if (!timestamp.isValid()) timestamp = QDateTime::fromString(timestampUtc, Qt::ISODate);
    return timestamp.isValid()
        ? QLocale().toString(timestamp.toLocalTime(), QLocale::ShortFormat) : QString();
}

void ApplicationUsageController::requestOverview(quint64 generation)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("usage-statistics-overview"));
    if (!operation.isValid()) return;
    m_requests.insert(operation.id.value, {Request::Kind::Overview, generation});
    const QString firstDay = m_firstLocalDay;
    const QString lastDay = m_lastLocalDay;
    const QString startUtc = m_startUtc;
    const QString endUtc = m_endUtc;
    const QString search = m_searchText;
    const QString sort = m_sortKey;
    if (!m_graph->submitStorage(operation,
        [firstDay, lastDay, startUtc, endUtc, search, sort](storage::StorageRepositories &repositories,
                                                            const std::function<bool()> &cancelled) {
            storage::StorageReply reply;
            if (cancelled()) { reply.error = domain::AppError::cancelled(); return reply; }
            QString diagnostic;
            reply.values = repositories.applicationUsage.queryOverview(
                firstDay, lastDay, startUtc, endUtc, search, sort, &diagnostic);
            if (!diagnostic.isEmpty()) reply.error = domain::AppError(domain::ErrorCategory::Storage);
            return reply;
        })) {
        if (m_graph->operations().acceptsResult(operation.id)) m_requests.remove(operation.id.value);
    }
}

void ApplicationUsageController::requestDetail(quint64 generation)
{
    if (m_selectedApplicationId.isEmpty()) return;
    const auto operation = m_graph->beginOperation(QStringLiteral("usage-statistics-detail"));
    if (!operation.isValid()) return;
    m_requests.insert(operation.id.value, {Request::Kind::Detail, generation});
    const QString applicationId = m_selectedApplicationId;
    const QString firstDay = m_firstLocalDay;
    const QString lastDay = m_lastLocalDay;
    const QString startUtc = m_startUtc;
    const QString endUtc = m_endUtc;
    if (!m_graph->submitStorage(operation,
        [applicationId, firstDay, lastDay, startUtc, endUtc](storage::StorageRepositories &repositories,
                                                            const std::function<bool()> &cancelled) {
            storage::StorageReply reply;
            if (cancelled()) { reply.error = domain::AppError::cancelled(); return reply; }
            QString diagnostic;
            reply.values = repositories.applicationUsage.queryApplication(
                applicationId, firstDay, lastDay, startUtc, endUtc, &diagnostic);
            if (!diagnostic.isEmpty()) reply.error = domain::AppError(domain::ErrorCategory::Storage);
            return reply;
        })) {
        if (m_graph->operations().acceptsResult(operation.id)) m_requests.remove(operation.id.value);
    }
}

void ApplicationUsageController::onStorageFinished(quint64 operationId,
                                                     QVariantMap values,
                                                     domain::AppError error)
{
    const auto it = m_requests.find(operationId);
    if (it == m_requests.end()) return;
    const Request request = it.value();
    m_requests.erase(it);
    if (request.generation == m_generation) {
        if (error.isError() && !error.isCancelled()) {
            m_errorMessage = tr("Usage statistics could not be loaded from local storage.");
        } else if (!error.isCancelled()) {
            m_errorMessage.clear();
            if (request.kind == Request::Kind::Overview) m_overview = std::move(values);
            else m_detail = std::move(values);
            applyLiveStates();
        }
        updateLoading();
        emit stateChanged();
    }
}

void ApplicationUsageController::applyLiveStates()
{
    if (!m_service) return;
    const QVariantList live = m_service->liveApplications();
    QHash<QString, QVariantMap> liveById;
    for (const QVariant &item : live) {
        const QVariantMap map = item.toMap();
        liveById.insert(map.value(QStringLiteral("applicationId")).toString(), map);
    }
    QVariantList rows = m_overview.value(QStringLiteral("applications")).toList();
    QSet<QString> present;
    for (int index = 0; index < rows.size(); ++index) {
        QVariantMap row = rows.at(index).toMap();
        const QString id = row.value(QStringLiteral("applicationId")).toString();
        present.insert(id);
        const auto current = liveById.constFind(id);
        row.insert(QStringLiteral("running"), current != liveById.cend());
        row.insert(QStringLiteral("currentState"), current == liveById.cend()
            ? QString() : current->value(QStringLiteral("state")));
        rows[index] = row;
    }
    for (auto it = liveById.cbegin(); it != liveById.cend(); ++it) {
        if (present.contains(it.key())) continue;
        const QString liveName = it->value(QStringLiteral("name")).toString();
        if (!m_searchText.trimmed().isEmpty()
            && !liveName.contains(m_searchText.trimmed(), Qt::CaseInsensitive)) continue;
        rows.push_back(QVariantMap{
            {QStringLiteral("applicationId"), it.key()},
            {QStringLiteral("name"), liveName},
            {QStringLiteral("executableName"), it->value(QStringLiteral("executableName"))},
            {QStringLiteral("executablePath"), it->value(QStringLiteral("executablePath"))},
            {QStringLiteral("totalMs"), 0}, {QStringLiteral("foregroundMs"), 0},
            {QStringLiteral("fullscreenMs"), 0}, {QStringLiteral("windowedMs"), 0},
            {QStringLiteral("backgroundMs"), 0}, {QStringLiteral("minimizedMs"), 0},
            {QStringLiteral("unknownMs"), 0}, {QStringLiteral("sessionCount"), 0},
            {QStringLiteral("running"), true},
            {QStringLiteral("currentState"), it->value(QStringLiteral("state"))}});
    }
    m_overview.insert(QStringLiteral("applications"), rows);
    m_overview.insert(QStringLiteral("applicationCount"), rows.size());
    if (!m_selectedApplicationId.isEmpty()) {
        const auto current = liveById.constFind(m_selectedApplicationId);
        m_detail.insert(QStringLiteral("running"), current != liveById.cend());
        m_detail.insert(QStringLiteral("currentState"), current == liveById.cend()
            ? QString() : current->value(QStringLiteral("state")));
    }
    emit stateChanged();
}

void ApplicationUsageController::updateDateRange()
{
    const QDate today = QDate::currentDate();
    QDate first = today;
    QDate last = today;
    if (m_rangePreset == QStringLiteral("yesterday")) first = last = today.addDays(-1);
    else if (m_rangePreset == QStringLiteral("last7Days")) first = today.addDays(-6);
    else if (m_rangePreset == QStringLiteral("last30Days")) first = today.addDays(-29);
    else if (m_rangePreset == QStringLiteral("currentMonth")) first = QDate(today.year(), today.month(), 1);
    else if (m_rangePreset == QStringLiteral("custom")) {
        const QDate customFirst = QDate::fromString(m_customFirstDay, Qt::ISODate);
        const QDate customLast = QDate::fromString(m_customLastDay, Qt::ISODate);
        if (customFirst.isValid() && customLast.isValid() && customFirst <= customLast) {
            first = customFirst;
            last = customLast;
        }
    }
    m_firstLocalDay = first.toString(Qt::ISODate);
    m_lastLocalDay = last.toString(Qt::ISODate);
    const QDateTime localStart(first, QTime(0, 0), Qt::LocalTime);
    const QDateTime localEnd(last.addDays(1), QTime(0, 0), Qt::LocalTime);
    m_startUtc = localStart.toUTC().toString(Qt::ISODateWithMs);
    m_endUtc = localEnd.toUTC().toString(Qt::ISODateWithMs);
}

void ApplicationUsageController::updateLoading()
{
    m_loading = false;
    for (auto it = m_requests.cbegin(); it != m_requests.cend(); ++it) {
        if (it->generation == m_generation) { m_loading = true; break; }
    }
}

} // namespace something::controllers

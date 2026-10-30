#include "steam_release_controller.h"

#include "../domain/steam_game.h"
#include "../providers/feature_parsers.h"
#include "../providers/steam_release_provider.h"
#include "../services/steam_release_service.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QThread>
#include <QUrl>

#include <utility>

namespace something::controllers {
namespace {

constexpr int kMaximumRefreshRequests = 4;
constexpr int kMaximumArtworkRequests = 3;

QString safeRefreshError(domain::AppError error)
{
    switch (error.category()) {
    case domain::ErrorCategory::Connection:
        return QStringLiteral("Could not connect to Steam.");
    case domain::ErrorCategory::Timeout:
        return QStringLiteral("Steam took too long to respond.");
    case domain::ErrorCategory::Http:
    case domain::ErrorCategory::Provider:
        return QStringLiteral("Steam could not provide this game's release details.");
    case domain::ErrorCategory::InvalidResponse:
    case domain::ErrorCategory::ResponseTooLarge:
        return QStringLiteral("Steam returned release details that could not be read safely.");
    case domain::ErrorCategory::CapabilityUnavailable:
        return QStringLiteral("Steam networking is unavailable.");
    case domain::ErrorCategory::Busy:
        return QStringLiteral("Steam updates are busy. Try again shortly.");
    default:
        return QStringLiteral("Release information could not be refreshed.");
    }
}

QString artworkFilename(const QVariantMap &release)
{
    return release.value(QStringLiteral("artworkFilename")).toString();
}

} // namespace

SteamReleaseController::SteamReleaseController(
    application::ApplicationServiceGraph *graph, QObject *parent)
    : QObject(parent), m_graph(graph), m_releases(this)
{
    Q_ASSERT(m_graph);
    Q_ASSERT(QThread::currentThread() == thread());
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &SteamReleaseController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::httpsFinished,
            this, &SteamReleaseController::onHttpsFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &SteamReleaseController::onValueJobFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &SteamReleaseController::onLifecycleChanged);
    onLifecycleChanged(m_graph->lifecycle().state());
}

void SteamReleaseController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    if (state == application::ApplicationLifecycle::State::Running) {
        m_ready = true;
        if (!m_snapshotStarted) {
            m_snapshotStarted = true;
            requestSnapshot();
        }
        return;
    }
    if (state == application::ApplicationLifecycle::State::ShuttingDown
        || state == application::ApplicationLifecycle::State::Failed
        || state == application::ApplicationLifecycle::State::Stopped) {
        m_ready = false;
        cancelAll();
        emit stateChanged();
    }
}

quint64 SteamReleaseController::submitStorage(Kind kind, storage::StorageCommand command,
                                             Pending pending)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("Steam Release Radar storage"));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    pending.kind = kind;
    m_pending.insert(id, std::move(pending));
    if (!m_graph->submitStorage(operation, std::move(command))) {
        m_pending.remove(id);
        return 0;
    }
    return id;
}

quint64 SteamReleaseController::submitNetwork(Kind kind,
                                              providers::ProviderRequest request,
                                              Pending pending)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("Steam release provider request"));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    pending.kind = kind;
    m_pending.insert(id, std::move(pending));
    if (!m_graph->requestHttps(operation, std::move(request))) {
        m_pending.remove(id);
        return 0;
    }
    return id;
}

quint64 SteamReleaseController::submitParse(Kind kind, QByteArray body, Pending pending)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("parse Steam release details"));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    pending.kind = kind;
    const quint32 expectedAppId = pending.appId;
    m_pending.insert(id, pending);
    const bool submitted = m_graph->submitExpensiveValue(operation,
        [body = std::move(body), expectedAppId](const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            if (token.isCancellationRequested()) {
                result.error = domain::AppError::cancelled();
                return result;
            }
            const auto parsed = providers::parseSteamReleaseDetails(body, expectedAppId);
            if (!parsed.succeeded()) {
                result.error = parsed.error;
                return result;
            }
            result.values = SteamReleaseController::detailsToMap(parsed.value);
            return result;
        });
    if (!submitted) {
        m_pending.remove(id);
        return 0;
    }
    return id;
}

quint64 SteamReleaseController::submitArtworkStage(QByteArray body, Pending pending)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("validate Steam release artwork"));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    pending.kind = Kind::ArtworkStage;
    m_pending.insert(id, pending);
    const QString profileDirectory = m_graph->profileDirectory();
    const quint32 appId = pending.appId;
    const bool submitted = m_graph->submitExpensiveValue(operation,
        [profileDirectory, appId, body = std::move(body)](
            const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            services::StagedSteamArtwork artwork;
            result.error = services::SteamArtworkStore::stageResponse(
                profileDirectory, appId, body, token, &artwork);
            if (result.error.isError()) return result;
            result.values = {{QStringLiteral("filename"), artwork.asset.filename},
                             {QStringLiteral("mimeType"), artwork.asset.mimeType},
                             {QStringLiteral("byteSize"), artwork.asset.byteSize},
                             {QStringLiteral("stagingFilename"), artwork.stagingFilename},
                             {QStringLiteral("stagedNewFile"), artwork.stagedNewFile}};
            return result;
        });
    if (!submitted) {
        m_pending.remove(id);
        return 0;
    }
    return id;
}

void SteamReleaseController::requestSnapshot()
{
    if (!m_ready || m_snapshotBusy) return;
    m_snapshotBusy = true;
    m_errorMessage.clear();
    Pending pending;
    pending.kind = Kind::Snapshot;
    if (!submitStorage(Kind::Snapshot,
            services::SteamReleaseService::loadSnapshot(m_graph->profileDirectory()), pending)) {
        m_snapshotBusy = false;
        m_errorMessage = QStringLiteral("Saved Release Radar entries could not be loaded.");
    }
    emit stateChanged();
}

void SteamReleaseController::addRelease(QString storeUrl)
{
    if (!m_ready || !m_snapshotLoaded || m_addBusy || m_refreshBusy || m_deleteBusy
        || m_dateChangeAcknowledgeBusy) return;
    m_addError.clear();
    m_notification.clear();
    const providers::SteamReleaseProviderRequest request =
        providers::makeSteamReleaseProviderRequest(std::move(storeUrl));
    if (!request.succeeded()) {
        m_addError = QStringLiteral("Enter a valid Steam Store /app/<id> URL.");
        emit stateChanged();
        return;
    }
    if (m_releases.rowForAppId(request.appId) >= 0
        || m_reservedAppIds.contains(request.appId)) {
        m_addError = QStringLiteral("This game is already in Release Radar.");
        emit stateChanged();
        return;
    }
    m_addBusy = true;
    m_addError.clear();
    m_errorMessage.clear();
    m_reservedAppIds.insert(request.appId);
    Pending pending;
    pending.kind = Kind::AddNetwork;
    pending.appId = request.appId;
    pending.canonicalStoreUrl = request.canonicalStoreUrl;
    if (!submitNetwork(Kind::AddNetwork, request.providerRequest, pending)) {
        releaseAddReservation(request.appId);
        m_addBusy = false;
        m_addError = QStringLiteral("Steam networking is unavailable right now.");
    }
    emit stateChanged();
}

void SteamReleaseController::refreshReleases()
{
    if (!m_ready || !m_snapshotLoaded || m_refreshBusy || m_addBusy || m_deleteBusy
        || m_dateChangeAcknowledgeBusy) return;
    m_refreshWork.clear();
    m_refreshUpdates.clear();
    m_refreshFinished.clear();
    m_refreshInFlight.clear();
    const QVector<QVariantMap> rows = m_releases.allRows();
    m_refreshWork.reserve(rows.size());
    for (const QVariantMap &row : rows) {
        RefreshWork work;
        work.appId = row.value(QStringLiteral("appId")).toUInt();
        work.canonicalStoreUrl = row.value(QStringLiteral("storeUrl")).toString();
        work.attemptedAtUtc = currentTimestamp();
        m_refreshWork.push_back(std::move(work));
    }
    if (m_refreshWork.isEmpty()) {
        m_refreshStatus = QStringLiteral("There are no followed releases to refresh.");
        emit stateChanged();
        return;
    }
    m_refreshUpdates.resize(m_refreshWork.size());
    m_refreshFinished.fill(false, m_refreshWork.size());
    m_refreshInFlight.fill(false, m_refreshWork.size());
    m_nextRefreshIndex = 0;
    m_activeRefreshRequests = 0;
    m_refreshFinishedCount = 0;
    m_refreshUpdatedCount = 0;
    m_refreshFailedCount = 0;
    m_refreshCommitPending = false;
    ++m_refreshGeneration;
    m_refreshBusy = true;
    m_errorMessage.clear();
    m_refreshStatus = QStringLiteral("Checking release information…");
    emit stateChanged();
    dispatchRefreshQueue();
}

void SteamReleaseController::dispatchRefreshQueue()
{
    if (!m_refreshBusy) return;
    while (m_activeRefreshRequests < kMaximumRefreshRequests
           && m_nextRefreshIndex < m_refreshWork.size()) {
        const int index = m_nextRefreshIndex++;
        const RefreshWork &work = m_refreshWork.at(index);
        const providers::SteamReleaseProviderRequest request =
            providers::makeSteamReleaseProviderRequest(work.canonicalStoreUrl);
        if (!request.succeeded()) {
            completeRefreshItem(index, nullptr,
                                QStringLiteral("The saved Steam Store URL is invalid."));
            continue;
        }
        Pending pending;
        pending.kind = Kind::RefreshNetwork;
        pending.appId = work.appId;
        pending.canonicalStoreUrl = request.canonicalStoreUrl;
        pending.refreshIndex = index;
        pending.refreshGeneration = m_refreshGeneration;
        if (!submitNetwork(Kind::RefreshNetwork, request.providerRequest, pending)) {
            completeRefreshItem(index, nullptr,
                                QStringLiteral("Steam networking is unavailable right now."));
            continue;
        }
        m_refreshInFlight[index] = true;
        ++m_activeRefreshRequests;
    }
    if (m_refreshFinishedCount == m_refreshWork.size() && m_activeRefreshRequests == 0)
        finishRefreshBatch();
}

void SteamReleaseController::completeRefreshItem(
    int index, const domain::SteamReleaseDetails *details, QString error)
{
    if (!m_refreshBusy || index < 0 || index >= m_refreshWork.size()
        || m_refreshFinished.at(index)) return;
    if (m_refreshInFlight.at(index)) {
        m_refreshInFlight[index] = false;
        m_activeRefreshRequests = qMax(0, m_activeRefreshRequests - 1);
    }
    const RefreshWork &work = m_refreshWork.at(index);
    storage::SteamReleaseRefreshUpdate update;
    update.appId = work.appId;
    update.attemptedAtUtc = work.attemptedAtUtc;
    if (details) {
        update.refreshedDetails = *details;
        ++m_refreshUpdatedCount;
    } else {
        update.safeError = error.trimmed().isEmpty()
            ? QStringLiteral("Release information could not be refreshed.") : error.trimmed();
        ++m_refreshFailedCount;
    }
    m_refreshUpdates[index] = std::move(update);
    m_refreshFinished[index] = true;
    ++m_refreshFinishedCount;
    m_refreshStatus = QStringLiteral("Checking release information… (%1/%2)")
        .arg(m_refreshFinishedCount).arg(m_refreshWork.size());
    if (m_refreshFinishedCount == m_refreshWork.size() && m_activeRefreshRequests == 0)
        finishRefreshBatch();
    emit stateChanged();
}

void SteamReleaseController::finishRefreshBatch()
{
    if (!m_refreshBusy || m_refreshCommitPending
        || m_refreshFinishedCount != m_refreshWork.size() || m_activeRefreshRequests != 0)
        return;
    m_refreshCommitPending = true;
    Pending pending;
    pending.kind = Kind::RefreshCommit;
    pending.refreshGeneration = m_refreshGeneration;
    if (!submitStorage(Kind::RefreshCommit,
            services::SteamReleaseService::applyRefresh(
                m_graph->profileDirectory(), m_refreshUpdates), pending)) {
        m_refreshCommitPending = false;
        m_refreshBusy = false;
        m_refreshStatus.clear();
        m_errorMessage = QStringLiteral("Release updates could not be saved. Cached information was kept.");
        emit stateChanged();
    }
}

void SteamReleaseController::removeRelease(quint32 appId)
{
    if (!m_ready || !m_snapshotLoaded || appId == 0 || m_deleteBusy
        || m_addBusy || m_refreshBusy || m_dateChangeAcknowledgeBusy) return;
    const int row = m_releases.rowForAppId(appId);
    if (row < 0) return;
    Pending pending;
    pending.kind = Kind::Delete;
    pending.appId = appId;
    pending.release = m_releases.allRows().at(row);
    m_deleteBusy = true;
    m_errorMessage.clear();
    if (!submitStorage(Kind::Delete,
            services::SteamReleaseService::removeRelease(m_graph->profileDirectory(), appId),
            pending)) {
        m_deleteBusy = false;
        m_errorMessage = QStringLiteral("The release could not be removed.");
    }
    emit stateChanged();
}

void SteamReleaseController::acknowledgeDateChange(quint32 appId)
{
    if (!m_ready || !m_snapshotLoaded || appId == 0 || m_dateChangeAcknowledgeBusy
        || m_addBusy || m_refreshBusy || m_deleteBusy) return;
    const int row = m_releases.rowForAppId(appId);
    if (row < 0 || !m_releases.allRows().at(row)
            .value(QStringLiteral("dateChangeUnacknowledged")).toBool()) return;

    Pending pending;
    pending.kind = Kind::DateChangeAcknowledge;
    pending.appId = appId;
    m_dateChangeAcknowledgeBusy = true;
    m_errorMessage.clear();
    if (!submitStorage(Kind::DateChangeAcknowledge,
            services::SteamReleaseService::acknowledgeDateChange(appId), pending)) {
        m_dateChangeAcknowledgeBusy = false;
        m_errorMessage = QStringLiteral("The release date change could not be acknowledged.");
    }
    emit stateChanged();
}

void SteamReleaseController::openReleaseDetails(quint32 appId)
{
    if (appId == 0) return;
    m_pendingReleaseDetailsAppId = appId;
    emit stateChanged();
}

void SteamReleaseController::clearPendingReleaseDetailsRequest()
{
    if (m_pendingReleaseDetailsAppId == 0) return;
    m_pendingReleaseDetailsAppId = 0;
    emit stateChanged();
}

void SteamReleaseController::openStore(quint32 appId)
{
    const int row = m_releases.rowForAppId(appId);
    if (row < 0) return;
    const QString storeUrl = m_releases.allRows().at(row)
        .value(QStringLiteral("storeUrl")).toString();
    const domain::SteamStoreUrl normalized = domain::normalizeSteamStoreUrl(storeUrl);
    if (!normalized.isValid() || normalized.appId != appId
        || !QDesktopServices::openUrl(QUrl(normalized.canonicalUrl))) {
        m_errorMessage = QStringLiteral("The Steam Store page could not be opened.");
        emit stateChanged();
    }
}

void SteamReleaseController::retryLoad()
{
    if (!m_ready || m_snapshotBusy || m_snapshotLoaded) return;
    requestSnapshot();
}

void SteamReleaseController::clearNotification()
{
    if (m_notification.isEmpty()) return;
    m_notification.clear();
    emit stateChanged();
}

void SteamReleaseController::setSearchText(QString text)
{
    m_releases.setSearchText(std::move(text));
}

void SteamReleaseController::setStatusFilter(QString filter)
{
    m_releases.setStatusFilter(std::move(filter));
}

void SteamReleaseController::setSortOrder(QString order)
{
    m_releases.setSortOrder(std::move(order));
}

void SteamReleaseController::ensureArtwork(quint32 appId)
{
    if (!m_ready || appId == 0) return;
    const int row = m_releases.rowForAppId(appId);
    if (row < 0) return;
    const QVariantMap release = m_releases.allRows().at(row);
    if (!release.value(QStringLiteral("artworkSource")).toString().isEmpty()) return;
    const QString url = domain::normalizeSteamArtworkUrl(
        release.value(QStringLiteral("artworkUrl")).toString());
    if (url.isEmpty()) return;
    if (m_activeArtworkUrls.value(appId) == url
        || m_queuedArtworkUrls.value(appId) == url) return;
    m_queuedArtworkUrls.insert(appId, url);
    m_artworkQueue.enqueue({appId, url});
    dispatchArtworkQueue();
}

void SteamReleaseController::dispatchArtworkQueue()
{
    while (m_activeArtworkRequests < kMaximumArtworkRequests && !m_artworkQueue.isEmpty()) {
        const int candidates = m_artworkQueue.size();
        bool submittedAny = false;
        for (int attempt = 0; attempt < candidates; ++attempt) {
            const ArtworkWork work = m_artworkQueue.dequeue();
            if (m_queuedArtworkUrls.value(work.appId) != work.artworkUrl) continue;
            if (m_activeArtworkUrls.contains(work.appId)) {
                m_artworkQueue.enqueue(work);
                continue;
            }
            m_queuedArtworkUrls.remove(work.appId);
            const int row = m_releases.rowForAppId(work.appId);
            if (row < 0) continue;
            const QVariantMap release = m_releases.allRows().at(row);
            if (!release.value(QStringLiteral("artworkSource")).toString().isEmpty()
                || domain::normalizeSteamArtworkUrl(
                    release.value(QStringLiteral("artworkUrl")).toString()) != work.artworkUrl) {
                continue;
            }
            Pending pending;
            pending.kind = Kind::ArtworkNetwork;
            pending.appId = work.appId;
            pending.artworkUrl = work.artworkUrl;
            providers::ProviderRequest request{providers::ProviderId::SteamArtwork,
                                                QUrl(work.artworkUrl)};
            if (!submitNetwork(Kind::ArtworkNetwork, std::move(request), pending)) continue;
            m_activeArtworkUrls.insert(work.appId, work.artworkUrl);
            ++m_activeArtworkRequests;
            submittedAny = true;
            break;
        }
        if (!submittedAny) break;
    }
}

void SteamReleaseController::finishArtwork(quint32 appId, const QString &artworkUrl)
{
    if (m_activeArtworkUrls.value(appId) == artworkUrl) {
        m_activeArtworkUrls.remove(appId);
        m_activeArtworkRequests = qMax(0, m_activeArtworkRequests - 1);
    }
    dispatchArtworkQueue();
}

void SteamReleaseController::onHttpsFinished(quint64 id, const QByteArray &body,
                                              domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    Pending pending = it.value();
    m_pending.erase(it);
    switch (pending.kind) {
    case Kind::AddNetwork:
        if (error.isError() || error.isCancelled()) {
            m_addError = safeProviderMessage(error,
                QStringLiteral("Steam release details could not be loaded."));
            releaseAddReservation(pending.appId);
            m_addBusy = false;
            emit stateChanged();
            return;
        }
        if (!submitParse(Kind::AddParse, body, pending)) {
            m_addError = QStringLiteral("Steam release details could not be parsed.");
            releaseAddReservation(pending.appId);
            m_addBusy = false;
            emit stateChanged();
        }
        return;
    case Kind::RefreshNetwork:
        if (!m_refreshBusy || pending.refreshGeneration != m_refreshGeneration) return;
        if (error.isError() || error.isCancelled()) {
            completeRefreshItem(pending.refreshIndex, nullptr, safeRefreshError(error));
            dispatchRefreshQueue();
            return;
        }
        if (!submitParse(Kind::RefreshParse, body, pending)) {
            completeRefreshItem(pending.refreshIndex, nullptr,
                                QStringLiteral("Steam release details could not be parsed."));
            dispatchRefreshQueue();
        }
        return;
    case Kind::ArtworkNetwork:
        if (error.isError() || error.isCancelled()) {
            finishArtwork(pending.appId, pending.artworkUrl);
            return;
        }
        if (!submitArtworkStage(body, pending))
            finishArtwork(pending.appId, pending.artworkUrl);
        return;
    default:
        return;
    }
}

void SteamReleaseController::onValueJobFinished(quint64 id, const QVariantMap &values,
                                                domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Pending pending = it.value();
    m_pending.erase(it);
    if (pending.kind == Kind::AddParse) {
        if (error.isError() || error.isCancelled()) {
            m_addError = safeProviderMessage(error,
                QStringLiteral("Steam returned invalid release details."));
            releaseAddReservation(pending.appId);
            m_addBusy = false;
            emit stateChanged();
            return;
        }
        const domain::SteamReleaseDetails details = detailsFromMap(values);
        if (details.appId != pending.appId || !domain::isValidSteamReleaseDetails(details)) {
            m_addError = QStringLiteral("Steam returned details that did not match this game.");
            releaseAddReservation(pending.appId);
            m_addBusy = false;
            emit stateChanged();
            return;
        }
        Pending insert = pending;
        insert.kind = Kind::AddInsert;
        if (!submitStorage(Kind::AddInsert,
                services::SteamReleaseService::addRelease(
                    pending.canonicalStoreUrl, details, currentTimestamp()), insert)) {
            m_addError = QStringLiteral("Release information was loaded but could not be saved.");
            releaseAddReservation(pending.appId);
            m_addBusy = false;
            emit stateChanged();
        }
        return;
    }
    if (pending.kind == Kind::RefreshParse) {
        if (!m_refreshBusy || pending.refreshGeneration != m_refreshGeneration) return;
        if (error.isError() || error.isCancelled()) {
            completeRefreshItem(pending.refreshIndex, nullptr, safeRefreshError(error));
        } else {
            const domain::SteamReleaseDetails details = detailsFromMap(values);
            if (details.appId != pending.appId || !domain::isValidSteamReleaseDetails(details))
                completeRefreshItem(pending.refreshIndex, nullptr,
                                    QStringLiteral("Steam returned details for a different game."));
            else
                completeRefreshItem(pending.refreshIndex, &details, {});
        }
        dispatchRefreshQueue();
        return;
    }
    if (pending.kind == Kind::ArtworkStage) {
        if (error.isError() || error.isCancelled()) {
            finishArtwork(pending.appId, pending.artworkUrl);
            return;
        }
        services::StagedSteamArtwork artwork;
        artwork.asset.filename = values.value(QStringLiteral("filename")).toString();
        artwork.asset.mimeType = values.value(QStringLiteral("mimeType")).toString();
        artwork.asset.byteSize = values.value(QStringLiteral("byteSize")).toLongLong();
        artwork.stagingFilename = values.value(QStringLiteral("stagingFilename")).toString();
        artwork.stagedNewFile = values.value(QStringLiteral("stagedNewFile")).toBool();
        Pending save = pending;
        save.kind = Kind::ArtworkSave;
        if (!submitStorage(Kind::ArtworkSave,
                services::SteamReleaseService::saveArtwork(
                    m_graph->profileDirectory(), pending.appId, pending.artworkUrl, artwork), save)) {
            services::SteamArtworkStore::discard(m_graph->profileDirectory(), artwork);
            finishArtwork(pending.appId, pending.artworkUrl);
        }
    }
}

void SteamReleaseController::onStorageFinished(quint64 id, const QVariantMap &values,
                                               domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Pending pending = it.value();
    m_pending.erase(it);
    switch (pending.kind) {
    case Kind::Snapshot:
        m_snapshotBusy = false;
        if (error.isError() || error.isCancelled()) {
            setSafeError(error, QStringLiteral("Saved Release Radar entries could not be loaded."));
        } else {
            m_releases.replace(values.value(QStringLiteral("releases")).toList());
            m_snapshotLoaded = true;
            m_errorMessage.clear();
        }
        emit stateChanged();
        return;
    case Kind::AddInsert:
        m_addBusy = false;
        releaseAddReservation(pending.appId);
        if (error.isError() || error.isCancelled()) {
            m_addError = error.category() == domain::ErrorCategory::Conflict
                ? QStringLiteral("This game is already in Release Radar.")
                : QStringLiteral("The release could not be saved.");
        } else {
            const QVariantMap release = values.value(QStringLiteral("release")).toMap();
            m_releases.append(release);
            emit releaseCalendarDataChanged();
            m_addError.clear();
            m_errorMessage.clear();
            m_notification = QStringLiteral("Added %1 to Release Radar.")
                .arg(release.value(QStringLiteral("title")).toString());
        }
        emit stateChanged();
        return;
    case Kind::Delete:
        m_deleteBusy = false;
        if (error.isError() || error.isCancelled()) {
            setSafeError(error, QStringLiteral("The release could not be removed."));
        } else {
            m_releases.removeByAppId(pending.appId);
            emit releaseCalendarDataChanged();
            const QString filename = artworkFilename(pending.release);
            if (!filename.isEmpty()) emit artworkInvalidated(filename);
            m_errorMessage.clear();
            m_notification = QStringLiteral("Release removed from Release Radar.");
        }
        emit stateChanged();
        return;
    case Kind::DateChangeAcknowledge:
        m_dateChangeAcknowledgeBusy = false;
        if (error.isError() || error.isCancelled()) {
            setSafeError(error,
                         QStringLiteral("The release date change could not be acknowledged."));
        } else {
            const int row = m_releases.rowForAppId(pending.appId);
            if (row >= 0) {
                QVariantMap release = m_releases.allRows().at(row);
                release.insert(QStringLiteral("dateChangeUnacknowledged"), false);
                m_releases.update(std::move(release));
            }
            m_errorMessage.clear();
        }
        emit stateChanged();
        return;
    case Kind::RefreshCommit:
        if (pending.refreshGeneration != m_refreshGeneration) return;
        m_refreshCommitPending = false;
        m_refreshBusy = false;
        if (error.isError() || error.isCancelled()) {
            m_refreshStatus.clear();
            m_errorMessage = QStringLiteral(
                "Release updates could not be saved. Cached information was kept.");
            emit stateChanged();
            return;
        }
        {
            const QVector<QVariantMap> previous = m_releases.allRows();
            const QVariantList fresh = values.value(QStringLiteral("releases")).toList();
            QHash<quint32, QVariantMap> nextByAppId;
            for (const QVariant &item : fresh) {
                const QVariantMap release = item.toMap();
                nextByAppId.insert(release.value(QStringLiteral("appId")).toUInt(), release);
            }
            for (const QVariantMap &oldRelease : previous) {
                const quint32 appId = oldRelease.value(QStringLiteral("appId")).toUInt();
                const QString oldFilename = artworkFilename(oldRelease);
                if (oldFilename.isEmpty()) continue;
                const QString newFilename = artworkFilename(nextByAppId.value(appId));
                if (oldFilename != newFilename) emit artworkInvalidated(oldFilename);
            }
            m_releases.replace(fresh);
            emit releaseCalendarDataChanged();
        }
        m_refreshStatus = QStringLiteral("Updated %1; %2 failed of %3 releases.")
            .arg(m_refreshUpdatedCount).arg(m_refreshFailedCount).arg(m_refreshWork.size());
        m_errorMessage.clear();
        emit stateChanged();
        return;
    case Kind::ArtworkSave:
        if (!error.isError() && !error.isCancelled()) {
            const QVariantMap release = values.value(QStringLiteral("release")).toMap();
            const int row = m_releases.rowForAppId(pending.appId);
            if (row >= 0 && m_releases.allRows().at(row)
                    .value(QStringLiteral("artworkUrl")).toString() == pending.artworkUrl) {
                const QVariantMap oldRelease = m_releases.allRows().at(row);
                const QString oldFilename = artworkFilename(oldRelease);
                const QString newFilename = artworkFilename(release);
                m_releases.update(release);
                if (!oldFilename.isEmpty() && oldFilename != newFilename)
                    emit artworkInvalidated(oldFilename);
                if (!newFilename.isEmpty()) emit artworkInvalidated(newFilename);
            }
        }
        finishArtwork(pending.appId, pending.artworkUrl);
        emit stateChanged();
        return;
    default:
        return;
    }
}

void SteamReleaseController::cancelPending(quint64 id)
{
    if (!id) return;
    m_pending.remove(id);
    m_graph->cancelOperation({id});
}

void SteamReleaseController::cancelAll()
{
    const auto ids = m_pending.keys();
    for (quint64 id : ids) cancelPending(id);
    m_pending.clear();
    m_reservedAppIds.clear();
    m_refreshWork.clear();
    m_refreshUpdates.clear();
    m_refreshFinished.clear();
    m_refreshInFlight.clear();
    m_nextRefreshIndex = 0;
    m_activeRefreshRequests = 0;
    m_refreshFinishedCount = 0;
    m_refreshUpdatedCount = 0;
    m_refreshFailedCount = 0;
    m_refreshCommitPending = false;
    m_artworkQueue.clear();
    m_queuedArtworkUrls.clear();
    m_activeArtworkUrls.clear();
    m_activeArtworkRequests = 0;
    m_snapshotBusy = false;
    m_addBusy = false;
    m_refreshBusy = false;
    m_deleteBusy = false;
    m_dateChangeAcknowledgeBusy = false;
}

void SteamReleaseController::releaseAddReservation(quint32 appId)
{
    m_reservedAppIds.remove(appId);
}

void SteamReleaseController::setSafeError(domain::AppError error, const QString &fallback)
{
    m_errorMessage = safeProviderMessage(error, fallback);
}

QString SteamReleaseController::safeProviderMessage(domain::AppError error,
                                                     const QString &fallback)
{
    switch (error.category()) {
    case domain::ErrorCategory::Connection:
        return QStringLiteral("Could not connect to Steam. Try again later.");
    case domain::ErrorCategory::Timeout:
        return QStringLiteral("Steam took too long to respond. Try again later.");
    case domain::ErrorCategory::Http:
    case domain::ErrorCategory::Provider:
        return QStringLiteral("Steam could not provide this game's release details right now.");
    case domain::ErrorCategory::InvalidResponse:
    case domain::ErrorCategory::ResponseTooLarge:
        return QStringLiteral("Steam returned details that could not be read safely.");
    case domain::ErrorCategory::CapabilityUnavailable:
        return QStringLiteral("Steam networking is unavailable right now.");
    case domain::ErrorCategory::Busy:
        return QStringLiteral("Steam updates are busy. Try again shortly.");
    default:
        return fallback;
    }
}

QString SteamReleaseController::currentTimestamp()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

domain::SteamReleaseDetails SteamReleaseController::detailsFromMap(const QVariantMap &map)
{
    domain::SteamReleaseDetails details;
    details.appId = map.value(QStringLiteral("appId")).toUInt();
    details.title = map.value(QStringLiteral("title")).toString();
    details.artworkUrl = map.value(QStringLiteral("artworkUrl")).toString();
    details.releaseDate.announcedText = map.value(QStringLiteral("releaseDateText")).toString();
    const auto precision = domain::steamReleaseDatePrecisionFromName(
        map.value(QStringLiteral("datePrecision")).toString());
    if (precision) details.releaseDate.precision = *precision;
    const QString exact = map.value(QStringLiteral("exactDate")).toString();
    const QString start = map.value(QStringLiteral("windowStart")).toString();
    const QString end = map.value(QStringLiteral("windowEnd")).toString();
    if (!exact.isEmpty()) details.releaseDate.exactDate = QDate::fromString(exact, Qt::ISODate);
    if (!start.isEmpty()) details.releaseDate.windowStart = QDate::fromString(start, Qt::ISODate);
    if (!end.isEmpty()) details.releaseDate.windowEnd = QDate::fromString(end, Qt::ISODate);
    const auto status = domain::steamReleaseStatusFromName(
        map.value(QStringLiteral("status")).toString());
    if (status) details.status = *status;
    return details;
}

QVariantMap SteamReleaseController::detailsToMap(const domain::SteamReleaseDetails &details)
{
    return {{QStringLiteral("appId"), details.appId},
            {QStringLiteral("title"), details.title},
            {QStringLiteral("artworkUrl"), details.artworkUrl},
            {QStringLiteral("releaseDateText"), details.releaseDate.announcedText},
            {QStringLiteral("datePrecision"),
             domain::steamReleaseDatePrecisionName(details.releaseDate.precision)},
            {QStringLiteral("exactDate"), details.releaseDate.exactDate
                 ? details.releaseDate.exactDate->toString(Qt::ISODate) : QString()},
            {QStringLiteral("windowStart"), details.releaseDate.windowStart
                 ? details.releaseDate.windowStart->toString(Qt::ISODate) : QString()},
            {QStringLiteral("windowEnd"), details.releaseDate.windowEnd
                 ? details.releaseDate.windowEnd->toString(Qt::ISODate) : QString()},
            {QStringLiteral("status"), domain::steamReleaseStatusName(details.status)}};
}

} // namespace something::controllers


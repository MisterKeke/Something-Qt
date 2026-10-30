#include "steam_game_updates_controller.h"

#include "../providers/feature_parsers.h"
#include "../providers/steam_news_provider.h"
#include "../providers/steam_release_provider.h"
#include "../services/bounded_worker_pool.h"
#include "../services/steam_game_updates_service.h"
#include "../services/steam_artwork_store.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace something::controllers {
namespace {

constexpr int kMaximumRefreshRequests = 3;
constexpr int kMaximumArtworkRequests = 3;
constexpr int kNewsPageSize = 60;

storage::StorageReply failed(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

bool safeExternalNewsUrl(const QString &raw, QUrl *result)
{
    const QUrl url(raw, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QStringLiteral("http")
                           && scheme != QStringLiteral("https"))
        || url.host().isEmpty() || !url.userInfo().isEmpty() || !url.fragment().isEmpty()
        || (url.port(-1) != -1
            && url.port(-1) != (scheme == QStringLiteral("https") ? 443 : 80))) {
        return false;
    }
    if (result) *result = url;
    return true;
}

services::StagedSteamArtwork stagedArtworkFromMap(const QVariantMap &values)
{
    services::StagedSteamArtwork artwork;
    artwork.asset.filename = values.value(QStringLiteral("filename")).toString();
    artwork.asset.mimeType = values.value(QStringLiteral("mimeType")).toString();
    artwork.asset.byteSize = values.value(QStringLiteral("byteSize")).toLongLong();
    artwork.stagingFilename = values.value(QStringLiteral("stagingFilename")).toString();
    artwork.stagedNewFile = values.value(QStringLiteral("stagedNewFile")).toBool();
    return artwork;
}

} // namespace

SteamGameUpdatesController::SteamGameUpdatesController(
    application::ApplicationServiceGraph *graph, QObject *parent)
    : QObject(parent), m_graph(graph)
{
    Q_ASSERT(m_graph);
    Q_ASSERT(QThread::currentThread() == thread());
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &SteamGameUpdatesController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::httpsFinished,
            this, &SteamGameUpdatesController::onHttpsFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &SteamGameUpdatesController::onValueJobFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &SteamGameUpdatesController::onLifecycleChanged);
    onLifecycleChanged(m_graph->lifecycle().state());
}

int SteamGameUpdatesController::totalUnreadCount() const
{
    int result = 0;
    for (const QVariant &value : m_games)
        result += value.toMap().value(QStringLiteral("unreadNewsCount")).toInt();
    return result;
}

void SteamGameUpdatesController::onLifecycleChanged(
    application::ApplicationLifecycle::State state)
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

quint64 SteamGameUpdatesController::submitStorage(
    Kind kind, storage::StorageCommand command, Pending pending)
{
    const auto operation = m_graph->beginOperation(
        QStringLiteral("Game Updates storage operation"));
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

quint64 SteamGameUpdatesController::submitNetwork(
    Kind kind, providers::ProviderRequest request, Pending pending)
{
    const auto operation = m_graph->beginOperation(
        QStringLiteral("Game Updates provider request"));
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

quint64 SteamGameUpdatesController::submitValueJob(
    Kind kind, QByteArray body, Pending pending)
{
    const auto operation = m_graph->beginOperation(
        kind == Kind::AddParse ? QStringLiteral("parse Steam game details")
                               : QStringLiteral("parse Steam game news"));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    pending.kind = kind;
    const quint32 expectedAppId = pending.appId;
    m_pending.insert(id, pending);
    const bool submitted = m_graph->submitExpensiveValue(operation,
        [body = std::move(body), expectedAppId, kind](
            const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            if (token.isCancellationRequested()) {
                result.error = domain::AppError::cancelled();
                return result;
            }
            if (kind == Kind::AddParse) {
                const auto parsed = providers::parseSteamGameDetails(body, expectedAppId);
                if (!parsed.succeeded()) {
                    result.error = parsed.error;
                    return result;
                }
                if (parsed.value.appId != expectedAppId || parsed.value.name.trimmed().isEmpty()) {
                    result.error = domain::AppError(domain::ErrorCategory::InvalidResponse);
                    return result;
                }
                result.values = {{QStringLiteral("appId"), parsed.value.appId},
                                 {QStringLiteral("name"), parsed.value.name},
                                 {QStringLiteral("artworkUrl"), parsed.value.artworkUrl}};
                return result;
            }

            const auto parsed = providers::parseSteamNewsArticles(body, expectedAppId);
            if (!parsed.succeeded()) {
                result.error = parsed.error;
                return result;
            }
            QVariantList articles;
            articles.reserve(parsed.value.size());
            for (const domain::SteamNewsArticle &article : parsed.value) {
                articles.push_back(SteamGameUpdatesController::articleMap(article));
            }
            result.values.insert(QStringLiteral("articles"), articles);
            return result;
        });
    if (!submitted) {
        m_pending.remove(id);
        return 0;
    }
    return id;
}

quint64 SteamGameUpdatesController::submitArtworkStage(QByteArray body, Pending pending)
{
    const auto operation = m_graph->beginOperation(
        QStringLiteral("validate Steam Game Updates artwork"));
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
            if (result.error.isError() || result.error.isCancelled()) return result;
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

void SteamGameUpdatesController::requestSnapshot()
{
    if (!m_ready || m_snapshotBusy || m_snapshotLoaded) return;
    m_snapshotBusy = true;
    m_errorMessage.clear();
    Pending pending;
    if (!submitStorage(Kind::Snapshot,
            services::SteamGameUpdatesService::loadSnapshot(m_graph->profileDirectory()), pending)) {
        m_snapshotBusy = false;
        m_errorMessage = QStringLiteral("Saved Game Updates games could not be loaded.");
    }
    emit stateChanged();
}

void SteamGameUpdatesController::addGame(QString storeUrl)
{
    if (!m_ready || !m_snapshotLoaded || m_addBusy || m_removeBusy || m_refreshBusy) return;
    m_addError.clear();
    m_notification.clear();
    const auto request = providers::makeSteamReleaseProviderRequest(std::move(storeUrl));
    if (!request.succeeded()) {
        m_addError = QStringLiteral("Enter a valid Steam Store /app/<id> URL.");
        emit stateChanged();
        return;
    }
    if (gameIndex(request.appId) >= 0 || m_reservedAppIds.contains(request.appId)) {
        m_addError = QStringLiteral("This game is already in Game Updates.");
        emit stateChanged();
        return;
    }
    m_addBusy = true;
    m_errorMessage.clear();
    m_reservedAppIds.insert(request.appId);
    Pending pending;
    pending.appId = request.appId;
    pending.storeUrl = request.canonicalStoreUrl;
    if (!submitNetwork(Kind::AddNetwork, request.providerRequest, pending)) {
        m_reservedAppIds.remove(request.appId);
        m_addBusy = false;
        m_addError = QStringLiteral("Steam networking is unavailable right now.");
    }
    emit stateChanged();
}

void SteamGameUpdatesController::removeGame(quint32 appId)
{
    if (!m_ready || !m_snapshotLoaded || m_removeBusy || m_addBusy || m_refreshBusy
        || gameIndex(appId) < 0) return;
    m_removeBusy = true;
    m_errorMessage.clear();
    Pending pending;
    pending.appId = appId;
    if (!submitStorage(Kind::RemoveFollow,
            services::SteamGameUpdatesService::removeFollow(
                m_graph->profileDirectory(), appId), pending)) {
        m_removeBusy = false;
        m_errorMessage = QStringLiteral("Game Updates is unavailable right now.");
    }
    emit stateChanged();
}

void SteamGameUpdatesController::setOwnershipStatus(quint32 appId, bool owned)
{
    if (!m_ready || gameIndex(appId) < 0 || m_removeBusy) return;
    Pending pending;
    pending.appId = appId;
    const auto status = owned ? domain::SteamGameUpdateOwnershipStatus::Owned
                              : domain::SteamGameUpdateOwnershipStatus::NotOwned;
    if (!submitStorage(Kind::SetOwnership,
            services::SteamGameUpdatesService::setOwnershipStatus(
                m_graph->profileDirectory(), appId, status), pending)) {
        m_errorMessage = QStringLiteral("The ownership label could not be saved.");
        emit stateChanged();
    }
}

void SteamGameUpdatesController::setSelectedAppId(quint32 appId)
{
    if (appId != 0 && gameIndex(appId) < 0) appId = 0;
    if (m_selectedAppId == appId) return;
    m_selectedAppId = appId;
    m_newsAppFilter = appId == 0 ? std::nullopt : std::optional<quint32>(appId);
    reloadNews();
}

void SteamGameUpdatesController::setUnreadOnly(bool unreadOnly)
{
    if (m_unreadOnly == unreadOnly) return;
    m_unreadOnly = unreadOnly;
    reloadNews();
}

void SteamGameUpdatesController::setSearchText(QString text)
{
    text = text.left(120);
    if (m_searchText == text) return;
    m_searchText = std::move(text);
    reloadNews();
}

void SteamGameUpdatesController::requestNewsPage(bool append)
{
    if (!m_ready || !m_snapshotLoaded || m_newsLoading) return;
    Pending pending;
    pending.appendPage = append;
    pending.generation = m_newsGeneration;
    const auto id = submitStorage(Kind::NewsPage,
        services::SteamGameUpdatesService::loadNewsPage(
            m_newsAppFilter, m_unreadOnly, m_searchText, kNewsPageSize,
            append ? m_newsCursor : storage::SteamGameUpdateNewsCursor{}), pending);
    if (!id) {
        m_newsError = QStringLiteral("Cached Game Updates articles could not be loaded.");
    } else {
        m_newsLoading = true;
        m_newsError.clear();
    }
    emit stateChanged();
}

void SteamGameUpdatesController::reloadNews()
{
    ++m_newsGeneration;
    const auto ids = m_pending.keys();
    for (quint64 id : ids) {
        const auto it = m_pending.constFind(id);
        if (it != m_pending.cend() && it->kind == Kind::NewsPage) cancelPending(id);
    }
    m_newsItems.clear();
    m_newsCursor = {};
    m_newsHasMore = false;
    m_newsLoading = false;
    m_newsError.clear();
    requestNewsPage(false);
}

void SteamGameUpdatesController::loadMoreNews()
{
    if (!m_newsHasMore || m_newsLoading) return;
    requestNewsPage(true);
}

void SteamGameUpdatesController::refreshGame(quint32 appId)
{
    if (gameIndex(appId) < 0) return;
    startRefresh({appId});
}

void SteamGameUpdatesController::refreshAll()
{
    QVector<quint32> appIds;
    appIds.reserve(m_games.size());
    for (const QVariant &value : m_games)
        appIds.push_back(value.toMap().value(QStringLiteral("appId")).toUInt());
    startRefresh(std::move(appIds));
}

void SteamGameUpdatesController::loadOlderFromSteam(quint32 appId)
{
    if (!m_ready || !m_snapshotLoaded || m_refreshBusy || m_addBusy || m_removeBusy
        || gameIndex(appId) < 0) return;
    m_refreshBusy = true;
    m_refreshStatus = QStringLiteral("Finding older cached news…");
    m_errorMessage.clear();
    Pending pending;
    pending.appId = appId;
    pending.generation = m_refreshGeneration;
    if (!submitStorage(Kind::OldestNewsTimestamp,
            services::SteamGameUpdatesService::oldestNewsPublishedAt(appId), pending)) {
        m_refreshBusy = false;
        m_refreshStatus = QStringLiteral("Could not load older Steam news.");
        m_errorMessage = QStringLiteral("The saved news cache could not be searched.");
    }
    emit stateChanged();
}

void SteamGameUpdatesController::startRefresh(
    QVector<quint32> appIds, std::optional<qint64> olderThanUnixSeconds)
{
    if (!m_ready || !m_snapshotLoaded || m_refreshBusy || m_addBusy || m_removeBusy) return;
    std::sort(appIds.begin(), appIds.end());
    appIds.erase(std::unique(appIds.begin(), appIds.end()), appIds.end());
    m_refreshQueue.clear();
    m_refreshCompletedAppIds.clear();
    for (quint32 appId : appIds) {
        if (appId != 0 && gameIndex(appId) >= 0) m_refreshQueue.enqueue(appId);
    }
    m_refreshTotal = m_refreshQueue.size();
    m_refreshOlderThanUnixSeconds = olderThanUnixSeconds;
    m_refreshFinishedCount = 0;
    m_refreshSuccessCount = 0;
    m_refreshFailureCount = 0;
    m_activeRefreshRequests = 0;
    ++m_refreshGeneration;
    if (m_refreshTotal == 0) {
        m_refreshOlderThanUnixSeconds.reset();
        m_refreshStatus = QStringLiteral("There are no followed games to refresh.");
        emit stateChanged();
        return;
    }
    m_refreshBusy = true;
    m_refreshStatus = m_refreshOlderThanUnixSeconds
        ? QStringLiteral("Loading older Steam news…")
        : QStringLiteral("Checking Steam news…");
    m_errorMessage.clear();
    emit stateChanged();
    dispatchRefreshQueue();
}

void SteamGameUpdatesController::dispatchRefreshQueue()
{
    if (!m_refreshBusy) return;
    while (m_activeRefreshRequests < kMaximumRefreshRequests
           && !m_refreshQueue.isEmpty()) {
        const quint32 appId = m_refreshQueue.dequeue();
        const auto request = providers::makeSteamNewsProviderRequest(
            appId, m_refreshOlderThanUnixSeconds);
        const QString attemptedAt = currentTimestamp();
        if (!request.succeeded()) {
            recordRefreshCheck(appId, attemptedAt, false,
                               QStringLiteral("The Steam App ID is invalid."),
                               m_refreshGeneration);
            continue;
        }
        Pending pending;
        pending.appId = appId;
        pending.attemptedAtUtc = attemptedAt;
        pending.generation = m_refreshGeneration;
        if (!submitNetwork(Kind::RefreshNetwork, request.providerRequest, pending)) {
            recordRefreshCheck(appId, attemptedAt, false,
                               QStringLiteral("Steam networking is unavailable right now."),
                               m_refreshGeneration);
            continue;
        }
        ++m_activeRefreshRequests;
    }
}

void SteamGameUpdatesController::recordRefreshCheck(
    quint32 appId, const QString &attemptedAtUtc, bool succeeded,
    const QString &safeError, quint64 generation)
{
    Pending pending;
    pending.appId = appId;
    pending.attemptedAtUtc = attemptedAtUtc;
    pending.success = succeeded;
    pending.safeError = safeError;
    pending.generation = generation;
    if (!submitStorage(Kind::RecordNewsCheck,
            services::SteamGameUpdatesService::recordNewsCheck(
                m_graph->profileDirectory(), appId, attemptedAtUtc, succeeded, safeError), pending)) {
        if (generation == m_refreshGeneration) {
            setLocalRefreshFailure(appId, attemptedAtUtc,
                safeError.isEmpty()
                    ? QStringLiteral("The refresh result could not be saved to this profile.")
                    : safeError);
            completeRefreshItem(appId, false, generation);
        }
    }
}

void SteamGameUpdatesController::completeRefreshItem(
    quint32 appId, bool succeeded, quint64 generation)
{
    if (!m_refreshBusy || generation != m_refreshGeneration
        || m_refreshCompletedAppIds.contains(appId)) return;
    m_refreshCompletedAppIds.insert(appId);
    ++m_refreshFinishedCount;
    if (succeeded) ++m_refreshSuccessCount;
    else ++m_refreshFailureCount;
    m_refreshStatus = QStringLiteral("Checked %1 of %2 games · %3 failed")
        .arg(m_refreshFinishedCount).arg(m_refreshTotal).arg(m_refreshFailureCount);
    if (m_refreshFinishedCount >= m_refreshTotal) finishRefresh();
    emit stateChanged();
}

void SteamGameUpdatesController::finishRefresh()
{
    m_refreshBusy = false;
    m_refreshQueue.clear();
    m_refreshOlderThanUnixSeconds.reset();
    m_refreshStatus = QStringLiteral("Updated %1 of %2 games; %3 failed.")
        .arg(m_refreshSuccessCount).arg(m_refreshTotal).arg(m_refreshFailureCount);
    reloadNews();
}

void SteamGameUpdatesController::cancelRefresh()
{
    if (!m_refreshBusy) return;
    const quint64 cancelledGeneration = m_refreshGeneration;
    const auto ids = m_pending.keys();
    for (quint64 id : ids) {
        const auto it = m_pending.constFind(id);
        if (it != m_pending.cend() && it->generation == cancelledGeneration
            && (it->kind == Kind::OldestNewsTimestamp
                || it->kind == Kind::RefreshNetwork || it->kind == Kind::RefreshParse
                || it->kind == Kind::SaveNewsPage || it->kind == Kind::RecordNewsCheck)) {
            cancelPending(id);
        }
    }
    ++m_refreshGeneration;
    m_refreshQueue.clear();
    m_refreshOlderThanUnixSeconds.reset();
    m_refreshCompletedAppIds.clear();
    m_activeRefreshRequests = 0;
    m_refreshBusy = false;
    m_refreshStatus = QStringLiteral("Refresh canceled. Cached news was kept.");
    reloadNews();
    emit stateChanged();
}

void SteamGameUpdatesController::setNewsRead(
    quint32 appId, QString newsId, bool isRead)
{
    if (!m_ready || m_newsMutationBusy || gameIndex(appId) < 0) return;
    m_newsMutationBusy = true;
    Pending pending;
    pending.appId = appId;
    pending.newsId = newsId;
    if (!submitStorage(Kind::MarkRead,
            services::SteamGameUpdatesService::setNewsRead(appId, std::move(newsId), isRead), pending)) {
        m_newsMutationBusy = false;
        m_newsError = QStringLiteral("The article read state could not be saved.");
    }
    emit stateChanged();
}

void SteamGameUpdatesController::markAllRead(quint32 appId)
{
    if (!m_ready || m_newsMutationBusy || gameIndex(appId) < 0) return;
    m_newsMutationBusy = true;
    Pending pending;
    pending.appId = appId;
    if (!submitStorage(Kind::MarkAllRead,
            services::SteamGameUpdatesService::setAllNewsRead(appId, true), pending)) {
        m_newsMutationBusy = false;
        m_newsError = QStringLiteral("The articles could not be marked as read.");
    }
    emit stateChanged();
}

void SteamGameUpdatesController::openArticle(quint32 appId, QString newsId)
{
    for (const QVariant &value : m_newsItems) {
        const QVariantMap item = value.toMap();
        if (item.value(QStringLiteral("appId")).toUInt() != appId
            || item.value(QStringLiteral("newsId")).toString() != newsId) continue;
        QUrl url;
        const QString source = item.value(QStringLiteral("sourceUrl")).toString();
        if (!safeExternalNewsUrl(source, &url) || !QDesktopServices::openUrl(url)) {
            m_errorMessage = QStringLiteral("The original Steam news article could not be opened.");
            emit stateChanged();
        }
        return;
    }
}

void SteamGameUpdatesController::openStore(quint32 appId)
{
    const int index = gameIndex(appId);
    if (index < 0) return;
    const domain::SteamStoreUrl normalized = domain::normalizeSteamStoreUrl(
        m_games.at(index).toMap().value(QStringLiteral("storeUrl")).toString());
    if (!normalized.isValid() || normalized.appId != appId
        || !QDesktopServices::openUrl(QUrl(normalized.canonicalUrl))) {
        m_errorMessage = QStringLiteral("The Steam Store page could not be opened.");
        emit stateChanged();
    }
}

void SteamGameUpdatesController::retryLoad()
{
    if (!m_ready || m_snapshotBusy || m_snapshotLoaded) return;
    requestSnapshot();
}

void SteamGameUpdatesController::clearMessages()
{
    if (m_notification.isEmpty() && m_errorMessage.isEmpty()) return;
    m_notification.clear();
    m_errorMessage.clear();
    emit stateChanged();
}

void SteamGameUpdatesController::onHttpsFinished(
    quint64 id, const QByteArray &body, domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Pending pending = it.value();
    m_pending.erase(it);

    if (pending.kind == Kind::AddNetwork) {
        if (error.isError() || error.isCancelled()) {
            m_reservedAppIds.remove(pending.appId);
            m_addBusy = false;
            m_addError = error.category() == domain::ErrorCategory::NotFound
                ? QStringLiteral("Steam could not find that game.")
                : QStringLiteral("Steam game details could not be loaded.");
        } else {
            Pending parse = pending;
            if (!submitValueJob(Kind::AddParse, body, parse)) {
                m_reservedAppIds.remove(pending.appId);
                m_addBusy = false;
                m_addError = QStringLiteral("Steam game details could not be read safely.");
            }
        }
        emit stateChanged();
        return;
    }
    if (pending.kind == Kind::RefreshNetwork) {
        if (pending.generation != m_refreshGeneration || !m_refreshBusy) return;
        m_activeRefreshRequests = qMax(0, m_activeRefreshRequests - 1);
        dispatchRefreshQueue();
        if (error.isError() || error.isCancelled()) {
            recordRefreshCheck(pending.appId, pending.attemptedAtUtc, false,
                               safeNewsError(error), pending.generation);
        } else if (!submitValueJob(Kind::RefreshParse, body, pending)) {
            recordRefreshCheck(pending.appId, pending.attemptedAtUtc, false,
                               QStringLiteral("Steam news could not be parsed safely."),
                               pending.generation);
        }
        emit stateChanged();
        return;
    }
    if (pending.kind == Kind::ArtworkNetwork) {
        if (error.isError() || error.isCancelled()) {
            finishArtwork(pending.appId, pending.artworkUrl);
        } else if (!submitArtworkStage(body, pending)) {
            finishArtwork(pending.appId, pending.artworkUrl);
        }
    }
}

void SteamGameUpdatesController::onValueJobFinished(
    quint64 id, const QVariantMap &values, domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Pending pending = it.value();
    m_pending.erase(it);
    if (pending.kind == Kind::AddParse) {
        if (error.isError() || error.isCancelled()) {
            m_reservedAppIds.remove(pending.appId);
            m_addBusy = false;
            m_addError = QStringLiteral("Steam returned game details that could not be read safely.");
            emit stateChanged();
            return;
        }
        domain::SteamGameUpdateFollow follow;
        follow.appId = pending.appId;
        follow.canonicalStoreUrl = pending.storeUrl;
        follow.name = values.value(QStringLiteral("name")).toString();
        follow.artwork.sourceUrl = domain::normalizeSteamArtworkUrl(
            values.value(QStringLiteral("artworkUrl")).toString());
        follow.addedAtUtc = currentTimestamp();
        Pending insert = pending;
        if (!submitStorage(Kind::AddInsert,
                services::SteamGameUpdatesService::addFollow(
                    m_graph->profileDirectory(), std::move(follow)), insert)) {
            m_reservedAppIds.remove(pending.appId);
            m_addBusy = false;
            m_addError = QStringLiteral("The game could not be saved to Game Updates.");
        }
        emit stateChanged();
        return;
    }
    if (pending.kind == Kind::RefreshParse) {
        if (pending.generation != m_refreshGeneration || !m_refreshBusy) return;
        if (error.isError() || error.isCancelled()) {
            recordRefreshCheck(pending.appId, pending.attemptedAtUtc, false,
                               safeNewsError(error), pending.generation);
            return;
        }
        QVector<domain::SteamNewsArticle> articles;
        const QVariantList encoded = values.value(QStringLiteral("articles")).toList();
        articles.reserve(encoded.size());
        for (const QVariant &article : encoded)
            articles.push_back(articleFromMap(article.toMap()));
        Pending save = pending;
        if (!submitStorage(Kind::SaveNewsPage,
                services::SteamGameUpdatesService::saveNewsPage(
                    pending.appId, std::move(articles), currentTimestamp()), save)) {
            recordRefreshCheck(pending.appId, pending.attemptedAtUtc, false,
                               QStringLiteral("Steam news could not be saved to this profile."),
                               pending.generation);
        }
        return;
    }
    if (pending.kind == Kind::ArtworkStage) {
        if (error.isError() || error.isCancelled()) {
            finishArtwork(pending.appId, pending.artworkUrl);
            return;
        }
        services::StagedSteamArtwork artwork = stagedArtworkFromMap(values);
        if (!submitStorage(Kind::ArtworkSave,
                services::SteamGameUpdatesService::saveArtwork(
                    m_graph->profileDirectory(), pending.appId, pending.artworkUrl,
                    artwork), pending)) {
            services::SteamArtworkStore::discard(m_graph->profileDirectory(), artwork);
            finishArtwork(pending.appId, pending.artworkUrl);
        }
    }
}

void SteamGameUpdatesController::onStorageFinished(
    quint64 id, const QVariantMap &values, domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Pending pending = it.value();
    m_pending.erase(it);
    switch (pending.kind) {
    case Kind::OldestNewsTimestamp:
        if (pending.generation != m_refreshGeneration || !m_refreshBusy) return;
        if (error.isError() || error.isCancelled()) {
            m_refreshBusy = false;
            m_refreshStatus = QStringLiteral("Could not load older Steam news.");
            m_errorMessage = QStringLiteral("The saved news cache could not be searched.");
            emit stateChanged();
            return;
        }
        {
            const qint64 oldest = values.value(
                QStringLiteral("publishedAtUnixSeconds")).toLongLong();
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            const qint64 cursor = oldest > 0
                ? (oldest < now ? oldest + 1 : now) : now;
            m_refreshBusy = false;
            startRefresh({pending.appId}, cursor);
        }
        return;
    case Kind::Snapshot:
        m_snapshotBusy = false;
        if (error.isError() || error.isCancelled()) {
            m_errorMessage = QStringLiteral("Saved Game Updates games could not be loaded.");
        } else {
            m_games = values.value(QStringLiteral("games")).toList();
            m_snapshotLoaded = true;
            m_errorMessage.clear();
            for (const QVariant &game : m_games)
                queueArtwork(game.toMap().value(QStringLiteral("appId")).toUInt());
            reloadNews();
        }
        emit stateChanged();
        return;
    case Kind::AddInsert:
        m_addBusy = false;
        m_reservedAppIds.remove(pending.appId);
        if (error.isError() || error.isCancelled()) {
            m_addError = error.category() == domain::ErrorCategory::Conflict
                ? QStringLiteral("This game is already in Game Updates.")
                : QStringLiteral("The game could not be added to Game Updates.");
        } else {
            const QVariantMap game = values.value(QStringLiteral("game")).toMap();
            updateGame(game);
            queueArtwork(pending.appId);
            m_addError.clear();
            m_notification = QStringLiteral("Following news for %1.")
                .arg(game.value(QStringLiteral("name")).toString());
            reloadNews();
        }
        emit stateChanged();
        return;
    case Kind::RemoveFollow:
        m_removeBusy = false;
        if (error.isError() || error.isCancelled()) {
            m_errorMessage = QStringLiteral("The game could not be removed from Game Updates.");
        } else {
            const int index = gameIndex(pending.appId);
            if (index >= 0) {
                const QString filename = m_games.at(index).toMap()
                    .value(QStringLiteral("artworkFilename")).toString();
                if (!filename.isEmpty()) emit artworkInvalidated(filename);
                m_games.removeAt(index);
            }
            if (m_selectedAppId == pending.appId) {
                m_selectedAppId = 0;
                m_newsAppFilter.reset();
            }
            m_notification = QStringLiteral("Game removed from Game Updates.");
            reloadNews();
        }
        emit stateChanged();
        return;
    case Kind::SetOwnership:
        if (error.isError() || error.isCancelled())
            m_errorMessage = QStringLiteral("The ownership label could not be saved.");
        else
            updateGame(values.value(QStringLiteral("game")).toMap());
        emit stateChanged();
        return;
    case Kind::NewsPage:
        if (pending.generation != m_newsGeneration) return;
        m_newsLoading = false;
        if (error.isError() || error.isCancelled()) {
            m_newsError = QStringLiteral("Cached Game Updates articles could not be loaded.");
            emit stateChanged();
            return;
        }
        {
            const QVariantList items = values.value(QStringLiteral("items")).toList();
            if (!pending.appendPage) m_newsItems.clear();
            for (const QVariant &item : items) m_newsItems.push_back(item);
            m_newsHasMore = values.value(QStringLiteral("hasMore")).toBool();
            m_newsCursor = {};
            if (m_newsHasMore) {
                const QVariantMap cursor = values.value(QStringLiteral("nextCursor")).toMap();
                m_newsCursor.publishedAtUnixSeconds = cursor.value(
                    QStringLiteral("publishedAtUnixSeconds")).toLongLong();
                m_newsCursor.appId = cursor.value(QStringLiteral("appId")).toUInt();
                m_newsCursor.newsId = cursor.value(QStringLiteral("newsId")).toString();
            }
            m_newsError.clear();
        }
        emit stateChanged();
        return;
    case Kind::MarkRead:
        m_newsMutationBusy = false;
        if (error.isError() || error.isCancelled()) {
            m_newsError = QStringLiteral("The article read state could not be saved.");
        } else {
            setGameUnreadCount(pending.appId,
                values.value(QStringLiteral("unreadNewsCount")).toInt());
            reloadNews();
        }
        emit stateChanged();
        return;
    case Kind::MarkAllRead:
        m_newsMutationBusy = false;
        if (error.isError() || error.isCancelled()) {
            m_newsError = QStringLiteral("The articles could not be marked as read.");
        } else {
            setGameUnreadCount(pending.appId,
                values.value(QStringLiteral("unreadNewsCount")).toInt());
            reloadNews();
        }
        emit stateChanged();
        return;
    case Kind::SaveNewsPage:
        if (pending.generation != m_refreshGeneration || !m_refreshBusy) return;
        if (error.isError() || error.isCancelled()) {
            recordRefreshCheck(pending.appId, pending.attemptedAtUtc, false,
                               QStringLiteral("Steam news was received but could not be saved."),
                               pending.generation);
        } else {
            recordRefreshCheck(pending.appId, pending.attemptedAtUtc, true, {},
                               pending.generation);
        }
        return;
    case Kind::RecordNewsCheck:
        if (pending.generation != m_refreshGeneration || !m_refreshBusy) return;
        if (!error.isError() && !error.isCancelled())
            updateGame(values.value(QStringLiteral("game")).toMap());
        else
            setLocalRefreshFailure(pending.appId, pending.attemptedAtUtc,
                pending.safeError.isEmpty()
                    ? QStringLiteral("The refresh result could not be saved to this profile.")
                    : pending.safeError);
        completeRefreshItem(pending.appId,
                            pending.success && !error.isError() && !error.isCancelled(),
                            pending.generation);
        return;
    case Kind::ArtworkSave:
        if (!error.isError() && !error.isCancelled()) {
            const QVariantMap game = values.value(QStringLiteral("game")).toMap();
            updateGame(game);
            const QString filename = game.value(QStringLiteral("artworkFilename")).toString();
            if (!filename.isEmpty()) emit artworkInvalidated(filename);
        }
        finishArtwork(pending.appId, pending.artworkUrl);
        emit stateChanged();
        return;
    default:
        return;
    }
}

void SteamGameUpdatesController::cancelPending(quint64 id)
{
    if (!id) return;
    m_pending.remove(id);
    m_graph->cancelOperation({id});
}

void SteamGameUpdatesController::cancelAll()
{
    const auto ids = m_pending.keys();
    for (quint64 id : ids) cancelPending(id);
    m_pending.clear();
    m_reservedAppIds.clear();
    m_refreshQueue.clear();
    m_refreshCompletedAppIds.clear();
    m_queuedArtworkUrls.clear();
    m_activeArtworkUrls.clear();
    m_activeRefreshRequests = 0;
    m_activeArtworkRequests = 0;
    m_refreshBusy = false;
    m_addBusy = false;
    m_removeBusy = false;
    m_newsLoading = false;
    m_newsMutationBusy = false;
    m_snapshotBusy = false;
}

void SteamGameUpdatesController::updateGame(QVariantMap game)
{
    const quint32 appId = game.value(QStringLiteral("appId")).toUInt();
    const int index = gameIndex(appId);
    if (index < 0) m_games.push_back(std::move(game));
    else m_games[index] = std::move(game);
    emit stateChanged();
}

void SteamGameUpdatesController::setGameUnreadCount(quint32 appId, int count)
{
    const int index = gameIndex(appId);
    if (index < 0) return;
    QVariantMap game = m_games.at(index).toMap();
    game.insert(QStringLiteral("unreadNewsCount"), qMax(0, count));
    m_games[index] = game;
    emit stateChanged();
}

int SteamGameUpdatesController::gameIndex(quint32 appId) const
{
    for (int index = 0; index < m_games.size(); ++index) {
        if (m_games.at(index).toMap().value(QStringLiteral("appId")).toUInt() == appId)
            return index;
    }
    return -1;
}

void SteamGameUpdatesController::queueArtwork(quint32 appId)
{
    const int index = gameIndex(appId);
    if (index < 0) return;
    const QVariantMap game = m_games.at(index).toMap();
    if (!game.value(QStringLiteral("artworkFilename")).toString().isEmpty()) return;
    const QString url = domain::normalizeSteamArtworkUrl(
        game.value(QStringLiteral("artworkUrl")).toString());
    if (url.isEmpty() || m_queuedArtworkUrls.value(appId) == url
        || m_activeArtworkUrls.value(appId) == url) return;
    m_queuedArtworkUrls.insert(appId, url);
    m_artworkQueue.enqueue({appId, url});
    dispatchArtworkQueue();
}

void SteamGameUpdatesController::dispatchArtworkQueue()
{
    if (!m_ready) return;
    while (m_activeArtworkRequests < kMaximumArtworkRequests
           && !m_artworkQueue.isEmpty()) {
        const ArtworkWork work = m_artworkQueue.dequeue();
        if (m_queuedArtworkUrls.value(work.appId) != work.url) continue;
        m_queuedArtworkUrls.remove(work.appId);
        m_activeArtworkUrls.insert(work.appId, work.url);
        Pending pending;
        pending.appId = work.appId;
        pending.artworkUrl = work.url;
        const providers::ProviderRequest request{
            providers::ProviderId::SteamArtwork, QUrl(work.url)};
        ++m_activeArtworkRequests;
        if (!submitNetwork(Kind::ArtworkNetwork, request, pending)) {
            finishArtwork(work.appId, work.url);
            continue;
        }
    }
}

void SteamGameUpdatesController::finishArtwork(quint32 appId, const QString &url)
{
    if (m_activeArtworkUrls.value(appId) == url) {
        m_activeArtworkUrls.remove(appId);
        m_activeArtworkRequests = qMax(0, m_activeArtworkRequests - 1);
    }
    if (m_queuedArtworkUrls.value(appId) == url) m_queuedArtworkUrls.remove(appId);
    dispatchArtworkQueue();
}

QString SteamGameUpdatesController::safeNewsError(domain::AppError error) const
{
    switch (error.category()) {
    case domain::ErrorCategory::Connection:
        return QStringLiteral("Could not connect to Steam News.");
    case domain::ErrorCategory::Timeout:
        return QStringLiteral("Steam News took too long to respond.");
    case domain::ErrorCategory::Http:
    case domain::ErrorCategory::Provider:
        return QStringLiteral("Steam could not provide this game's news.");
    case domain::ErrorCategory::InvalidResponse:
    case domain::ErrorCategory::ResponseTooLarge:
        return QStringLiteral("Steam returned news that could not be read safely.");
    case domain::ErrorCategory::CapabilityUnavailable:
        return QStringLiteral("Steam networking is unavailable.");
    case domain::ErrorCategory::Busy:
        return QStringLiteral("Steam updates are busy. Try again shortly.");
    default:
        return QStringLiteral("This game's news could not be refreshed.");
    }
}

void SteamGameUpdatesController::setLocalRefreshFailure(
    quint32 appId, const QString &attemptedAtUtc, const QString &error)
{
    const int index = gameIndex(appId);
    if (index < 0) return;
    QVariantMap game = m_games.at(index).toMap();
    game.insert(QStringLiteral("lastAttemptedNewsCheckAtUtc"), attemptedAtUtc);
    game.insert(QStringLiteral("refreshState"), QStringLiteral("failed"));
    game.insert(QStringLiteral("lastRefreshError"), error);
    m_games[index] = game;
}

QVariantMap SteamGameUpdatesController::articleMap(
    const domain::SteamNewsArticle &article)
{
    return {{QStringLiteral("appId"), article.appId},
            {QStringLiteral("newsId"), article.newsId},
            {QStringLiteral("title"), article.title},
            {QStringLiteral("publishedAtUnixSeconds"), article.publishedAtUnixSeconds},
            {QStringLiteral("sourceUrl"), article.sourceUrl},
            {QStringLiteral("author"), article.author},
            {QStringLiteral("feedLabel"), article.feedLabel},
            {QStringLiteral("feedName"), article.feedName},
            {QStringLiteral("feedType"), article.feedType
                 ? QVariant(*article.feedType) : QVariant()},
            {QStringLiteral("contentText"), article.contentText},
            {QStringLiteral("externalUrl"), article.externalUrl}};
}

domain::SteamNewsArticle SteamGameUpdatesController::articleFromMap(
    const QVariantMap &map)
{
    domain::SteamNewsArticle article;
    article.appId = map.value(QStringLiteral("appId")).toUInt();
    article.newsId = map.value(QStringLiteral("newsId")).toString();
    article.title = map.value(QStringLiteral("title")).toString();
    article.publishedAtUnixSeconds = map.value(
        QStringLiteral("publishedAtUnixSeconds")).toLongLong();
    article.sourceUrl = map.value(QStringLiteral("sourceUrl")).toString();
    article.author = map.value(QStringLiteral("author")).toString();
    article.feedLabel = map.value(QStringLiteral("feedLabel")).toString();
    article.feedName = map.value(QStringLiteral("feedName")).toString();
    if (map.value(QStringLiteral("feedType")).isValid()
        && !map.value(QStringLiteral("feedType")).isNull()) {
        article.feedType = map.value(QStringLiteral("feedType")).toInt();
    }
    article.contentText = map.value(QStringLiteral("contentText")).toString();
    article.externalUrl = map.value(QStringLiteral("externalUrl")).toBool();
    return article;
}

QString SteamGameUpdatesController::currentTimestamp()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

} // namespace something::controllers

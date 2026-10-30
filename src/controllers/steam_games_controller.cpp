#include "steam_games_controller.h"

#include "../domain/steam_game.h"
#include "../providers/feature_parsers.h"
#include "../services/steam_service.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QThread>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace something::controllers {
namespace {

using namespace something;
constexpr int kMaximumSteamRefreshRequests = 4;

QString safeProviderMessage(domain::AppError error, const QString &fallback)
{
    switch (error.category()) {
    case domain::ErrorCategory::Validation:
        return QStringLiteral("Steam returned data that did not match a tracked game.");
    case domain::ErrorCategory::Connection:
        return QStringLiteral("Could not connect to Steam. Try again later.");
    case domain::ErrorCategory::Timeout:
        return QStringLiteral("Steam took too long to respond. Try again later.");
    case domain::ErrorCategory::Http:
    case domain::ErrorCategory::Provider:
        return QStringLiteral("Steam could not provide this game's details right now.");
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

} // namespace

int SteamGamesController::stalePriceCount() const
{
    int count = 0;
    for (int row = 0; row < m_games.count(); ++row) {
        const QString checkedCountry = m_games.rowAt(row)
            .value(QStringLiteral("priceCountryCode")).toString();
        if (!checkedCountry.isEmpty() && checkedCountry != m_countryCode) ++count;
    }
    return count;
}

SteamGamesController::SteamGamesController(application::ApplicationServiceGraph *graph,
                                           QObject *parent)
    : QObject(parent), m_graph(graph), m_games(this),
      m_countries(services::SteamService::countryValues())
{
    Q_ASSERT(m_graph);
    Q_ASSERT(QThread::currentThread() == thread());
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &SteamGamesController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::httpsFinished,
            this, &SteamGamesController::onHttpsFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &SteamGamesController::onValueJobFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &SteamGamesController::onLifecycleChanged);
    onLifecycleChanged(m_graph->lifecycle().state());
}

void SteamGamesController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    if (state == application::ApplicationLifecycle::State::Running) {
        if (!m_ready) {
            m_ready = true;
            if (!m_autoRefreshStarted) {
                m_autoRefreshStarted = true;
                requestSnapshot(true);
            }
        }
        return;
    }
    if (state == application::ApplicationLifecycle::State::ShuttingDown
        || state == application::ApplicationLifecycle::State::Failed
        || state == application::ApplicationLifecycle::State::Stopped) {
        m_ready = false;
        cancelAll();
        m_snapshotBusy = false;
        m_addBusy = false;
        m_refreshBusy = false;
        m_countryBusy = false;
        m_deleteBusy = false;
        emit stateChanged();
    }
}

void SteamGamesController::requestSnapshot(bool autoRefresh, bool manualRefreshAfterLoad)
{
    if (!m_ready) return;
    if (m_snapshotBusy) {
        m_manualRefreshAfterLoad = m_manualRefreshAfterLoad || manualRefreshAfterLoad;
        return;
    }
    m_snapshotBusy = true;
    m_errorMessage.clear();
    Pending pending;
    pending.kind = Kind::Snapshot;
    pending.autoRefreshAfterSnapshot = autoRefresh;
    pending.manualRefreshAfterSnapshot = manualRefreshAfterLoad;
    const quint64 id = submitStorage(Kind::Snapshot,
        services::SteamService::loadSnapshot(m_graph->profileDirectory()), pending);
    if (!id) {
        m_snapshotBusy = false;
        m_errorMessage = QStringLiteral("Saved Steam games could not be loaded.");
    }
    emit stateChanged();
}

quint64 SteamGamesController::submitStorage(Kind kind, storage::StorageCommand command,
                                            Pending pending)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("Steam storage operation"));
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

quint64 SteamGamesController::submitNetwork(Kind kind, providers::ProviderRequest request,
                                           Pending pending)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("Steam provider request"));
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

quint64 SteamGamesController::submitParse(Kind kind, QByteArray body, Pending pending)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("parse Steam Store response"));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    pending.kind = kind;
    m_pending.insert(id, pending);
    const quint32 expectedAppId = pending.appId;
    const bool submitted = m_graph->submitExpensiveValue(operation,
        [body = std::move(body), expectedAppId](const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            if (token.isCancellationRequested()) {
                result.error = domain::AppError::cancelled();
                return result;
            }
            const auto parsed = providers::parseSteamGameDetails(body, expectedAppId);
            if (!parsed.succeeded()) {
                result.error = parsed.error;
                return result;
            }
            result.values = SteamGamesController::detailsToMap(parsed.value);
            return result;
        });
    if (!submitted) {
        m_pending.remove(id);
        return 0;
    }
    return id;
}

quint64 SteamGamesController::submitArtworkStage(QByteArray body, Pending pending)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("validate Steam artwork"));
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

QUrl SteamGamesController::appDetailsUrl(quint32 appId, const QString &countryCode)
{
    QUrl url(QStringLiteral("https://store.steampowered.com/api/appdetails"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("appids"), QString::number(appId));
    query.addQueryItem(QStringLiteral("cc"), countryCode);
    query.addQueryItem(QStringLiteral("l"), QStringLiteral("english"));
    url.setQuery(query);
    return url;
}

QString SteamGamesController::currentTimestamp()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

void SteamGamesController::setSelectedCountry(QString countryCode)
{
    countryCode = countryCode.trimmed().toUpper();
    if (!domain::isSupportedSteamCountry(countryCode) || countryCode == m_selectedCountry) return;
    m_selectedCountry = std::move(countryCode);
    m_errorMessage.clear();
    emit settingsChanged();
}

void SteamGamesController::saveCountry()
{
    if (!m_ready || m_countryBusy || m_refreshBusy || !domain::isSupportedSteamCountry(m_selectedCountry))
        return;
    if (m_selectedCountry == m_countryCode) return;
    m_countryBusy = true;
    m_errorMessage.clear();
    Pending pending;
    pending.kind = Kind::CountrySave;
    pending.countryCode = m_selectedCountry;
    if (!submitStorage(Kind::CountrySave,
                       services::SteamService::setCountry(m_selectedCountry), pending)) {
        m_countryBusy = false;
        m_errorMessage = QStringLiteral("The Steam country setting could not be saved.");
    }
    emit stateChanged();
}

void SteamGamesController::addGame(QString storeUrl)
{
    if (!m_ready || m_addBusy || !m_snapshotLoaded) return;
    const domain::SteamStoreUrl normalized = domain::normalizeSteamStoreUrl(std::move(storeUrl));
    if (!normalized.isValid()) {
        m_addError = QStringLiteral("Enter an HTTP or HTTPS Steam Store /app/<id> URL.");
        emit stateChanged();
        return;
    }
    for (int row = 0; row < m_games.count(); ++row) {
        if (m_games.rowAt(row).value(QStringLiteral("steamAppId")).toUInt() == normalized.appId) {
            m_addError = QStringLiteral("This Steam game is already being tracked.");
            emit stateChanged();
            return;
        }
    }
    if (m_reservedAppIds.contains(normalized.appId)) {
        m_addError = QStringLiteral("This Steam game is already being added.");
        emit stateChanged();
        return;
    }
    m_reservedAppIds.insert(normalized.appId);
    m_addBusy = true;
    m_addError.clear();
    m_notification.clear();
    Pending pending;
    pending.kind = Kind::AddNetwork;
    pending.appId = normalized.appId;
    pending.canonicalUrl = normalized.canonicalUrl;
    pending.countryCode = m_countryCode;
    providers::ProviderRequest request{providers::ProviderId::SteamStore,
                                       appDetailsUrl(normalized.appId, m_countryCode)};
    if (!submitNetwork(Kind::AddNetwork, std::move(request), pending)) {
        releaseAddReservation(normalized.appId);
        m_addBusy = false;
        m_addError = QStringLiteral("Steam details could not be requested right now.");
    }
    emit stateChanged();
}

void SteamGamesController::refreshPrices()
{
    if (!m_ready || m_refreshBusy || m_countryBusy || m_deleteBusy) return;
    if (!m_snapshotLoaded) {
        m_manualRefreshAfterLoad = true;
        requestSnapshot(false, true);
        return;
    }
    beginRefresh();
}

void SteamGamesController::beginRefresh()
{
    if (!m_ready || m_refreshBusy || m_countryBusy || m_deleteBusy) return;
    ++m_refreshGeneration;
    m_refreshCountry = m_countryCode;
    m_refreshWork.clear();
    m_refreshUpdates.clear();
    m_refreshFinished.clear();
    m_nextRefreshIndex = 0;
    m_activeRefreshRequests = 0;
    m_refreshFinishedCount = 0;
    m_refreshUpdatedCount = 0;
    m_refreshFailedCount = 0;
    m_refreshCommitPending = false;
    m_errorMessage.clear();
    m_refreshStatus.clear();
    for (int row = 0; row < m_games.count(); ++row)
        m_refreshWork.push_back({m_games.rowAt(row), {}});
    if (m_refreshWork.isEmpty()) {
        m_refreshStatus = QStringLiteral("There are no tracked games to refresh.");
        emit stateChanged();
        return;
    }
    m_refreshUpdates.resize(m_refreshWork.size());
    m_refreshFinished.fill(false, m_refreshWork.size());
    m_refreshBusy = true;
    m_refreshStatus = QStringLiteral("Checking 0 of %1 games…").arg(m_refreshWork.size());
    emit stateChanged();
    dispatchRefreshQueue();
}

void SteamGamesController::dispatchRefreshQueue()
{
    if (!m_ready || !m_refreshBusy || m_refreshCommitPending) return;
    while (m_activeRefreshRequests < kMaximumSteamRefreshRequests
           && m_nextRefreshIndex < m_refreshWork.size()) {
        const int index = m_nextRefreshIndex++;
        RefreshWork &work = m_refreshWork[index];
        work.attemptedAt = currentTimestamp();
        const QVariantMap game = work.game;
        const quint32 appId = game.value(QStringLiteral("steamAppId")).toUInt();
        Pending pending;
        pending.kind = Kind::RefreshNetwork;
        pending.appId = appId;
        pending.gameId = game.value(QStringLiteral("id")).toLongLong();
        pending.refreshIndex = index;
        pending.refreshGeneration = m_refreshGeneration;
        pending.attemptedAt = work.attemptedAt;
        pending.countryCode = m_refreshCountry;
        pending.game = game;
        ++m_activeRefreshRequests;
        providers::ProviderRequest request{providers::ProviderId::SteamStore,
                                           appDetailsUrl(appId, m_refreshCountry), true};
        if (!submitNetwork(Kind::RefreshNetwork, std::move(request), pending)) {
            completeRefreshItem(index, nullptr,
                QStringLiteral("Steam price lookup could not be started."));
        }
    }
    if (m_nextRefreshIndex >= m_refreshWork.size() && m_activeRefreshRequests == 0)
        finishRefreshBatch();
}

void SteamGamesController::completeRefreshItem(int index,
                                               const domain::SteamGameDetails *details,
                                               QString safeError)
{
    if (index < 0 || index >= m_refreshWork.size() || m_refreshFinished.at(index)) return;
    if (!m_refreshBusy || !m_ready) return;
    m_refreshFinished[index] = true;
    const RefreshWork &work = m_refreshWork.at(index);
    const QVariantMap &current = work.game;
    storage::SteamRefreshUpdate update;
    update.gameId = current.value(QStringLiteral("id")).toLongLong();
    update.attemptedAt = work.attemptedAt;
    if (details) {
        storage::SteamGame refreshed;
        refreshed.id = update.gameId;
        refreshed.steamAppId = current.value(QStringLiteral("steamAppId")).toLongLong();
        refreshed.storeUrl = current.value(QStringLiteral("storeUrl")).toString();
        refreshed.name = details->name;
        refreshed.imageSourceUrl = details->artworkUrl;
        refreshed.priceStatus = domain::steamPriceStateName(details->priceState);
        refreshed.currency = details->currency;
        refreshed.regularPriceMinor = details->regularPriceMinor;
        refreshed.currentPriceMinor = details->currentPriceMinor;
        refreshed.discountPercent = details->discountPercent;
        refreshed.priceCountryCode = m_refreshCountry;
        update.refreshedGame = std::move(refreshed);
        ++m_refreshUpdatedCount;
    } else {
        update.safeError = safeError.isEmpty()
            ? QStringLiteral("Steam could not refresh this game's price.") : std::move(safeError);
        ++m_refreshFailedCount;
    }
    m_refreshUpdates[index] = std::move(update);
    ++m_refreshFinishedCount;
    m_refreshStatus = QStringLiteral("Checking %1 of %2 games…")
        .arg(m_refreshFinishedCount).arg(m_refreshWork.size());
    if (m_activeRefreshRequests > 0) --m_activeRefreshRequests;
    emit stateChanged();
}

void SteamGamesController::finishRefreshBatch()
{
    if (!m_refreshBusy || m_refreshCommitPending) return;
    if (m_refreshFinishedCount != m_refreshWork.size()) return;
    m_refreshCommitPending = true;
    Pending pending;
    pending.kind = Kind::RefreshCommit;
    pending.refreshGeneration = m_refreshGeneration;
    if (!submitStorage(Kind::RefreshCommit,
            services::SteamService::applyRefresh(m_graph->profileDirectory(), m_refreshUpdates),
            pending)) {
        m_refreshCommitPending = false;
        m_refreshBusy = false;
        m_errorMessage = QStringLiteral("Steam prices could not be saved. Cached prices were kept.");
        emit stateChanged();
    }
}

void SteamGamesController::deleteGame(qint64 gameId)
{
    if (!m_ready || m_deleteBusy || m_refreshBusy || m_games.rowForId(gameId) < 0) return;
    m_deleteBusy = true;
    m_errorMessage.clear();
    Pending pending;
    pending.kind = Kind::Delete;
    pending.gameId = gameId;
    pending.game = m_games.rowAt(m_games.rowForId(gameId));
    if (!submitStorage(Kind::Delete,
            services::SteamService::deleteGame(m_graph->profileDirectory(), gameId), pending)) {
        m_deleteBusy = false;
        m_errorMessage = QStringLiteral("The game could not be removed from the list.");
    }
    emit stateChanged();
}

void SteamGamesController::openStore(qint64 gameId)
{
    const int row = m_games.rowForId(gameId);
    if (row < 0) return;
    const QVariantMap game = m_games.rowAt(row);
    const domain::SteamStoreUrl normalized = domain::normalizeSteamStoreUrl(
        game.value(QStringLiteral("storeUrl")).toString());
    if (!normalized.isValid()
        || normalized.appId != game.value(QStringLiteral("steamAppId")).toUInt()
        || !QDesktopServices::openUrl(QUrl(normalized.canonicalUrl)))
        m_errorMessage = QStringLiteral("The Steam Store page could not be opened.");
    else
        m_errorMessage.clear();
    emit stateChanged();
}

void SteamGamesController::retryLoad()
{
    if (m_snapshotBusy || !m_ready) return;
    requestSnapshot(!m_snapshotLoaded && m_autoRefreshStarted,
                    m_manualRefreshAfterLoad);
}

void SteamGamesController::clearNotification()
{
    if (m_notification.isEmpty()) return;
    m_notification.clear();
    emit stateChanged();
}

void SteamGamesController::onHttpsFinished(quint64 id, const QByteArray &body,
                                           domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Pending pending = it.value();
    m_pending.erase(it);
    switch (pending.kind) {
    case Kind::AddNetwork:
        if (error.isError()) {
            m_addError = safeProviderMessage(error, QStringLiteral("Steam game details could not be loaded."));
            releaseAddReservation(pending.appId);
            m_addBusy = false;
            emit stateChanged();
            return;
        }
        if (!submitParse(Kind::AddParse, body, pending)) {
            m_addError = QStringLiteral("Steam game details could not be read.");
            releaseAddReservation(pending.appId);
            m_addBusy = false;
            emit stateChanged();
        }
        return;
    case Kind::RefreshNetwork:
        if (!m_refreshBusy || pending.refreshGeneration != m_refreshGeneration) return;
        if (error.isError()) {
            completeRefreshItem(pending.refreshIndex, nullptr, safeRefreshError(error));
            dispatchRefreshQueue();
            return;
        }
        if (!submitParse(Kind::RefreshParse, body, pending)) {
            completeRefreshItem(pending.refreshIndex, nullptr,
                                QStringLiteral("Steam returned unreadable game data."));
            dispatchRefreshQueue();
        }
        return;
    case Kind::ArtworkNetwork:
        if (m_artworkGenerationByGame.value(pending.gameId) != pending.artworkGeneration)
            return;
        if (!error.isError() && !submitArtworkStage(body, pending))
            emit stateChanged();
        return;
    default:
        return;
    }
}

void SteamGamesController::onValueJobFinished(quint64 id, const QVariantMap &values,
                                              domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Pending pending = it.value();
    m_pending.erase(it);
    if (pending.kind == Kind::AddParse) {
        if (error.isError()) {
            m_addError = safeProviderMessage(error, QStringLiteral("Steam returned invalid game details."));
            releaseAddReservation(pending.appId);
            m_addBusy = false;
            emit stateChanged();
            return;
        }
        const domain::SteamGameDetails details = detailsFromMap(values);
        if (details.appId != pending.appId) {
            m_addError = QStringLiteral("Steam returned details for a different game.");
            releaseAddReservation(pending.appId);
            m_addBusy = false;
            emit stateChanged();
            return;
        }
        Pending insert = pending;
        insert.kind = Kind::AddInsert;
        if (!submitStorage(Kind::AddInsert,
                services::SteamService::addGame(m_graph->profileDirectory(),
                    pending.canonicalUrl, pending.countryCode, details, currentTimestamp()), insert)) {
            m_addError = QStringLiteral("The game details were loaded but could not be saved.");
            releaseAddReservation(pending.appId);
            m_addBusy = false;
            emit stateChanged();
        }
        return;
    }
    if (pending.kind == Kind::RefreshParse) {
        if (!m_refreshBusy || pending.refreshGeneration != m_refreshGeneration) return;
        if (error.isError()) {
            completeRefreshItem(pending.refreshIndex, nullptr, safeRefreshError(error));
        } else {
            const domain::SteamGameDetails details = detailsFromMap(values);
            if (details.appId != pending.appId)
                completeRefreshItem(pending.refreshIndex, nullptr,
                                    QStringLiteral("Steam returned a different game."));
            else
                completeRefreshItem(pending.refreshIndex, &details, {});
        }
        dispatchRefreshQueue();
        return;
    }
    if (pending.kind == Kind::ArtworkStage) {
        if (error.isError() || m_artworkGenerationByGame.value(pending.gameId)
                != pending.artworkGeneration) return;
        services::StagedSteamArtwork artwork;
        artwork.asset.filename = values.value(QStringLiteral("filename")).toString();
        artwork.asset.mimeType = values.value(QStringLiteral("mimeType")).toString();
        artwork.asset.byteSize = values.value(QStringLiteral("byteSize")).toLongLong();
        artwork.stagingFilename = values.value(QStringLiteral("stagingFilename")).toString();
        artwork.stagedNewFile = values.value(QStringLiteral("stagedNewFile")).toBool();
        Pending save = pending;
        save.kind = Kind::ArtworkSave;
        if (!submitStorage(Kind::ArtworkSave,
                services::SteamService::saveArtwork(m_graph->profileDirectory(),
                                                    pending.gameId, artwork), save)) {
            services::SteamArtworkStore::discard(m_graph->profileDirectory(), artwork);
        }
    }
}

void SteamGamesController::onStorageFinished(quint64 id, const QVariantMap &values,
                                             domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Pending pending = it.value();
    m_pending.erase(it);
    switch (pending.kind) {
    case Kind::Snapshot: {
        m_snapshotBusy = false;
        if (error.isError()) {
            setSafeError(error, QStringLiteral("Saved Steam games could not be loaded."));
            emit stateChanged();
            return;
        }
        m_games.replace(values.value(QStringLiteral("games")).toList());
        m_countryCode = values.value(QStringLiteral("countryCode")).toString();
        if (!domain::isSupportedSteamCountry(m_countryCode)) m_countryCode = QStringLiteral("TR");
        m_selectedCountry = m_countryCode;
        m_countries = values.value(QStringLiteral("countries")).toList();
        if (m_countries.isEmpty()) m_countries = services::SteamService::countryValues();
        m_snapshotLoaded = true;
        m_errorMessage.clear();
        emit settingsChanged();
        if (pending.autoRefreshAfterSnapshot) beginRefresh();
        else if (pending.manualRefreshAfterSnapshot || m_manualRefreshAfterLoad) {
            m_manualRefreshAfterLoad = false;
            beginRefresh();
        }
        emit stateChanged();
        return;
    }
    case Kind::CountrySave:
        m_countryBusy = false;
        if (error.isError()) setSafeError(error, QStringLiteral("Steam country could not be saved."));
        else {
            m_countryCode = values.value(QStringLiteral("countryCode")).toString();
            m_selectedCountry = m_countryCode;
            m_errorMessage.clear();
            m_notification = QStringLiteral("New Steam price checks will use %1.").arg(m_countryCode);
            emit settingsChanged();
        }
        emit stateChanged();
        return;
    case Kind::AddInsert:
        m_addBusy = false;
        releaseAddReservation(pending.appId);
        if (error.isError()) {
            m_addError = error.category() == domain::ErrorCategory::Conflict
                ? QStringLiteral("This Steam game is already being tracked.")
                : QStringLiteral("The game could not be saved.");
        } else {
            const QVariantMap game = values.value(QStringLiteral("game")).toMap();
            m_games.append(game);
            m_addError.clear();
            m_notification = QStringLiteral("Added %1 to tracked games.")
                .arg(game.value(QStringLiteral("name")).toString());
            const domain::SteamGameDetails details = detailsFromMap(
                {{QStringLiteral("appId"), pending.appId},
                 {QStringLiteral("name"), game.value(QStringLiteral("name"))},
                 {QStringLiteral("artworkUrl"), game.value(QStringLiteral("imageSourceUrl"))},
                 {QStringLiteral("priceState"), game.value(QStringLiteral("priceStatus"))},
                 {QStringLiteral("currency"), game.value(QStringLiteral("currency"))},
                 {QStringLiteral("regularPriceMinor"), game.value(QStringLiteral("regularPriceMinor"))},
                 {QStringLiteral("hasRegularPrice"), game.contains(QStringLiteral("regularPriceMinor"))},
                 {QStringLiteral("currentPriceMinor"), game.value(QStringLiteral("currentPriceMinor"))},
                 {QStringLiteral("hasCurrentPrice"), game.contains(QStringLiteral("currentPriceMinor"))},
                 {QStringLiteral("discountPercent"), game.value(QStringLiteral("discountPercent"))}});
            if (!details.artworkUrl.isEmpty()) queueArtwork(game.value(QStringLiteral("id")).toLongLong(),
                                                            pending.appId, details.artworkUrl);
        }
        emit stateChanged();
        return;
    case Kind::Delete:
        m_deleteBusy = false;
        if (error.isError()) setSafeError(error, QStringLiteral("The game could not be removed."));
        else {
            m_games.removeById(pending.gameId);
            const QString oldArtwork = pending.game.value(QStringLiteral("artworkSource")).toString();
            const QString filename = oldArtwork.startsWith(QStringLiteral("image://steam-artwork/"))
                ? oldArtwork.mid(QStringLiteral("image://steam-artwork/").size()) : QString();
            if (!filename.isEmpty()) emit artworkInvalidated(filename);
            m_errorMessage.clear();
            m_notification = QStringLiteral("Game removed from tracking.");
        }
        emit stateChanged();
        return;
    case Kind::RefreshCommit:
        if (pending.refreshGeneration != m_refreshGeneration) return;
        m_refreshCommitPending = false;
        m_refreshBusy = false;
        if (error.isError()) {
            m_errorMessage = QStringLiteral("Steam prices could not be saved. Cached prices were kept.");
            m_refreshStatus.clear();
            emit stateChanged();
            return;
        }
        m_games.replace(values.value(QStringLiteral("games")).toList());
        m_refreshStatus = QStringLiteral("Updated %1, failed %2 of %3 games.")
            .arg(m_refreshUpdatedCount).arg(m_refreshFailedCount).arg(m_refreshWork.size());
        for (int index = 0; index < m_refreshUpdates.size(); ++index) {
            const storage::SteamRefreshUpdate &update = m_refreshUpdates.at(index);
            if (!update.refreshedGame || update.refreshedGame->imageSourceUrl.isEmpty()) continue;
            const QVariantMap &previous = m_refreshWork.at(index).game;
            if (previous.value(QStringLiteral("artworkSource")).toString().isEmpty()
                || previous.value(QStringLiteral("imageSourceUrl")).toString()
                    != update.refreshedGame->imageSourceUrl) {
                queueArtwork(update.gameId,
                             static_cast<quint32>(update.refreshedGame->steamAppId),
                             update.refreshedGame->imageSourceUrl);
            }
        }
        emit stateChanged();
        return;
    case Kind::ArtworkSave:
        if (error.isError() || m_artworkGenerationByGame.value(pending.gameId)
                != pending.artworkGeneration) return;
        {
            const int row = m_games.rowForId(pending.gameId);
            if (row >= 0) {
                const QString oldSource = m_games.rowAt(row)
                    .value(QStringLiteral("artworkSource")).toString();
                const QString newSource = values.value(QStringLiteral("game")).toMap()
                    .value(QStringLiteral("artworkSource")).toString();
                if (oldSource != newSource
                    && oldSource.startsWith(QStringLiteral("image://steam-artwork/"))) {
                    emit artworkInvalidated(oldSource.mid(
                        QStringLiteral("image://steam-artwork/").size()));
                }
            }
        }
        m_games.update(values.value(QStringLiteral("game")).toMap());
        emit stateChanged();
        return;
    default:
        return;
    }
}

void SteamGamesController::queueArtwork(qint64 gameId, quint32 appId, const QString &url)
{
    const QString normalized = domain::normalizeSteamArtworkUrl(url);
    if (!m_ready || gameId <= 0 || appId == 0 || normalized.isEmpty()) return;
    cancelArtworkForGame(gameId);
    const quint64 generation = m_artworkGenerationByGame.value(gameId) + 1;
    m_artworkGenerationByGame.insert(gameId, generation);
    Pending pending;
    pending.kind = Kind::ArtworkNetwork;
    pending.gameId = gameId;
    pending.appId = appId;
    pending.artworkGeneration = generation;
    providers::ProviderRequest request{providers::ProviderId::SteamArtwork,
                                       QUrl(normalized)};
    if (!submitNetwork(Kind::ArtworkNetwork, std::move(request), pending)) return;
}

void SteamGamesController::cancelArtworkForGame(qint64 gameId)
{
    const auto ids = m_pending.keys();
    for (quint64 id : ids) {
        const Pending pending = m_pending.value(id);
        if (pending.gameId == gameId
            && (pending.kind == Kind::ArtworkNetwork || pending.kind == Kind::ArtworkStage
                || pending.kind == Kind::ArtworkSave)) cancelPending(id);
    }
}

void SteamGamesController::cancelPending(quint64 id)
{
    if (!id) return;
    m_pending.remove(id);
    m_graph->cancelOperation({id});
}

void SteamGamesController::cancelAll()
{
    const auto ids = m_pending.keys();
    for (quint64 id : ids) cancelPending(id);
    m_pending.clear();
    m_reservedAppIds.clear();
    m_refreshWork.clear();
    m_refreshUpdates.clear();
    m_refreshFinished.clear();
    m_activeRefreshRequests = 0;
    m_nextRefreshIndex = 0;
    m_refreshCommitPending = false;
    m_manualRefreshAfterLoad = false;
}

void SteamGamesController::releaseAddReservation(quint32 appId)
{
    m_reservedAppIds.remove(appId);
}

QString SteamGamesController::safeRefreshError(domain::AppError error)
{
    return safeProviderMessage(error, QStringLiteral("Steam price refresh failed."));
}

void SteamGamesController::setSafeError(domain::AppError error, const QString &fallback)
{
    if (error.category() == domain::ErrorCategory::Validation)
        m_errorMessage = QStringLiteral("Check the Steam country or game details and try again.");
    else if (error.category() == domain::ErrorCategory::CapabilityUnavailable)
        m_errorMessage = QStringLiteral("This Steam capability is unavailable right now.");
    else
        m_errorMessage = fallback;
}

domain::SteamGameDetails SteamGamesController::detailsFromMap(const QVariantMap &map)
{
    domain::SteamGameDetails details;
    details.appId = map.value(QStringLiteral("appId")).toUInt();
    details.name = map.value(QStringLiteral("name")).toString();
    details.artworkUrl = map.value(QStringLiteral("artworkUrl")).toString();
    const QString state = map.value(QStringLiteral("priceState")).toString();
    details.priceState = state == QStringLiteral("priced") ? domain::SteamPriceState::Priced
        : state == QStringLiteral("free") ? domain::SteamPriceState::Free
        : state == QStringLiteral("unsupported") ? domain::SteamPriceState::Unsupported
                                                   : domain::SteamPriceState::Unavailable;
    details.currency = map.value(QStringLiteral("currency")).toString();
    if (map.value(QStringLiteral("hasRegularPrice")).toBool())
        details.regularPriceMinor = map.value(QStringLiteral("regularPriceMinor")).toLongLong();
    if (map.value(QStringLiteral("hasCurrentPrice")).toBool())
        details.currentPriceMinor = map.value(QStringLiteral("currentPriceMinor")).toLongLong();
    details.discountPercent = map.value(QStringLiteral("discountPercent")).toInt();
    return details;
}

QVariantMap SteamGamesController::detailsToMap(const domain::SteamGameDetails &details)
{
    QVariantMap result{{QStringLiteral("appId"), details.appId},
                       {QStringLiteral("name"), details.name},
                       {QStringLiteral("artworkUrl"), details.artworkUrl},
                       {QStringLiteral("priceState"), domain::steamPriceStateName(details.priceState)},
                       {QStringLiteral("currency"), details.currency},
                       {QStringLiteral("discountPercent"), details.discountPercent},
                       {QStringLiteral("hasRegularPrice"), details.regularPriceMinor.has_value()},
                       {QStringLiteral("hasCurrentPrice"), details.currentPriceMinor.has_value()}};
    if (details.regularPriceMinor)
        result.insert(QStringLiteral("regularPriceMinor"), *details.regularPriceMinor);
    if (details.currentPriceMinor)
        result.insert(QStringLiteral("currentPriceMinor"), *details.currentPriceMinor);
    return result;
}

} // namespace something::controllers

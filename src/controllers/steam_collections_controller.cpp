#include "steam_collections_controller.h"

#include "../domain/steam_game_catalog.h"
#include "../providers/feature_parsers.h"
#include "../providers/steam_game_catalog_provider.h"
#include "../services/bounded_worker_pool.h"
#include "../services/steam_artwork_store.h"
#include "../services/steam_collections_service.h"

#include <QDesktopServices>
#include <QTimer>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace something::controllers {
namespace {

constexpr int kMaximumArtworkRequests = 3;
constexpr int kMaximumArtworkScansPerTurn = 32;

storage::StorageReply failed(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

std::optional<domain::SteamCollectionGameStatus> collectionStatus(const QString &status)
{
    if (status.isEmpty()) return std::nullopt;
    return domain::steamCollectionGameStatusFromName(status);
}

QString safeMutationError(const QString &mutation, domain::AppError error)
{
    if (error.category() == domain::ErrorCategory::Conflict) {
        if (mutation == QStringLiteral("collection-created")
            || mutation == QStringLiteral("collection-updated")) {
            return QStringLiteral("A collection with that name already exists.");
        }
        if (mutation == QStringLiteral("game-added"))
            return QStringLiteral("That game is already in this collection.");
        if (mutation == QStringLiteral("game-moved")
            || mutation == QStringLiteral("game-copied"))
            return QStringLiteral(
                "The game is already in the destination collection; its existing membership was left unchanged.");
    }
    if (error.category() == domain::ErrorCategory::Validation)
        return QStringLiteral("The collection details are not valid.");
    return mutation == QStringLiteral("collection-deleted")
        ? QStringLiteral("The collection could not be deleted.")
        : QStringLiteral("The collection change could not be saved.");
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

SteamCollectionsController::SteamCollectionsController(
    application::ApplicationServiceGraph *graph, QObject *parent)
    : QObject(parent), m_graph(graph)
{
    Q_ASSERT(m_graph);
    Q_ASSERT(QThread::currentThread() == thread());
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &SteamCollectionsController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::httpsFinished,
            this, &SteamCollectionsController::onHttpsFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &SteamCollectionsController::onValueJobFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &SteamCollectionsController::onLifecycleChanged);
    onLifecycleChanged(m_graph->lifecycle().state());
}

QString SteamCollectionsController::selectedCollectionName() const
{
    if (m_selectedCollectionId == 0) return QStringLiteral("All games");
    for (const QVariant &value : m_collections) {
        const QVariantMap collection = value.toMap();
        if (collection.value(QStringLiteral("id")).toLongLong() == m_selectedCollectionId)
            return collection.value(QStringLiteral("name")).toString();
    }
    return QStringLiteral("All games");
}

int SteamCollectionsController::allGamesCount() const
{
    QSet<quint32> appIds;
    for (const QVariant &value : m_memberships)
        appIds.insert(value.toMap().value(QStringLiteral("appId")).toUInt());
    return appIds.size();
}

void SteamCollectionsController::onLifecycleChanged(
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

quint64 SteamCollectionsController::submitStorage(
    Kind kind, storage::StorageCommand command, Pending pending)
{
    const auto operation = m_graph->beginOperation(
        QStringLiteral("Steam collections storage operation"));
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

quint64 SteamCollectionsController::submitNetwork(
    Kind kind, providers::ProviderRequest request, Pending pending)
{
    const auto operation = m_graph->beginOperation(
        QStringLiteral("Steam Store game catalog request"));
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

quint64 SteamCollectionsController::submitArtworkStage(QByteArray body,
                                                       Pending pending)
{
    const auto operation = m_graph->beginOperation(
        QStringLiteral("Validate Steam collection artwork"));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    pending.kind = Kind::ArtworkStage;
    const QString profileDirectory = m_graph->profileDirectory();
    const quint32 appId = pending.appId;
    m_pending.insert(id, pending);
    const bool submitted = m_graph->submitExpensiveValue(operation,
        [body = std::move(body), profileDirectory, appId](
            const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            services::StagedSteamArtwork artwork;
            result.error = services::SteamArtworkStore::stageResponse(
                profileDirectory, appId, body, token, &artwork);
            if (result.error.isError()) return result;
            result.values = {
                {QStringLiteral("filename"), artwork.asset.filename},
                {QStringLiteral("mimeType"), artwork.asset.mimeType},
                {QStringLiteral("byteSize"), artwork.asset.byteSize},
                {QStringLiteral("stagingFilename"), artwork.stagingFilename},
                {QStringLiteral("stagedNewFile"), artwork.stagedNewFile},
            };
            return result;
        });
    if (!submitted) {
        m_pending.remove(id);
        return 0;
    }
    return id;
}

quint64 SteamCollectionsController::submitParse(QByteArray body, Pending pending)
{
    const auto operation = m_graph->beginOperation(
        QStringLiteral("Parse Steam game catalog details"));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    pending.kind = Kind::AddParse;
    const quint32 expectedAppId = pending.appId;
    m_pending.insert(id, pending);
    const bool submitted = m_graph->submitExpensiveValue(operation,
        [body = std::move(body), expectedAppId](
            const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            if (token.isCancellationRequested()) {
                result.error = domain::AppError::cancelled();
                return result;
            }
            const auto parsed = providers::parseSteamGameCatalogDetails(body, expectedAppId);
            if (!parsed.succeeded()) {
                result.error = parsed.error;
                return result;
            }
            if (parsed.value.appId != expectedAppId
                || !domain::isValidSteamGameCatalogDetails(parsed.value)) {
                result.error = domain::AppError(domain::ErrorCategory::InvalidResponse);
                return result;
            }
            result.values = {
                {QStringLiteral("appId"), parsed.value.appId},
                {QStringLiteral("storeUrl"), parsed.value.canonicalStoreUrl},
                {QStringLiteral("name"), parsed.value.name},
                {QStringLiteral("artworkUrl"), parsed.value.artworkUrl},
            };
            return result;
        });
    if (!submitted) {
        m_pending.remove(id);
        return 0;
    }
    return id;
}

bool SteamCollectionsController::submitMutation(
    storage::StorageCommand command, QString mutationType, qint64 collectionId)
{
    if (!m_ready || !m_snapshotLoaded || m_busy || m_addBusy) return false;
    Pending pending;
    pending.kind = Kind::Mutation;
    pending.mutationType = std::move(mutationType);
    pending.collectionId = collectionId;
    m_busy = true;
    m_errorMessage.clear();
    m_notification.clear();
    if (!submitStorage(Kind::Mutation, std::move(command), pending)) {
        m_busy = false;
        return false;
    }
    emit stateChanged();
    return true;
}

void SteamCollectionsController::requestSnapshot()
{
    if (!m_ready || m_loading) return;
    m_snapshotStarted = true;
    m_loading = true;
    m_errorMessage.clear();
    Pending pending;
    if (!submitStorage(Kind::Snapshot,
            services::SteamCollectionsService::loadSnapshot(
                m_graph->profileDirectory()), pending)) {
        m_loading = false;
        m_errorMessage = QStringLiteral("Saved collections could not be loaded.");
    }
    emit stateChanged();
}

void SteamCollectionsController::refreshSnapshot()
{
    if (!m_ready || !m_snapshotLoaded || m_loading || m_busy || m_addBusy) return;
    requestSnapshot();
}

void SteamCollectionsController::applySnapshot(const QVariantMap &values)
{
    m_collections = values.value(QStringLiteral("collections")).toList();
    m_memberships = values.value(QStringLiteral("memberships")).toList();
    m_catalog = values.value(QStringLiteral("catalog")).toList();
    QHash<qint64, QSet<quint32>> counts;
    for (const QVariant &value : m_memberships) {
        const QVariantMap membership = value.toMap();
        counts[membership.value(QStringLiteral("collectionId")).toLongLong()].insert(
            membership.value(QStringLiteral("appId")).toUInt());
    }
    for (QVariant &value : m_collections) {
        QVariantMap collection = value.toMap();
        collection.insert(QStringLiteral("gameCount"),
            counts.value(collection.value(QStringLiteral("id")).toLongLong()).size());
        value = collection;
    }
    bool selectedExists = m_selectedCollectionId == 0;
    for (const QVariant &value : m_collections) {
        if (value.toMap().value(QStringLiteral("id")).toLongLong()
            == m_selectedCollectionId) {
            selectedExists = true;
            break;
        }
    }
    if (!selectedExists) m_selectedCollectionId = 0;
    m_snapshotLoaded = true;
    m_errorMessage.clear();
    rebuildGames();
}

void SteamCollectionsController::rebuildGames()
{
    QVector<QVariantMap> filtered;
    QHash<quint32, int> groupedRows;
    const QString needle = m_searchText.trimmed().toCaseFolded();
    for (const QVariant &value : m_memberships) {
        const QVariantMap membership = value.toMap();
        const qint64 collectionId =
            membership.value(QStringLiteral("collectionId")).toLongLong();
        if (m_selectedCollectionId != 0 && collectionId != m_selectedCollectionId)
            continue;
        const quint32 appId = membership.value(QStringLiteral("appId")).toUInt();
        QVariantMap game;
        int row = -1;
        if (m_selectedCollectionId == 0) {
            const auto found = groupedRows.constFind(appId);
            if (found != groupedRows.cend()) {
                row = found.value();
                game = filtered[row];
                if (membership.value(QStringLiteral("createdAt")).toString()
                    > game.value(QStringLiteral("createdAt")).toString()) {
                    game.insert(QStringLiteral("createdAt"),
                                membership.value(QStringLiteral("createdAt")));
                }
            }
        }
        if (row < 0) {
            game = membership;
            game.insert(QStringLiteral("memberships"), QVariantList{});
            if (m_selectedCollectionId == 0) {
                game.insert(QStringLiteral("status"), QString());
                game.insert(QStringLiteral("rating"), QVariant());
                game.insert(QStringLiteral("comment"), QString());
            }
            row = filtered.size();
            filtered.push_back(game);
            if (m_selectedCollectionId == 0) groupedRows.insert(appId, row);
        }
        QVariantList gameMemberships = game.value(QStringLiteral("memberships")).toList();
        gameMemberships.push_back(membership);
        game.insert(QStringLiteral("memberships"), gameMemberships);
        filtered[row] = game;
    }

    QVector<QVariantMap> visible;
    visible.reserve(filtered.size());
    for (QVariantMap game : filtered) {
        const QVariantList memberships = game.value(QStringLiteral("memberships")).toList();
        bool statusMatches = m_statusFilter == QStringLiteral("all");
        bool textMatches = needle.isEmpty()
            || game.value(QStringLiteral("name")).toString().toCaseFolded().contains(needle);
        if (!textMatches) {
            for (const QVariant &membershipValue : memberships) {
                const QVariantMap membership = membershipValue.toMap();
                if (membership.value(QStringLiteral("collectionName"))
                        .toString().toCaseFolded().contains(needle)
                    || membership.value(QStringLiteral("comment"))
                        .toString().toCaseFolded().contains(needle)) {
                    textMatches = true;
                    break;
                }
            }
        }
        for (const QVariant &membershipValue : memberships) {
            const QVariantMap membership = membershipValue.toMap();
            if (membership.value(QStringLiteral("status")).toString() == m_statusFilter)
                statusMatches = true;
        }
        if (textMatches && statusMatches) visible.push_back(std::move(game));
    }

    std::sort(visible.begin(), visible.end(), [this](const QVariantMap &left,
                                                      const QVariantMap &right) {
        if (m_sortOrder == QStringLiteral("recently_added")) {
            const int recency = QString::compare(
                left.value(QStringLiteral("createdAt")).toString(),
                right.value(QStringLiteral("createdAt")).toString(), Qt::CaseSensitive);
            if (recency != 0) return recency > 0;
        }
        const int comparison = QString::compare(
            left.value(QStringLiteral("name")).toString(),
            right.value(QStringLiteral("name")).toString(), Qt::CaseInsensitive);
        if (comparison != 0)
            return m_sortOrder == QStringLiteral("name_desc")
                ? comparison > 0 : comparison < 0;
        return left.value(QStringLiteral("appId")).toUInt()
            < right.value(QStringLiteral("appId")).toUInt();
    });
    m_games.clear();
    m_games.reserve(visible.size());
    for (QVariantMap &game : visible) m_games.push_back(std::move(game));
}

void SteamCollectionsController::queueArtworkScan()
{
    if (!m_ready || !m_snapshotLoaded) return;
    m_nextArtworkIndex = 0;
    m_artworkScanActive = true;
    dispatchArtworkScan();
}

void SteamCollectionsController::dispatchArtworkScan()
{
    if (!m_ready || !m_snapshotLoaded || !m_artworkScanActive) return;
    int scanned = 0;
    while (m_activeArtworkApps.size() < kMaximumArtworkRequests
           && m_nextArtworkIndex < m_catalog.size()
           && scanned < kMaximumArtworkScansPerTurn) {
        const QVariantMap game = m_catalog.at(m_nextArtworkIndex++).toMap();
        ++scanned;
        const quint32 appId = game.value(QStringLiteral("appId")).toUInt();
        if (appId == 0 || !game.value(QStringLiteral("artworkFilename")).toString().isEmpty()
            || m_activeArtworkApps.contains(appId)) {
            continue;
        }
        const QString url = domain::normalizeSteamArtworkUrl(
            game.value(QStringLiteral("artworkUrl")).toString());
        if (url.isEmpty()) continue;
        Pending pending;
        pending.appId = appId;
        pending.artworkUrl = url;
        m_activeArtworkApps.insert(appId);
        providers::ProviderRequest request{
            providers::ProviderId::SteamArtwork, QUrl(url)};
        if (!submitNetwork(Kind::ArtworkNetwork, std::move(request), pending))
            m_activeArtworkApps.remove(appId);
    }
    if (m_nextArtworkIndex < m_catalog.size()
        && m_activeArtworkApps.size() < kMaximumArtworkRequests
        && scanned >= kMaximumArtworkScansPerTurn) {
        scheduleArtworkScan();
        return;
    }
    if (m_nextArtworkIndex >= m_catalog.size() && m_activeArtworkApps.isEmpty())
        m_artworkScanActive = false;
}

void SteamCollectionsController::scheduleArtworkScan()
{
    if (m_artworkDispatchScheduled || !m_ready || !m_artworkScanActive) return;
    m_artworkDispatchScheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_artworkDispatchScheduled = false;
        dispatchArtworkScan();
    });
}

void SteamCollectionsController::finishArtwork(quint32 appId)
{
    m_activeArtworkApps.remove(appId);
    dispatchArtworkScan();
}

void SteamCollectionsController::selectCollection(qint64 collectionId)
{
    if (collectionId < 0) return;
    if (collectionId != 0) {
        bool exists = false;
        for (const QVariant &value : m_collections) {
            if (value.toMap().value(QStringLiteral("id")).toLongLong() == collectionId) {
                exists = true;
                break;
            }
        }
        if (!exists) return;
    }
    if (m_selectedCollectionId == collectionId) return;
    m_selectedCollectionId = collectionId;
    rebuildGames();
    emit stateChanged();
}

void SteamCollectionsController::setSearchText(QString text)
{
    text = text.left(120);
    if (m_searchText == text) return;
    m_searchText = std::move(text);
    rebuildGames();
    emit stateChanged();
}

void SteamCollectionsController::setSortOrder(QString order)
{
    if (order != QStringLiteral("name_asc")
        && order != QStringLiteral("name_desc")
        && order != QStringLiteral("recently_added")) return;
    if (m_sortOrder == order) return;
    m_sortOrder = std::move(order);
    rebuildGames();
    emit stateChanged();
}

void SteamCollectionsController::setStatusFilter(QString filter)
{
    if (filter != QStringLiteral("all") && !collectionStatus(filter)) return;
    if (m_statusFilter == filter) return;
    m_statusFilter = std::move(filter);
    rebuildGames();
    emit stateChanged();
}

void SteamCollectionsController::createCollection(
    QString name, QString description, QString color, QString icon)
{
    if (!m_ready || m_busy || m_addBusy) return;
    domain::SteamCollectionInput input{std::move(name), std::move(description),
                                      std::move(color), std::move(icon)};
    Pending pending;
    pending.mutationType = QStringLiteral("collection-created");
    if (!submitMutation(services::SteamCollectionsService::createCollection(
            m_graph->profileDirectory(), std::move(input)),
            pending.mutationType)) {
        m_errorMessage = QStringLiteral("Collections are unavailable right now.");
        emit stateChanged();
    }
}

void SteamCollectionsController::updateCollection(
    qint64 collectionId, QString name, QString description,
    QString color, QString icon)
{
    if (collectionId <= 0 || m_busy || m_addBusy) return;
    domain::SteamCollectionInput input{std::move(name), std::move(description),
                                      std::move(color), std::move(icon)};
    if (!submitMutation(services::SteamCollectionsService::updateCollection(
            m_graph->profileDirectory(), collectionId, std::move(input)),
            QStringLiteral("collection-updated"), collectionId)) {
        m_errorMessage = QStringLiteral("Collections are unavailable right now.");
        emit stateChanged();
    }
}

void SteamCollectionsController::moveCollection(qint64 collectionId, int direction)
{
    if (m_busy || m_addBusy || (direction != -1 && direction != 1)) return;
    QVector<qint64> orderedIds;
    orderedIds.reserve(m_collections.size());
    int index = -1;
    for (const QVariant &value : m_collections) {
        const qint64 id = value.toMap().value(QStringLiteral("id")).toLongLong();
        if (id == collectionId) index = orderedIds.size();
        orderedIds.push_back(id);
    }
    const int target = index + direction;
    if (index < 0 || target < 0 || target >= orderedIds.size()) return;
    orderedIds.swapItemsAt(index, target);
    if (!submitMutation(services::SteamCollectionsService::reorderCollections(
            m_graph->profileDirectory(), std::move(orderedIds)),
            QStringLiteral("collections-reordered"))) {
        m_errorMessage = QStringLiteral("Collection order could not be saved.");
        emit stateChanged();
    }
}

void SteamCollectionsController::deleteCollection(qint64 collectionId)
{
    if (collectionId <= 0 || m_busy || m_addBusy) return;
    if (!submitMutation(services::SteamCollectionsService::deleteCollection(
            m_graph->profileDirectory(), collectionId),
            QStringLiteral("collection-deleted"), collectionId)) {
        m_errorMessage = QStringLiteral("Collections are unavailable right now.");
        emit stateChanged();
    }
}

bool SteamCollectionsController::hasMembership(qint64 collectionId,
                                                quint32 appId) const
{
    for (const QVariant &value : m_memberships) {
        const QVariantMap membership = value.toMap();
        if (membership.value(QStringLiteral("collectionId")).toLongLong() == collectionId
            && membership.value(QStringLiteral("appId")).toUInt() == appId) return true;
    }
    return false;
}

QString SteamCollectionsController::reservationKey(qint64 collectionId,
                                                    quint32 appId) const
{
    return QString::number(collectionId) + QLatin1Char(':') + QString::number(appId);
}

void SteamCollectionsController::addGame(QString storeUrl)
{
    if (!m_ready || !m_snapshotLoaded || m_busy || m_addBusy) return;
    if (m_selectedCollectionId <= 0) {
        m_addError = QStringLiteral("Select a collection before adding a game.");
        emit stateChanged();
        return;
    }
    const auto request = providers::makeSteamGameCatalogProviderRequest(std::move(storeUrl));
    if (!request.succeeded()) {
        m_addError = QStringLiteral("Enter a valid Steam Store /app/<id> URL.");
        emit stateChanged();
        return;
    }
    const QString reservation = reservationKey(m_selectedCollectionId, request.appId);
    if (hasMembership(m_selectedCollectionId, request.appId)
        || m_reservedMemberships.contains(reservation)) {
        m_addError = QStringLiteral("This game is already in the selected collection.");
        emit stateChanged();
        return;
    }
    Pending pending;
    pending.collectionId = m_selectedCollectionId;
    pending.appId = request.appId;
    pending.canonicalUrl = request.canonicalStoreUrl;
    pending.addReservation = reservation;
    pending.mutationType = QStringLiteral("game-added");
    m_reservedMemberships.insert(reservation);
    m_addBusy = true;
    m_addError.clear();
    m_errorMessage.clear();
    m_notification.clear();
    if (!submitNetwork(Kind::AddNetwork, request.providerRequest, pending)) {
        m_reservedMemberships.remove(reservation);
        m_addBusy = false;
        m_addError = QStringLiteral("Steam networking is unavailable right now.");
    }
    emit stateChanged();
}

void SteamCollectionsController::addExistingGame(quint32 appId,
                                                  qint64 collectionId)
{
    if (!m_ready || !m_snapshotLoaded || appId == 0 || collectionId <= 0
        || m_busy || m_addBusy) return;
    const QString reservation = reservationKey(collectionId, appId);
    if (hasMembership(collectionId, appId) || m_reservedMemberships.contains(reservation)) {
        m_errorMessage = QStringLiteral("That game is already in the selected collection.");
        emit stateChanged();
        return;
    }
    Pending pending;
    pending.collectionId = collectionId;
    pending.appId = appId;
    pending.addReservation = reservation;
    pending.mutationType = QStringLiteral("game-added");
    m_reservedMemberships.insert(reservation);
    m_addBusy = true;
    m_addError.clear();
    if (!submitStorage(Kind::AddInsert,
            services::SteamCollectionsService::addExistingGame(
                m_graph->profileDirectory(), collectionId, appId), pending)) {
        m_reservedMemberships.remove(reservation);
        m_addBusy = false;
        m_addError = QStringLiteral("The saved Steam game could not be added.");
    }
    emit stateChanged();
}

void SteamCollectionsController::updateGame(
    qint64 collectionId, quint32 appId, QString status, int rating,
    QString comment)
{
    if (collectionId <= 0 || appId == 0 || rating < 0 || rating > 5
        || m_busy || m_addBusy) return;
    domain::SteamCollectionGameInput input;
    input.steamAppId = appId;
    input.status = collectionStatus(status);
    if (!status.isEmpty() && !input.status) {
        m_errorMessage = QStringLiteral("Choose a valid personal status.");
        emit stateChanged();
        return;
    }
    if (rating > 0) input.personalRating = rating;
    input.personalComment = std::move(comment);
    if (!submitMutation(services::SteamCollectionsService::updateGame(
            m_graph->profileDirectory(), collectionId, std::move(input)),
            QStringLiteral("game-updated"), collectionId)) {
        m_errorMessage = QStringLiteral("Game details could not be saved.");
        emit stateChanged();
    }
}

void SteamCollectionsController::removeGame(qint64 collectionId, quint32 appId)
{
    if (collectionId <= 0 || appId == 0 || m_busy || m_addBusy) return;
    if (!submitMutation(services::SteamCollectionsService::removeGame(
            m_graph->profileDirectory(), collectionId, appId),
            QStringLiteral("game-removed"), collectionId)) {
        m_errorMessage = QStringLiteral("The game could not be removed from that collection.");
        emit stateChanged();
    }
}

void SteamCollectionsController::moveGame(qint64 sourceCollectionId,
                                           qint64 targetCollectionId,
                                           quint32 appId)
{
    if (!m_ready || !m_snapshotLoaded || sourceCollectionId <= 0
        || targetCollectionId <= 0 || sourceCollectionId == targetCollectionId
        || appId == 0 || m_busy || m_addBusy) {
        return;
    }
    if (!hasMembership(sourceCollectionId, appId)) {
        m_errorMessage = QStringLiteral("That game is no longer in the source collection.");
        emit stateChanged();
        return;
    }
    if (hasMembership(targetCollectionId, appId)) {
        m_errorMessage = QStringLiteral(
            "The game is already in the destination collection; its existing membership was left unchanged.");
        emit stateChanged();
        return;
    }
    if (!submitMutation(services::SteamCollectionsService::moveGame(
            m_graph->profileDirectory(), sourceCollectionId,
            targetCollectionId, appId), QStringLiteral("game-moved"),
            sourceCollectionId)) {
        m_errorMessage = QStringLiteral("The game could not be moved between collections.");
        emit stateChanged();
    }
}

void SteamCollectionsController::copyGame(qint64 sourceCollectionId,
                                           qint64 targetCollectionId,
                                           quint32 appId)
{
    if (!m_ready || !m_snapshotLoaded || sourceCollectionId <= 0
        || targetCollectionId <= 0 || sourceCollectionId == targetCollectionId
        || appId == 0 || m_busy || m_addBusy) {
        return;
    }
    if (!hasMembership(sourceCollectionId, appId)) {
        m_errorMessage = QStringLiteral("That game is no longer in the source collection.");
        emit stateChanged();
        return;
    }
    if (hasMembership(targetCollectionId, appId)) {
        m_errorMessage = QStringLiteral(
            "The game is already in the destination collection; its existing membership was left unchanged.");
        emit stateChanged();
        return;
    }
    if (!submitMutation(services::SteamCollectionsService::copyGame(
            m_graph->profileDirectory(), sourceCollectionId,
            targetCollectionId, appId), QStringLiteral("game-copied"),
            sourceCollectionId)) {
        m_errorMessage = QStringLiteral("The game could not be copied to that collection.");
        emit stateChanged();
    }
}

QVariantList SteamCollectionsController::availableCatalogGames(
    qint64 collectionId) const
{
    QVariantList available;
    if (collectionId <= 0) return available;
    for (const QVariant &value : m_catalog) {
        const QVariantMap game = value.toMap();
        if (!hasMembership(collectionId, game.value(QStringLiteral("appId")).toUInt()))
            available.push_back(game);
    }
    return available;
}

void SteamCollectionsController::openStore(quint32 appId)
{
    for (const QVariant &value : m_catalog) {
        const QVariantMap game = value.toMap();
        if (game.value(QStringLiteral("appId")).toUInt() != appId) continue;
        const domain::SteamStoreUrl normalized = domain::normalizeSteamStoreUrl(
            game.value(QStringLiteral("storeUrl")).toString());
        if (!normalized.isValid() || normalized.appId != appId
            || !QDesktopServices::openUrl(QUrl(normalized.canonicalUrl))) {
            m_errorMessage = QStringLiteral("The Steam Store page could not be opened.");
            emit stateChanged();
        }
        return;
    }
}

void SteamCollectionsController::retryLoad()
{
    if (!m_ready || m_loading) return;
    m_snapshotLoaded = false;
    m_snapshotStarted = false;
    requestSnapshot();
}

void SteamCollectionsController::clearMessages()
{
    if (m_errorMessage.isEmpty() && m_addError.isEmpty() && m_notification.isEmpty()) return;
    m_errorMessage.clear();
    m_addError.clear();
    m_notification.clear();
    emit stateChanged();
}

void SteamCollectionsController::clearNotification()
{
    if (m_notification.isEmpty()) return;
    m_notification.clear();
    emit stateChanged();
}

void SteamCollectionsController::onHttpsFinished(
    quint64 id, const QByteArray &body, domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Pending pending = it.value();
    m_pending.erase(it);
    if (pending.kind == Kind::ArtworkNetwork) {
        if (error.isError() || error.isCancelled()
            || !submitArtworkStage(body, pending)) {
            finishArtwork(pending.appId);
        }
        return;
    }
    if (pending.kind != Kind::AddNetwork) return;
    if (error.isError() || error.isCancelled()) {
        m_reservedMemberships.remove(pending.addReservation);
        m_addBusy = false;
        m_addError = error.category() == domain::ErrorCategory::NotFound
            ? QStringLiteral("Steam could not find that game.")
            : QStringLiteral("Steam game details could not be loaded.");
    } else if (!submitParse(body, pending)) {
        m_reservedMemberships.remove(pending.addReservation);
        m_addBusy = false;
        m_addError = QStringLiteral("Steam game details could not be parsed safely.");
    }
    emit stateChanged();
}

void SteamCollectionsController::onValueJobFinished(
    quint64 id, const QVariantMap &values, domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    Pending pending = it.value();
    m_pending.erase(it);
    if (pending.kind == Kind::ArtworkStage) {
        if (error.isError() || error.isCancelled()) {
            finishArtwork(pending.appId);
            return;
        }
        const services::StagedSteamArtwork artwork = stagedArtworkFromMap(values);
        if (!submitStorage(Kind::ArtworkSave,
                services::SteamCollectionsService::saveArtwork(
                    m_graph->profileDirectory(), pending.appId, pending.artworkUrl,
                    artwork), pending)) {
            services::SteamArtworkStore::discard(m_graph->profileDirectory(), artwork);
            finishArtwork(pending.appId);
        }
        return;
    }
    if (pending.kind != Kind::AddParse) return;
    if (error.isError() || error.isCancelled()) {
        m_reservedMemberships.remove(pending.addReservation);
        m_addBusy = false;
        m_addError = QStringLiteral("Steam returned game details that could not be read safely.");
        emit stateChanged();
        return;
    }
    domain::SteamGameCatalogDetails details;
    details.appId = pending.appId;
    details.canonicalStoreUrl = pending.canonicalUrl;
    details.name = values.value(QStringLiteral("name")).toString();
    details.artworkUrl = values.value(QStringLiteral("artworkUrl")).toString();
    if (!submitStorage(Kind::AddInsert,
            services::SteamCollectionsService::addGame(
                m_graph->profileDirectory(), pending.collectionId, std::move(details)),
            pending)) {
        m_reservedMemberships.remove(pending.addReservation);
        m_addBusy = false;
        m_addError = QStringLiteral("The game could not be saved to this collection.");
    }
    emit stateChanged();
}

void SteamCollectionsController::finishMutation(
    const Pending &pending, const QVariantMap &values, domain::AppError error)
{
    if (pending.kind == Kind::Mutation) m_busy = false;
    if (pending.kind == Kind::AddInsert) {
        m_addBusy = false;
        m_reservedMemberships.remove(pending.addReservation);
    }
    if (error.isError() || error.isCancelled()) {
        const QString message = safeMutationError(pending.mutationType, error);
        if (pending.kind == Kind::AddInsert) m_addError = message;
        else m_errorMessage = message;
        emit stateChanged();
        return;
    }

    applySnapshot(values);
    const QVariantMap mutation = values.value(QStringLiteral("mutation")).toMap();
    const QString type = mutation.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("collection-created")) {
        m_selectedCollectionId = mutation.value(QStringLiteral("collectionId")).toLongLong();
        m_notification = QStringLiteral("Created “%1”.")
            .arg(mutation.value(QStringLiteral("name")).toString());
    } else if (type == QStringLiteral("collection-updated")) {
        m_notification = QStringLiteral("Collection updated.");
    } else if (type == QStringLiteral("collection-deleted")) {
        if (m_selectedCollectionId == pending.collectionId) m_selectedCollectionId = 0;
        m_notification = QStringLiteral("Collection deleted. Its games and other tracking were kept.");
    } else if (type == QStringLiteral("collections-reordered")) {
        m_notification = QStringLiteral("Collection order saved.");
    } else if (type == QStringLiteral("game-added")) {
        m_notification = QStringLiteral("Added %1 to the collection.")
            .arg(mutation.value(QStringLiteral("name")).toString());
        m_addError.clear();
        queueArtworkScan();
    } else if (type == QStringLiteral("game-updated")) {
        m_notification = QStringLiteral("Game details saved.");
    } else if (type == QStringLiteral("game-removed")) {
        m_notification = QStringLiteral("Removed the game from this collection.");
    } else if (type == QStringLiteral("game-moved")) {
        m_notification = QStringLiteral("Moved %1 to another collection.")
            .arg(mutation.value(QStringLiteral("name")).toString());
    } else if (type == QStringLiteral("game-copied")) {
        m_notification = QStringLiteral("Copied %1 to another collection.")
            .arg(mutation.value(QStringLiteral("name")).toString());
    }
    rebuildGames();
    emit stateChanged();
}

void SteamCollectionsController::onStorageFinished(
    quint64 id, const QVariantMap &values, domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Pending pending = it.value();
    m_pending.erase(it);
    if (pending.kind == Kind::ArtworkSave) {
        if (!error.isError() && !error.isCancelled()) applySnapshot(values);
        finishArtwork(pending.appId);
        emit stateChanged();
        return;
    }
    if (pending.kind == Kind::Snapshot) {
        m_loading = false;
        if (error.isError() || error.isCancelled()) {
            m_errorMessage = QStringLiteral("Saved collections could not be loaded.");
        } else {
            applySnapshot(values);
            m_snapshotLoaded = true;
        }
        emit stateChanged();
        return;
    }
    finishMutation(pending, values, error);
}

void SteamCollectionsController::cancelPending(quint64 id)
{
    m_pending.remove(id);
    m_graph->cancelOperation(application::OperationId{id});
}

void SteamCollectionsController::cancelAll()
{
    const auto ids = m_pending.keys();
    for (quint64 id : ids) cancelPending(id);
    m_reservedMemberships.clear();
    m_loading = false;
    m_busy = false;
    m_addBusy = false;
    m_activeArtworkApps.clear();
    m_artworkScanActive = false;
    m_artworkDispatchScheduled = false;
}

} // namespace something::controllers

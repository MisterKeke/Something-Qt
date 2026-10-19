#include "favorites_controller.h"

#include "../domain/favorite_rules.h"
#include "../services/favorites_service.h"

#include <QBuffer>
#include <QDesktopServices>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QThread>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace something::controllers {
namespace {

using namespace something;
constexpr int kMaximumQueuedFavoriteMediaRequests = 256;
constexpr int kMaximumActiveFavoriteMediaRequests = 2;

QString itemCacheKey(const QVariantMap &item)
{
    const QString itemId = item.value(QStringLiteral("itemId")).toString();
    return itemId;
}

QVariantList imageSources(qsizetype count)
{
    QVariantList result;
    for (qsizetype i = 0; i < count; ++i) result.push_back(QString());
    return result;
}

QString errorText(domain::AppError error, const QString &fallback)
{
    const QString message = error.userMessage();
    return message.isEmpty() ? fallback : message;
}

QString isoUtc(const QDateTime &time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}

bool validTimestamp(const QString &value)
{
    return QDateTime::fromString(value, Qt::ISODateWithMs).isValid() ||
        QDateTime::fromString(value, Qt::ISODate).isValid();
}

QVariantMap normalizeImageItem(const QString &source, QVariantMap item)
{
    QVariantList images = item.value(QStringLiteral("images")).toList();
    item.insert(QStringLiteral("imageSources"), imageSources(images.size()));
    if (source == QStringLiteral("telegram")) {
        const QString channel = item.value(QStringLiteral("sourceId")).toString();
        const QString postId = item.value(QStringLiteral("itemId")).toString();
        item.insert(QStringLiteral("externalUrl"),
                    domain::telegramPostUrl(channel, postId).toString());
    } else {
        item.insert(QStringLiteral("externalUrl"),
                    domain::youTubeVideoUrl(item.value(QStringLiteral("itemId")).toString()).toString());
    }
    return item;
}

} // namespace

FavoritesController::FavoritesController(application::ApplicationServiceGraph *graph,
                                         const domain::Clock *clock,
                                         providers::ProviderImageProvider *images,
                                         QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_clock(clock)
    , m_images(images)
    , m_telegramPosts(this)
    , m_youTubeVideos(this)
    , m_telegramFavorites(this)
    , m_youTubeFavorites(this)
    , m_telegramCategories(this)
    , m_youTubeCategories(this)
{
    Q_ASSERT(m_graph && m_clock && m_images);
    Q_ASSERT(QThread::currentThread() == thread());
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &FavoritesController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::httpsFinished,
            this, &FavoritesController::onHttpsFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &FavoritesController::onValueJobFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, [this] { onLifecycleChanged(m_graph->lifecycle().state()); });
    onLifecycleChanged(m_graph->lifecycle().state());
}

void FavoritesController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    if (state == application::ApplicationLifecycle::State::Running) {
        loadSnapshot(QStringLiteral("telegram"));
        loadSnapshot(QStringLiteral("youtube"));
        if (!m_openScanStarted) {
            m_openScanStarted = true;
            startNewsScan(true);
        }
    } else if (state == application::ApplicationLifecycle::State::ShuttingDown ||
               state == application::ApplicationLifecycle::State::Failed ||
               state == application::ApplicationLifecycle::State::Stopped) {
        cancelNewsScan();
        const auto ids = m_pending.keys();
        for (quint64 id : ids) cancelPending(id);
        m_telegramLoading = false;
        m_youTubeLoading = false;
        m_favoritesLoading = false;
        m_mutationBusy = false;
        m_waitingMediaRequests.clear();
        m_activeMediaRequests = 0;
        emit telegramStateChanged();
        emit youTubeStateChanged();
        emit stateChanged();
    }
}

models::ProviderItemsModel *FavoritesController::contentModel(const QString &source)
{
    return source == QStringLiteral("telegram") ? &m_telegramPosts : &m_youTubeVideos;
}

models::FavoriteChannelsModel *FavoritesController::favoriteModel(const QString &source)
{
    return source == QStringLiteral("telegram") ? &m_telegramFavorites : &m_youTubeFavorites;
}

models::FavoriteCategoriesModel *FavoritesController::categoryModel(const QString &source)
{
    return source == QStringLiteral("telegram") ? &m_telegramCategories : &m_youTubeCategories;
}

void FavoritesController::setSource(QString source)
{
    source = domain::normalizeFavoriteSource(std::move(source));
    if (source.isEmpty() || source == m_activeSource) return;
    m_activeSource = source;
    emit activeSourceChanged();
    loadSnapshot(source);
}

quint64 FavoritesController::submitStorage(Pending pending, storage::StorageCommand command,
                                           QString freshnessKey)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("favorite feature storage"),
                                                    std::move(freshnessKey));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    m_pending.insert(id, std::move(pending));
    if (!m_graph->submitStorage(operation, std::move(command)) && m_pending.contains(id))
        m_pending.remove(id);
    return id;
}

quint64 FavoritesController::submitNetwork(Pending pending,
                                           providers::ProviderRequest request,
                                           QString freshnessKey)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("favorite provider request"),
                                                    std::move(freshnessKey));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    m_pending.insert(id, std::move(pending));
    if (!m_graph->requestHttps(operation, std::move(request)) && m_pending.contains(id))
        m_pending.remove(id);
    return id;
}

quint64 FavoritesController::submitParse(Pending pending, QByteArray body,
                                         QString freshnessKey)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("parse favorite provider data"),
                                                    std::move(freshnessKey));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    m_pending.insert(id, pending);
    const bool submitted = m_graph->submitExpensiveValue(operation,
        [pending = std::move(pending), body = std::move(body)]
        (const application::CancellationToken &cancellation) {
            services::BoundedWorkerPool::ValueResult result;
            if (cancellation.isCancellationRequested()) {
                result.error = domain::AppError::cancelled();
                return result;
            }

            if (pending.kind == PendingKind::MediaDecode) {
                QBuffer buffer;
                buffer.setData(body);
                if (!buffer.open(QIODevice::ReadOnly)) {
                    result.error = domain::AppError(domain::ErrorCategory::InvalidResponse);
                    return result;
                }
                QImageReader reader(&buffer);
                reader.setAutoTransform(true);
                const QSize sourceSize = reader.size();
                if (!sourceSize.isValid() || sourceSize.width() > 12000 || sourceSize.height() > 12000 ||
                    qint64(sourceSize.width()) * sourceSize.height() > 48'000'000) {
                    result.error = domain::AppError(domain::ErrorCategory::InvalidResponse);
                    return result;
                }
                reader.setScaledSize(sourceSize.scaled(QSize(1280, 900), Qt::KeepAspectRatio));
                QImage image = reader.read();
                if (image.isNull()) {
                    result.error = domain::AppError(domain::ErrorCategory::InvalidResponse);
                    return result;
                }
                result.values.insert(QStringLiteral("image"), QVariant::fromValue(std::move(image)));
                return result;
            }

            QVariantList items;
            if (pending.source == QStringLiteral("telegram")) {
                const auto parsed = providers::parseTelegramPosts(body);
                if (!parsed.succeeded()) {
                    result.error = parsed.error;
                    return result;
                }
                items.reserve(parsed.items.size());
                for (const auto &post : parsed.items) {
                    if (cancellation.isCancellationRequested()) {
                        result.error = domain::AppError::cancelled();
                        return result;
                    }
                    QVariantMap row = services::FavoritesService::telegramPostToMap(pending.reference, post);
                    if (row.value(QStringLiteral("itemId")).toString().isEmpty()) continue;
                    items.push_back(std::move(row));
                }
            } else if (pending.resolveHandle) {
                domain::AppError parseError;
                const QString id = providers::parseYouTubeChannelIdPage(body, &parseError);
                if (id.isEmpty()) {
                    result.error = parseError.isError()
                        ? parseError : domain::AppError(domain::ErrorCategory::InvalidResponse);
                    return result;
                }
                result.values.insert(QStringLiteral("channelId"), id);
                return result;
            } else {
                const auto parsed = providers::parseYouTubeFeed(body, pending.channelId);
                if (!parsed.succeeded()) {
                    result.error = parsed.error;
                    return result;
                }
                items.reserve(parsed.items.size());
                for (const auto &video : parsed.items) {
                    if (cancellation.isCancellationRequested()) {
                        result.error = domain::AppError::cancelled();
                        return result;
                    }
                    items.push_back(services::FavoritesService::youTubeVideoToMap(video));
                }
            }
            result.values.insert(QStringLiteral("items"), items);
            return result;
        });
    if (!submitted && m_pending.contains(id)) m_pending.remove(id);
    return id;
}

void FavoritesController::loadSnapshot(const QString &source, bool preserveMutationError)
{
    if (m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running) return;
    if (m_snapshotOperations.value(source) != 0) {
        cancelPending(m_snapshotOperations.value(source));
        m_snapshotOperations.remove(source);
    }
    m_favoritesLoading = true;
    if (!preserveMutationError) m_mutationError.clear();
    emit stateChanged();
    Pending pending;
    pending.kind = PendingKind::Snapshot;
    pending.source = source;
    pending.preserveMutationError = preserveMutationError;
    const quint64 id = submitStorage(pending, services::FavoritesService::loadSourceSnapshot(source),
                                    QStringLiteral("favorite-snapshot:") + source);
    if (id && m_pending.contains(id)) m_snapshotOperations.insert(source, id);
}

void FavoritesController::cancelPending(quint64 id)
{
    if (id == 0) return;
    m_pending.remove(id);
    m_graph->cancelOperation({id});
}

void FavoritesController::cancelChannelOperations(const QString &source)
{
    const auto ids = m_pending.keys();
    for (quint64 id : ids) {
        const auto it = m_pending.constFind(id);
        if (it == m_pending.cend() || it->source != source) continue;
        if (it->kind == PendingKind::ChannelRequest || it->kind == PendingKind::ChannelParse)
            cancelPending(id);
    }
    m_channelOperations.remove(source);
}

void FavoritesController::loadChannel(QString source, QString reference, bool refresh)
{
    source = domain::normalizeFavoriteSource(std::move(source));
    if (source.isEmpty() || m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running)
        return;
    if (source == QStringLiteral("telegram")) {
        const QString normalized = domain::normalizeTelegramChannel(std::move(reference));
        if (normalized.isEmpty()) {
            m_telegramError = QStringLiteral("Enter a valid Telegram channel username.");
            m_telegramLoading = false;
            emit telegramStateChanged();
            return;
        }
        const bool changed = normalized != m_telegramChannel;
        m_telegramChannel = normalized;
        emit channelReferenceChanged(source, normalized);
        if (changed) {
            discardPendingPageMedia(source);
            m_telegramPosts.replace({});
            m_telegramHasOlder = false;
        }
        m_telegramError.clear();
        emit telegramStateChanged();
        startTelegramRequest(normalized, 0, refresh, false);
    } else {
        bool isId = false;
        const QString normalized = domain::normalizeYouTubeReference(std::move(reference), &isId);
        if (normalized.isEmpty()) {
            m_youTubeError = QStringLiteral("Enter a valid YouTube channel ID or handle.");
            m_youTubeLoading = false;
            emit youTubeStateChanged();
            return;
        }
        const bool changed = normalized != m_youTubeChannel;
        m_youTubeChannel = normalized;
        emit channelReferenceChanged(source, normalized);
        if (changed) {
            discardPendingPageMedia(source);
            m_youTubeChannelId.clear();
            m_youTubeVideos.replace({});
        }
        m_youTubeError.clear();
        emit youTubeStateChanged();
        startYouTubeRequest(normalized, refresh);
    }
}

void FavoritesController::refreshChannel()
{
    if (m_activeSource == QStringLiteral("telegram"))
        loadChannel(m_activeSource, m_telegramChannel, true);
    else
        loadChannel(m_activeSource, m_youTubeChannel, true);
}

void FavoritesController::loadOlderTelegramPosts()
{
    if (m_telegramLoading || !m_telegramHasOlder || m_telegramPosts.count() == 0) return;
    // Pages and the model are newest first. Use the oldest valid ID as the
    // exclusive cursor, including when pages overlap or contain invalid rows.
    QStringList postIds;
    postIds.reserve(m_telegramPosts.count());
    for (int row = 0; row < m_telegramPosts.count(); ++row) {
        postIds.push_back(m_telegramPosts.itemAt(row).value(QStringLiteral("itemId")).toString());
    }
    const quint64 cursor = domain::oldestTelegramPostId(postIds);
    if (cursor == 0) {
        m_telegramHasOlder = false;
        emit telegramStateChanged();
        return;
    }
    startTelegramRequest(m_telegramChannel, cursor, false, true);
}

void FavoritesController::startTelegramRequest(QString channel, quint64 before,
                                               bool refresh, bool append)
{
    cancelChannelOperations(QStringLiteral("telegram"));
    if (refresh && before == 0)
        m_graph->providerResponseCache().invalidate(
            providers::ProviderCachePartition::TelegramPosts, channel);
    QByteArray cached;
    if (before == 0 && !refresh && m_graph->providerResponseCache().get(
            providers::ProviderCachePartition::TelegramPosts, channel, &cached)) {
        const QVariantList rows = services::FavoritesService::decodeCachedItems(cached);
        if (!rows.isEmpty()) {
            m_telegramPosts.replace(rows);
            m_telegramHasOlder = true;
            m_telegramLoading = false;
            m_telegramError.clear();
            startMediaLoads(QStringLiteral("telegram"), rows, &m_telegramPosts);
            emit telegramStateChanged();
            return;
        }
    }
    m_telegramLoading = true;
    m_telegramError.clear();
    emit telegramStateChanged();
    Pending pending;
    pending.kind = PendingKind::ChannelRequest;
    pending.source = QStringLiteral("telegram");
    pending.reference = channel;
    pending.cacheKey = channel;
    pending.before = before;
    pending.refresh = refresh;
    pending.append = append;
    const quint64 id = submitNetwork(pending,
        {providers::ProviderId::Telegram, domain::telegramPostsUrl(channel, before), refresh, 0},
        QStringLiteral("favorite-channel:telegram"));
    if (id && m_pending.contains(id)) m_channelOperations.insert(QStringLiteral("telegram"), id);
    else {
        m_telegramLoading = false;
        m_telegramError = QStringLiteral("Telegram data could not be loaded.");
        emit telegramStateChanged();
    }
}

void FavoritesController::startYouTubeRequest(QString reference, bool refresh)
{
    cancelChannelOperations(QStringLiteral("youtube"));
    bool isId = false;
    const QString normalized = domain::normalizeYouTubeReference(reference, &isId);
    if (normalized.isEmpty()) return;
    m_youTubeLoading = true;
    m_youTubeError.clear();
    emit youTubeStateChanged();
    if (isId) {
        m_youTubeChannelId = normalized;
        startYouTubeFeed(normalized, normalized, refresh);
        return;
    }

    const QString handleKey = normalized.toLower();
    QByteArray cachedId;
    if (m_graph->providerResponseCache().get(
            providers::ProviderCachePartition::YouTubeHandleResolution, handleKey, &cachedId)) {
        m_youTubeChannelId = QString::fromUtf8(cachedId);
        startYouTubeFeed(m_youTubeChannelId, normalized, refresh);
        return;
    }

    Pending pending;
    pending.kind = PendingKind::ChannelRequest;
    pending.source = QStringLiteral("youtube");
    pending.reference = normalized;
    pending.cacheKey = handleKey;
    pending.refresh = refresh;
    pending.resolveHandle = true;
    const quint64 id = submitNetwork(pending,
        {providers::ProviderId::YouTubePage,
         domain::youTubeChannelUrl(normalized, false), refresh, 0},
        QStringLiteral("favorite-channel:youtube"));
    if (id && m_pending.contains(id)) m_channelOperations.insert(QStringLiteral("youtube"), id);
    else {
        m_youTubeLoading = false;
        m_youTubeError = QStringLiteral("YouTube channel data could not be loaded.");
        emit youTubeStateChanged();
    }
}

void FavoritesController::startYouTubeFeed(QString channelId, QString originalReference,
                                           bool refresh)
{
    m_youTubeChannelId = channelId;
    if (refresh) m_graph->providerResponseCache().invalidate(
        providers::ProviderCachePartition::YouTubeVideos, channelId);
    QByteArray cached;
    if (!refresh && m_graph->providerResponseCache().get(
            providers::ProviderCachePartition::YouTubeVideos, channelId, &cached)) {
        const QVariantList rows = services::FavoritesService::decodeCachedItems(cached);
        if (!rows.isEmpty()) {
            m_youTubeVideos.replace(rows);
            m_youTubeLoading = false;
            m_youTubeError.clear();
            startMediaLoads(QStringLiteral("youtube"), rows, &m_youTubeVideos);
            emit youTubeStateChanged();
            return;
        }
    }
    Pending pending;
    pending.kind = PendingKind::ChannelRequest;
    pending.source = QStringLiteral("youtube");
    pending.reference = originalReference;
    pending.channelId = channelId;
    pending.cacheKey = channelId;
    pending.refresh = refresh;
    const quint64 id = submitNetwork(pending,
        {providers::ProviderId::YouTubeFeed, domain::youTubeFeedUrl(channelId), refresh, 0},
        QStringLiteral("favorite-channel:youtube"));
    if (id && m_pending.contains(id)) m_channelOperations.insert(QStringLiteral("youtube"), id);
    else {
        m_youTubeLoading = false;
        m_youTubeError = QStringLiteral("YouTube videos could not be loaded.");
        emit youTubeStateChanged();
    }
}

void FavoritesController::setChannelFailure(const QString &source, const QString &message)
{
    if (source == QStringLiteral("telegram")) {
        m_telegramLoading = false;
        m_telegramError = message;
        emit telegramStateChanged();
    } else {
        m_youTubeLoading = false;
        m_youTubeError = message;
        emit youTubeStateChanged();
    }
}

void FavoritesController::setMutationFailure(domain::AppError error)
{
    m_mutationError = error.userMessage();
    if (m_mutationError.isEmpty() && error.category() != domain::ErrorCategory::Cancelled)
        m_mutationError = QStringLiteral("The favorite change could not be saved.");
    m_mutationBusy = false;
    emit stateChanged();
}

void FavoritesController::addFavorite(QString source, QString categoryId,
                                      QString newCategoryName, QString newCategoryColor)
{
    if (m_mutationBusy) return;
    source = domain::normalizeFavoriteSource(std::move(source));
    if (source.isEmpty()) return;
    QString sourceId;
    QString label;
    if (source == QStringLiteral("telegram")) {
        sourceId = domain::normalizeTelegramChannel(m_telegramChannel);
        label = sourceId;
    } else {
        sourceId = domain::normalizeYouTubeReference(m_youTubeChannelId);
        if (sourceId.isEmpty()) {
            bool isId = false;
            sourceId = domain::normalizeYouTubeReference(m_youTubeChannel, &isId);
            if (!isId) sourceId.clear();
        }
        bool isId = false;
        label = domain::normalizeYouTubeReference(m_youTubeChannel, &isId);
        if (isId) label.clear();
    }
    if (sourceId.isEmpty()) {
        setMutationFailure(domain::AppError(domain::ErrorCategory::Validation));
        return;
    }
    bool categoryOk = false;
    const qint64 selectedCategory = categoryId.toLongLong(&categoryOk);
    const QString normalizedName = newCategoryName.trimmed();
    if (!normalizedName.isEmpty()) {
        if (domain::normalizeFavoriteCategoryColor(newCategoryColor).isEmpty() &&
            !newCategoryColor.trimmed().isEmpty()) {
            setMutationFailure(domain::AppError(domain::ErrorCategory::Validation));
            return;
        }
        m_mutationBusy = true;
        m_mutationError.clear();
        emit stateChanged();
        Pending pending;
        pending.kind = PendingKind::AddFavoriteCategory;
        pending.source = source;
        pending.channelId = sourceId;
        pending.reference = label;
        const quint64 id = submitStorage(pending,
            services::FavoritesService::createCategory(source, normalizedName, newCategoryColor));
        if (!id || !m_pending.contains(id))
            setMutationFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
        return;
    }
    if (categoryId != QStringLiteral("uncategorized") && (!categoryOk || selectedCategory <= 0)) {
        setMutationFailure(domain::AppError(domain::ErrorCategory::Validation));
        return;
    }
    m_mutationBusy = true;
    m_mutationError.clear();
    emit stateChanged();
    Pending pending;
    pending.kind = PendingKind::FavoriteAdd;
    pending.source = source;
    pending.channelId = sourceId;
    pending.reference = label;
    pending.categoryId = categoryId == QStringLiteral("uncategorized") ? 0 : selectedCategory;
    const quint64 id = submitStorage(pending,
        services::FavoritesService::addFavorite(source, sourceId, label));
    if (!id || !m_pending.contains(id))
        setMutationFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
}

void FavoritesController::removeFavorite(QString source, QString sourceId)
{
    if (m_mutationBusy) return;
    source = domain::normalizeFavoriteSource(std::move(source));
    if (source.isEmpty()) return;
    m_mutationBusy = true;
    m_mutationError.clear();
    emit stateChanged();
    Pending pending;
    pending.kind = PendingKind::Mutation;
    pending.source = source;
    const quint64 id = submitStorage(pending,
        services::FavoritesService::removeFavorite(source, std::move(sourceId)));
    if (!id) setMutationFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
}

void FavoritesController::assignFavoriteCategory(QString source, QString sourceId,
                                                 QString categoryId)
{
    if (m_mutationBusy) return;
    source = domain::normalizeFavoriteSource(std::move(source));
    bool ok = false;
    const qint64 category = categoryId.toLongLong(&ok);
    const bool uncategorized = categoryId == QStringLiteral("uncategorized");
    if (source.isEmpty() || (!uncategorized && (!ok || category <= 0))) return;
    m_mutationBusy = true;
    m_mutationError.clear();
    emit stateChanged();
    Pending pending;
    pending.kind = PendingKind::Mutation;
    pending.source = source;
    const quint64 id = submitStorage(pending,
        services::FavoritesService::assignFavoriteCategory(source, std::move(sourceId),
                                                           uncategorized ? 0 : category));
    if (!id) setMutationFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
}

void FavoritesController::createCategory(QString source, QString name, QString color)
{
    if (m_mutationBusy) return;
    source = domain::normalizeFavoriteSource(std::move(source));
    if (source.isEmpty()) return;
    m_mutationBusy = true;
    m_mutationError.clear();
    emit stateChanged();
    Pending pending;
    pending.kind = PendingKind::Mutation;
    pending.source = source;
    const quint64 id = submitStorage(pending,
        services::FavoritesService::createCategory(source, std::move(name), std::move(color)));
    if (!id) setMutationFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
}

void FavoritesController::updateCategory(QString source, qint64 id,
                                         QString name, QString color)
{
    if (m_mutationBusy) return;
    source = domain::normalizeFavoriteSource(std::move(source));
    if (source.isEmpty()) return;
    m_mutationBusy = true;
    m_mutationError.clear();
    emit stateChanged();
    Pending pending;
    pending.kind = PendingKind::Mutation;
    pending.source = source;
    const quint64 operationId = submitStorage(pending,
        services::FavoritesService::updateCategory(id, source, std::move(name), std::move(color)));
    if (!operationId) setMutationFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
}

void FavoritesController::moveCategory(QString source, qint64 id, int direction)
{
    if (m_mutationBusy) return;
    source = domain::normalizeFavoriteSource(std::move(source));
    if (source.isEmpty() || (direction != -1 && direction != 1)) return;
    const auto categories = categoryModel(source);
    QVector<qint64> ids;
    ids.reserve(categories->count());
    int index = -1;
    for (int row = 0; row < categories->count(); ++row) {
        const qint64 value = categories->rowAt(row).value(QStringLiteral("id")).toLongLong();
        if (value == id) index = row;
        ids.push_back(value);
    }
    const int nextIndex = index + direction;
    if (index < 0 || nextIndex < 0 || nextIndex >= ids.size()) return;
    std::swap(ids[index], ids[nextIndex]);
    m_mutationBusy = true;
    m_mutationError.clear();
    emit stateChanged();
    Pending pending;
    pending.kind = PendingKind::Mutation;
    pending.source = source;
    const quint64 operationId = submitStorage(pending,
        services::FavoritesService::reorderCategories(source, std::move(ids)));
    if (!operationId) setMutationFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
}

void FavoritesController::deleteCategory(QString source, qint64 id, QString mode,
                                         QString targetCategoryId)
{
    if (m_mutationBusy) return;
    source = domain::normalizeFavoriteSource(std::move(source));
    bool targetOk = false;
    const qint64 target = targetCategoryId.toLongLong(&targetOk);
    if (source.isEmpty() || (mode == QStringLiteral("move") && (!targetOk || target <= 0))) return;
    m_mutationBusy = true;
    m_mutationError.clear();
    emit stateChanged();
    Pending pending;
    pending.kind = PendingKind::Mutation;
    pending.source = source;
    const quint64 operationId = submitStorage(pending,
        services::FavoritesService::deleteCategory(id, std::move(mode), targetOk ? target : 0));
    if (!operationId) setMutationFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
}

void FavoritesController::refreshNews()
{
    if (m_newsScanning) return;
    startNewsScan(false);
}

void FavoritesController::openExternal(QString source, QString rawUrl)
{
    const QUrl url(rawUrl, QUrl::StrictMode);
    m_externalOpenError.clear();
    if (!domain::isAllowedExternalFavoriteUrl(source, url) || !QDesktopServices::openUrl(url)) {
        m_externalOpenError = QStringLiteral("This provider link could not be opened.");
        emit externalOpenFailed(QStringLiteral("This provider link could not be opened."));
    }
    emit stateChanged();
}

void FavoritesController::onStorageFinished(quint64 operationId,
                                            const QVariantMap &values,
                                            domain::AppError error)
{
    auto it = m_pending.find(operationId);
    if (it == m_pending.end()) return;
    const Pending pending = it.value();
    m_pending.erase(it);

    if (pending.kind == PendingKind::Snapshot) {
        if (m_snapshotOperations.value(pending.source) == operationId)
            m_snapshotOperations.remove(pending.source);
        m_favoritesLoading = !m_snapshotOperations.isEmpty();
        if (error.isError()) {
            m_mutationError = error.userMessage();
        } else if (!error.isCancelled()) {
            QVariantList categories = values.value(QStringLiteral("categories")).toList();
            QVariantList favorites = values.value(QStringLiteral("favorites")).toList();
            categoryModel(pending.source)->replace(std::move(categories));
            favoriteModel(pending.source)->replace(std::move(favorites));
            if (!pending.preserveMutationError) m_mutationError.clear();
        }
        emit stateChanged();
        return;
    }

    if (pending.kind == PendingKind::NewsPrepare) {
        m_newsPrepareOperation = 0;
        if (!m_newsScan || pending.scanGeneration != m_newsScan->generation) return;
        if (error.isError()) {
            m_newsScan.reset();
            m_newsScanning = false;
            m_newsScanPartial = false;
            m_newsScanError = error.userMessage();
            emit stateChanged();
            emit newsScanFinished(m_newsScanError, false);
            return;
        }
        m_newsScan->scanStartedAt = values.value(QStringLiteral("scanStartedAt")).toString();
        for (const QVariant &value : values.value(QStringLiteral("sources")).toList())
            m_newsScan->waiting.push_back(value.toMap());
        dispatchNewsFetches();
        return;
    }

    if (pending.kind == PendingKind::NewsCommit) {
        m_newsCommitOperation = 0;
        if (!m_newsScan || pending.scanGeneration != m_newsScan->generation) return;
        const bool partial = !m_newsScan->errors.isEmpty();
        QString partialMessage;
        if (partial) {
            QStringList details;
            for (const QVariantMap &row : std::as_const(m_newsScan->errors)) {
                if (details.size() == 2) break;
                details.push_back(QStringLiteral("%1: %2").arg(
                    row.value(QStringLiteral("sourceId")).toString(),
                    row.value(QStringLiteral("error")).toString()));
            }
            const int extra = m_newsScan->errors.size() - details.size();
            partialMessage = QStringLiteral("Some favorite channels could not be refreshed (%1%2).")
                .arg(details.join(QStringLiteral("; ")),
                     extra > 0 ? QStringLiteral(" and %1 more").arg(extra) : QString());
        }
        const QString scanError = error.isError()
            ? error.userMessage()
            : partialMessage;
        m_newsScan.reset();
        m_newsScanning = false;
        m_newsScanPartial = partial;
        m_newsScanError = scanError;
        emit stateChanged();
        if (!error.isError()) emit newsChanged();
        emit newsScanFinished(scanError, partial);
        return;
    }

    if (pending.kind == PendingKind::AddFavoriteCategory) {
        if (error.isError()) {
            setMutationFailure(error);
            return;
        }
        const qint64 categoryId = values.value(QStringLiteral("category")).toMap()
                                      .value(QStringLiteral("id")).toLongLong();
        if (categoryId <= 0) {
            setMutationFailure(domain::AppError(domain::ErrorCategory::Internal));
            return;
        }
        Pending add;
        add.kind = PendingKind::FavoriteAdd;
        add.source = pending.source;
        add.channelId = pending.channelId;
        add.reference = pending.reference;
        add.categoryId = categoryId;
        const quint64 next = submitStorage(add,
            services::FavoritesService::addFavorite(pending.source, pending.channelId,
                                                      pending.reference));
        if (!next || !m_pending.contains(next)) {
            setMutationFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
            loadSnapshot(pending.source, true);
        }
        return;
    }

    if (pending.kind == PendingKind::FavoriteAdd) {
        if (error.isError()) {
            setMutationFailure(error);
            if (pending.categoryId > 0) loadSnapshot(pending.source, true);
            return;
        }
        if (pending.categoryId > 0) {
            Pending assignment;
            assignment.kind = PendingKind::FavoriteAssignment;
            assignment.source = pending.source;
            assignment.channelId = pending.channelId;
            assignment.reference = pending.reference;
            const quint64 next = submitStorage(assignment,
                services::FavoritesService::assignFavoriteCategory(pending.source, pending.channelId,
                                                                   pending.categoryId));
            if (!next || !m_pending.contains(next)) {
                setMutationFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
                loadSnapshot(pending.source, true);
            }
            return;
        }
        m_mutationBusy = false;
        m_mutationError.clear();
        emit stateChanged();
        loadSnapshot(pending.source);
        return;
    }

    if (pending.kind == PendingKind::FavoriteAssignment) {
        if (error.isError()) {
            setMutationFailure(error);
            loadSnapshot(pending.source, true);
            return;
        }
        m_mutationBusy = false;
        m_mutationError.clear();
        emit stateChanged();
        loadSnapshot(pending.source);
        return;
    }

    if (pending.kind == PendingKind::Mutation) {
        if (error.isError()) {
            setMutationFailure(error);
            return;
        }
        m_mutationBusy = false;
        m_mutationError.clear();
        emit stateChanged();
        loadSnapshot(pending.source);
    }
}

void FavoritesController::onHttpsFinished(quint64 operationId,
                                          const QByteArray &body,
                                          domain::AppError error)
{
    auto it = m_pending.find(operationId);
    if (it == m_pending.end()) return;
    Pending pending = it.value();
    m_pending.erase(it);
    if (pending.kind == PendingKind::MediaRequest && m_activeMediaRequests > 0)
        --m_activeMediaRequests;
    QMetaObject::invokeMethod(this, [this] { dispatchMediaRequests(); }, Qt::QueuedConnection);
    if (m_channelOperations.value(pending.source) == operationId)
        m_channelOperations.remove(pending.source);

    if (pending.kind == PendingKind::ChannelRequest) {
        if (error.isError()) {
            setChannelFailure(pending.source, error.userMessage());
            return;
        }
        pending.kind = PendingKind::ChannelParse;
        const QString freshness = QStringLiteral("favorite-channel:") + pending.source;
        const quint64 parseId = submitParse(pending, body, freshness);
        if (parseId && m_pending.contains(parseId))
            m_channelOperations.insert(pending.source, parseId);
        else if (!parseId)
            setChannelFailure(pending.source, QStringLiteral("Provider data could not be parsed."));
        return;
    }

    if (pending.kind == PendingKind::NewsRequest) {
        if (error.isError()) {
            storage::FavoriteNewsSourceScan batch;
            batch.source = pending.sourcePlan.value(QStringLiteral("source")).toString();
            batch.sourceId = pending.sourcePlan.value(QStringLiteral("sourceId")).toString();
            batch.checkedThrough = pending.sourcePlan.value(QStringLiteral("checkedThrough")).toString();
            batch.updateWindowStart = pending.sourcePlan.value(QStringLiteral("updateWindowStart")).toString();
            batch.sourceHasSeenItems = pending.sourcePlan.value(QStringLiteral("sourceHasSeenItems")).toBool();
            batch.error = error.userMessage();
            completeNewsFetch(std::move(batch), pending.sourcePlan, pending.scanGeneration);
            return;
        }
        pending.kind = PendingKind::NewsParse;
        if (!submitParse(pending, body)) {
            storage::FavoriteNewsSourceScan batch;
            batch.source = pending.sourcePlan.value(QStringLiteral("source")).toString();
            batch.sourceId = pending.sourcePlan.value(QStringLiteral("sourceId")).toString();
            batch.checkedThrough = pending.sourcePlan.value(QStringLiteral("checkedThrough")).toString();
            batch.updateWindowStart = pending.sourcePlan.value(QStringLiteral("updateWindowStart")).toString();
            batch.sourceHasSeenItems = pending.sourcePlan.value(QStringLiteral("sourceHasSeenItems")).toBool();
            batch.error = QStringLiteral("Provider data could not be parsed.");
            completeNewsFetch(std::move(batch), pending.sourcePlan, pending.scanGeneration);
        }
        return;
    }

    if (pending.kind == PendingKind::MediaRequest) {
        if (!error.isError()) {
            pending.kind = PendingKind::MediaDecode;
            submitParse(std::move(pending), body);
        }
        return;
    }
}

void FavoritesController::onValueJobFinished(quint64 operationId,
                                              const QVariantMap &values,
                                              domain::AppError error)
{
    auto it = m_pending.find(operationId);
    if (it == m_pending.end()) return;
    Pending pending = it.value();
    m_pending.erase(it);
    if (m_channelOperations.value(pending.source) == operationId)
        m_channelOperations.remove(pending.source);

    if (pending.kind == PendingKind::ChannelParse) {
        if (error.isError()) {
            setChannelFailure(pending.source, error.userMessage());
            return;
        }
        if (pending.resolveHandle) {
            const QString channelId = values.value(QStringLiteral("channelId")).toString();
            if (channelId.isEmpty()) {
                setChannelFailure(pending.source, QStringLiteral("The YouTube channel could not be resolved."));
                return;
            }
            m_graph->providerResponseCache().put(
                providers::ProviderCachePartition::YouTubeHandleResolution,
                pending.cacheKey, channelId.toUtf8());
            m_youTubeChannelId = channelId;
            emit youTubeStateChanged();
            startYouTubeFeed(channelId, pending.reference, pending.refresh);
            return;
        }

        QVariantList rows = values.value(QStringLiteral("items")).toList();
        for (QVariant &value : rows) value = normalizeImageItem(pending.source, value.toMap());
        if (pending.source == QStringLiteral("telegram")) {
            const int previousCount = m_telegramPosts.count();
            if (pending.append) m_telegramPosts.appendUnique(rows);
            else m_telegramPosts.replace(rows);
            if (rows.isEmpty() || (pending.append && m_telegramPosts.count() == previousCount))
                m_telegramHasOlder = false;
            else
                m_telegramHasOlder = true;
            if (!pending.append && pending.before == 0 && !rows.isEmpty())
                m_graph->providerResponseCache().put(
                    providers::ProviderCachePartition::TelegramPosts, pending.cacheKey,
                    services::FavoritesService::encodeCachedItems(rows), rows.size());
            startMediaLoads(pending.source, rows, &m_telegramPosts);
            m_telegramLoading = false;
            m_telegramError.clear();
            emit telegramStateChanged();
        } else {
            m_youTubeVideos.replace(rows);
            if (!rows.isEmpty())
                m_graph->providerResponseCache().put(
                    providers::ProviderCachePartition::YouTubeVideos, pending.channelId,
                    services::FavoritesService::encodeCachedItems(rows), rows.size());
            startMediaLoads(pending.source, rows, &m_youTubeVideos);
            m_youTubeLoading = false;
            m_youTubeError.clear();
            emit youTubeStateChanged();
        }
        return;
    }

    if (pending.kind == PendingKind::NewsParse) {
        storage::FavoriteNewsSourceScan batch;
        batch.source = pending.sourcePlan.value(QStringLiteral("source")).toString();
        batch.sourceId = pending.sourcePlan.value(QStringLiteral("sourceId")).toString();
        batch.checkedThrough = pending.sourcePlan.value(QStringLiteral("checkedThrough")).toString();
        batch.updateWindowStart = pending.sourcePlan.value(QStringLiteral("updateWindowStart")).toString();
        batch.sourceHasSeenItems = pending.sourcePlan.value(QStringLiteral("sourceHasSeenItems")).toBool();
        if (error.isError()) {
            batch.error = error.userMessage();
        } else {
            batch.succeeded = true;
            const QString discoveredAt = m_newsScan ? m_newsScan->scanStartedAt : QString();
            for (const QVariant &value : values.value(QStringLiteral("items")).toList()) {
                const QVariantMap item = value.toMap();
                const QString timestamp = item.value(QStringLiteral("publishedAt")).toString();
                if (!validTimestamp(timestamp) || item.value(QStringLiteral("itemId")).toString().isEmpty()) continue;
                if (batch.source == QStringLiteral("telegram") &&
                    item.value(QStringLiteral("externalUrl")).toString().isEmpty()) continue;
                batch.items.push_back(services::FavoritesService::newsItemFromMap(
                    batch.source, batch.sourceId, item, discoveredAt));
            }
        }
        completeNewsFetch(std::move(batch), pending.sourcePlan, pending.scanGeneration);
        return;
    }

    if (pending.kind == PendingKind::MediaDecode && !error.isError()) {
        const QImage image = values.value(QStringLiteral("image")).value<QImage>();
        const QString localSource = m_images->cacheImage(pending.imageUrl, image);
        if (localSource.isEmpty()) return;
        applyMediaSource(pending, localSource);
    }
}

void FavoritesController::startNewsScan(bool appOpen)
{
    if (m_newsScanning || m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running)
        return;
    const QDateTime now = m_clock->nowUtc().toUTC();
    auto scan = std::make_unique<NewsScan>();
    scan->generation = m_nextScanGeneration++;
    scan->appOpen = appOpen;
    scan->scanStartedAt = isoUtc(now);
    const quint64 generation = scan->generation;
    m_newsScan = std::move(scan);
    m_newsScanning = true;
    m_newsScanPartial = false;
    m_newsScanError.clear();
    emit stateChanged();

    Pending pending;
    pending.kind = PendingKind::NewsPrepare;
    pending.scanGeneration = generation;
    const quint64 id = submitStorage(pending,
        services::FavoritesService::prepareNewsScan(
            appOpen, isoUtc(now), isoUtc(now.addDays(-30))),
        QStringLiteral("favorite-news-prepare"));
    m_newsPrepareOperation = id && m_pending.contains(id) ? id : 0;
    if (!id && m_newsScan) {
        m_newsScan.reset();
        m_newsScanning = false;
        m_newsScanError = QStringLiteral("Favorite updates could not be prepared.");
        emit stateChanged();
        emit newsScanFinished(m_newsScanError, false);
    }
}

void FavoritesController::dispatchNewsFetches()
{
    if (!m_newsScan) return;
    while (m_newsScan->activeFetches < 4 && m_newsScan->nextIndex < m_newsScan->waiting.size()) {
        const QVariantMap plan = m_newsScan->waiting.at(m_newsScan->nextIndex++);
        const QString source = plan.value(QStringLiteral("source")).toString();
        const QString sourceId = plan.value(QStringLiteral("sourceId")).toString();
        const quint64 generation = m_newsScan->generation;
        Pending pending;
        pending.kind = PendingKind::NewsRequest;
        pending.source = source;
        pending.sourcePlan = plan;
        pending.scanGeneration = generation;
        providers::ProviderRequest request;
        if (source == QStringLiteral("telegram")) {
            request = {providers::ProviderId::Telegram,
                       domain::telegramPostsUrl(sourceId), true, 0};
        } else {
            request = {providers::ProviderId::YouTubeFeed,
                       domain::youTubeFeedUrl(sourceId), true, 0};
            pending.channelId = sourceId;
        }
        ++m_newsScan->activeFetches;
        if (!submitNetwork(std::move(pending), std::move(request))) {
            storage::FavoriteNewsSourceScan failed;
            failed.source = source;
            failed.sourceId = sourceId;
            failed.checkedThrough = plan.value(QStringLiteral("checkedThrough")).toString();
            failed.updateWindowStart = plan.value(QStringLiteral("updateWindowStart")).toString();
            failed.sourceHasSeenItems = plan.value(QStringLiteral("sourceHasSeenItems")).toBool();
            failed.error = QStringLiteral("The provider capability is unavailable.");
            completeNewsFetch(std::move(failed), plan, generation);
        }
    }
    finishNewsScanIfDrained();
}

void FavoritesController::completeNewsFetch(storage::FavoriteNewsSourceScan batch,
                                            const QVariantMap &plan,
                                            quint64 generation)
{
    Q_UNUSED(plan)
    if (!m_newsScan || m_newsScan->generation != generation) return;
    if (m_newsScan->activeFetches > 0) --m_newsScan->activeFetches;
    if (!batch.succeeded) {
        if (batch.error.isEmpty()) batch.error = QStringLiteral("Provider data could not be loaded.");
        m_newsScan->errors.push_back({{QStringLiteral("source"), batch.source},
                                      {QStringLiteral("sourceId"), batch.sourceId},
                                      {QStringLiteral("error"), batch.error}});
    }
    m_newsScan->completed.push_back(std::move(batch));
    QMetaObject::invokeMethod(this, [this, generation] {
        if (m_newsScan && m_newsScan->generation == generation) dispatchNewsFetches();
    }, Qt::QueuedConnection);
}

void FavoritesController::finishNewsScanIfDrained()
{
    if (!m_newsScan || m_newsScan->nextIndex < m_newsScan->waiting.size() ||
        m_newsScan->activeFetches != 0 || m_newsCommitOperation != 0) return;

    QJsonArray errors;
    for (const QVariantMap &row : std::as_const(m_newsScan->errors)) {
        errors.push_back(QJsonObject{{QStringLiteral("source"), row.value(QStringLiteral("source")).toString()},
                                    {QStringLiteral("source_id"), row.value(QStringLiteral("sourceId")).toString()},
                                    {QStringLiteral("error"), row.value(QStringLiteral("error")).toString()}});
    }
    storage::FavoriteNewsScanCommit commit;
    commit.scanStartedAt = m_newsScan->scanStartedAt;
    commit.errorsJson = QString::fromUtf8(QJsonDocument(errors).toJson(QJsonDocument::Compact));
    commit.sources = std::move(m_newsScan->completed);
    Pending pending;
    pending.kind = PendingKind::NewsCommit;
    pending.scanGeneration = m_newsScan->generation;
    const quint64 generation = pending.scanGeneration;
    const quint64 commitOperation = submitStorage(pending,
        services::FavoritesService::commitNewsScan(std::move(commit)),
        QStringLiteral("favorite-news-commit"));
    m_newsCommitOperation = commitOperation && m_pending.contains(commitOperation)
        ? commitOperation : 0;
    if (!m_newsCommitOperation && m_newsScan && m_newsScan->generation == generation) {
        m_newsScan.reset();
        m_newsScanning = false;
        m_newsScanError = QStringLiteral("Favorite updates could not be saved.");
        emit stateChanged();
        emit newsScanFinished(m_newsScanError, false);
    }
}

void FavoritesController::cancelNewsScan()
{
    if (!m_newsScan) return;
    const quint64 generation = m_newsScan->generation;
    const auto ids = m_pending.keys();
    for (quint64 id : ids) {
        const auto it = m_pending.constFind(id);
        if (it == m_pending.cend() || it->scanGeneration != generation) continue;
        cancelPending(id);
    }
    m_newsPrepareOperation = 0;
    m_newsCommitOperation = 0;
    m_newsScan.reset();
    m_newsScanning = false;
    emit stateChanged();
}

void FavoritesController::startMediaLoads(const QString &source,
                                          const QVariantList &items,
                                          models::ProviderItemsModel *model)
{
    if (!model) return;
    for (const QVariant &value : items) {
        const QVariantMap item = value.toMap();
        const QString itemId = itemCacheKey(item);
        const QVariantList images = item.value(QStringLiteral("images")).toList();
        for (qsizetype index = 0; index < images.size(); ++index) {
            const QString imageUrl = images.at(index).toString();
            if (imageUrl.isEmpty()) continue;
            const QString cachedSource = m_images->cachedSource(imageUrl);
            if (!cachedSource.isEmpty()) {
                model->updateImageSource(itemId, int(index), cachedSource);
                continue;
            }
            if (m_waitingMediaRequests.size() >= kMaximumQueuedFavoriteMediaRequests) break;
            const providers::ProviderId provider = source == QStringLiteral("telegram")
                ? providers::ProviderId::TelegramMedia : providers::ProviderId::YouTubeMedia;
            if (!providers::isAllowedProviderUrl(provider, QUrl(imageUrl, QUrl::StrictMode))) continue;
            Pending pending;
            pending.kind = PendingKind::MediaRequest;
            pending.source = source;
            pending.itemId = itemId;
            pending.imageIndex = int(index);
            pending.imageUrl = imageUrl;
            m_waitingMediaRequests.enqueue(std::move(pending));
        }
        if (m_waitingMediaRequests.size() >= kMaximumQueuedFavoriteMediaRequests) break;
    }
    dispatchMediaRequests();
}

void FavoritesController::loadNewsImages(QVariantList items)
{
    startDashboardMediaLoads(items);
}

void FavoritesController::startDashboardMediaLoads(const QVariantList &items)
{
    if (!m_dashboardNews) return;
    for (const QVariant &value : items) {
        const QVariantMap item = value.toMap();
        const QString source = domain::normalizeFavoriteSource(
            item.value(QStringLiteral("source")).toString());
        const providers::ProviderId provider = source == QStringLiteral("telegram")
            ? providers::ProviderId::TelegramMedia : providers::ProviderId::YouTubeMedia;
        const QString itemId = item.value(QStringLiteral("itemId")).toString();
        const QVariantList imageUrls = item.value(QStringLiteral("images")).toList();
        for (qsizetype index = 0; index < imageUrls.size(); ++index) {
            const QString imageUrl = imageUrls.at(index).toString();
            if (imageUrl.isEmpty()) continue;
            const QString cachedSource = m_images->cachedSource(imageUrl);
            if (!cachedSource.isEmpty()) {
                m_dashboardNews->updateImageSource(itemId, int(index), cachedSource);
                continue;
            }
            if (m_waitingMediaRequests.size() >= kMaximumQueuedFavoriteMediaRequests) break;
            if (!providers::isAllowedProviderUrl(provider, QUrl(imageUrl, QUrl::StrictMode))) continue;
            Pending pending;
            pending.kind = PendingKind::MediaRequest;
            pending.source = source;
            pending.itemId = itemId;
            pending.imageIndex = int(index);
            pending.imageUrl = imageUrl;
            pending.dashboardImage = true;
            m_waitingMediaRequests.enqueue(std::move(pending));
        }
        if (m_waitingMediaRequests.size() >= kMaximumQueuedFavoriteMediaRequests) break;
    }
    dispatchMediaRequests();
}

void FavoritesController::dispatchMediaRequests()
{
    while (m_activeMediaRequests < kMaximumActiveFavoriteMediaRequests &&
           !m_waitingMediaRequests.isEmpty()) {
        Pending pending = m_waitingMediaRequests.dequeue();
        const providers::ProviderId provider = pending.source == QStringLiteral("telegram")
            ? providers::ProviderId::TelegramMedia : providers::ProviderId::YouTubeMedia;
        const QUrl url(pending.imageUrl, QUrl::StrictMode);
        if (!providers::isAllowedProviderUrl(provider, url)) continue;
        const QString cachedSource = m_images->cachedSource(pending.imageUrl);
        if (!cachedSource.isEmpty()) {
            applyMediaSource(pending, cachedSource);
            continue;
        }
        const quint64 operationId = submitNetwork(pending, {provider, url, false, 0});
        if (!operationId || !m_pending.contains(operationId)) {
            continue;
        }
        ++m_activeMediaRequests;
    }
}

void FavoritesController::discardPendingPageMedia(const QString &source)
{
    QQueue<Pending> retained;
    while (!m_waitingMediaRequests.isEmpty()) {
        Pending pending = m_waitingMediaRequests.dequeue();
        if (pending.dashboardImage || pending.source != source)
            retained.enqueue(std::move(pending));
    }
    m_waitingMediaRequests = std::move(retained);
}

void FavoritesController::applyMediaSource(const Pending &pending, const QString &localSource)
{
    if (pending.dashboardImage) {
        if (!m_dashboardNews) return;
        for (int row = 0; row < m_dashboardNews->count(); ++row) {
            const QVariantMap current = m_dashboardNews->itemAt(row);
            const QVariantList images = current.value(QStringLiteral("images")).toList();
            if (current.value(QStringLiteral("itemId")).toString() != pending.itemId ||
                pending.imageIndex < 0 || pending.imageIndex >= images.size() ||
                images.at(pending.imageIndex).toString() != pending.imageUrl) continue;
            m_dashboardNews->updateImageSource(pending.itemId, pending.imageIndex, localSource);
            break;
        }
        return;
    }
    models::ProviderItemsModel *model = contentModel(pending.source);
    for (int row = 0; row < model->count(); ++row) {
        const QVariantMap current = model->itemAt(row);
        const QVariantList images = current.value(QStringLiteral("images")).toList();
        if (current.value(QStringLiteral("itemId")).toString() != pending.itemId ||
            pending.imageIndex < 0 || pending.imageIndex >= images.size() ||
            images.at(pending.imageIndex).toString() != pending.imageUrl) continue;
        model->updateImageSource(pending.itemId, pending.imageIndex, localSource);
        break;
    }
}

} // namespace something::controllers




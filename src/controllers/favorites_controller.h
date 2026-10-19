#pragma once

#include "../application/application_service_graph.h"
#include "../domain/clock.h"
#include "../models/dashboard_news_model.h"
#include "../models/provider_items_model.h"
#include "../providers/provider_image_provider.h"

#include <QObject>
#include <QHash>
#include <QQueue>
#include <QVariantMap>

#include <memory>

namespace something::controllers {

class FavoritesController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString activeSource READ activeSource NOTIFY activeSourceChanged)
    Q_PROPERTY(something::models::ProviderItemsModel *telegramPosts READ telegramPosts CONSTANT)
    Q_PROPERTY(something::models::ProviderItemsModel *youTubeVideos READ youTubeVideos CONSTANT)
    Q_PROPERTY(something::models::FavoriteChannelsModel *telegramFavorites READ telegramFavorites CONSTANT)
    Q_PROPERTY(something::models::FavoriteChannelsModel *youTubeFavorites READ youTubeFavorites CONSTANT)
    Q_PROPERTY(something::models::FavoriteCategoriesModel *telegramCategories READ telegramCategories CONSTANT)
    Q_PROPERTY(something::models::FavoriteCategoriesModel *youTubeCategories READ youTubeCategories CONSTANT)
    Q_PROPERTY(QString telegramChannel READ telegramChannel NOTIFY telegramStateChanged)
    Q_PROPERTY(QString youTubeChannel READ youTubeChannel NOTIFY youTubeStateChanged)
    Q_PROPERTY(QString youTubeChannelId READ youTubeChannelId NOTIFY youTubeStateChanged)
    Q_PROPERTY(bool telegramLoading READ telegramLoading NOTIFY telegramStateChanged)
    Q_PROPERTY(bool youTubeLoading READ youTubeLoading NOTIFY youTubeStateChanged)
    Q_PROPERTY(bool telegramHasOlder READ telegramHasOlder NOTIFY telegramStateChanged)
    Q_PROPERTY(QString telegramError READ telegramError NOTIFY telegramStateChanged)
    Q_PROPERTY(QString youTubeError READ youTubeError NOTIFY youTubeStateChanged)
    Q_PROPERTY(bool favoritesLoading READ favoritesLoading NOTIFY stateChanged)
    Q_PROPERTY(bool mutationBusy READ mutationBusy NOTIFY stateChanged)
    Q_PROPERTY(QString mutationError READ mutationError NOTIFY stateChanged)
    Q_PROPERTY(bool newsScanning READ newsScanning NOTIFY stateChanged)
    Q_PROPERTY(bool newsScanPartial READ newsScanPartial NOTIFY stateChanged)
    Q_PROPERTY(QString newsScanError READ newsScanError NOTIFY stateChanged)
    Q_PROPERTY(QString externalOpenError READ externalOpenError NOTIFY stateChanged)

public:
    FavoritesController(application::ApplicationServiceGraph *graph,
                        const domain::Clock *clock,
                        providers::ProviderImageProvider *images,
                        QObject *parent = nullptr);

    QString activeSource() const { return m_activeSource; }
    models::ProviderItemsModel *telegramPosts() noexcept { return &m_telegramPosts; }
    models::ProviderItemsModel *youTubeVideos() noexcept { return &m_youTubeVideos; }
    models::FavoriteChannelsModel *telegramFavorites() noexcept { return &m_telegramFavorites; }
    models::FavoriteChannelsModel *youTubeFavorites() noexcept { return &m_youTubeFavorites; }
    models::FavoriteCategoriesModel *telegramCategories() noexcept { return &m_telegramCategories; }
    models::FavoriteCategoriesModel *youTubeCategories() noexcept { return &m_youTubeCategories; }
    QString telegramChannel() const { return m_telegramChannel; }
    QString youTubeChannel() const { return m_youTubeChannel; }
    QString youTubeChannelId() const { return m_youTubeChannelId; }
    bool telegramLoading() const noexcept { return m_telegramLoading; }
    bool youTubeLoading() const noexcept { return m_youTubeLoading; }
    bool telegramHasOlder() const noexcept { return m_telegramHasOlder; }
    QString telegramError() const { return m_telegramError; }
    QString youTubeError() const { return m_youTubeError; }
    bool favoritesLoading() const noexcept { return m_favoritesLoading; }
    bool mutationBusy() const noexcept { return m_mutationBusy; }
    QString mutationError() const { return m_mutationError; }
    bool newsScanning() const noexcept { return m_newsScanning; }
    bool newsScanPartial() const noexcept { return m_newsScanPartial; }
    QString newsScanError() const { return m_newsScanError; }
    QString externalOpenError() const { return m_externalOpenError; }

    Q_INVOKABLE void setSource(QString source);
    Q_INVOKABLE void loadChannel(QString source, QString reference, bool refresh = false);
    Q_INVOKABLE void refreshChannel();
    Q_INVOKABLE void loadOlderTelegramPosts();
    Q_INVOKABLE void addFavorite(QString source, QString categoryId,
                                 QString newCategoryName = {}, QString newCategoryColor = {});
    Q_INVOKABLE void removeFavorite(QString source, QString sourceId);
    Q_INVOKABLE void assignFavoriteCategory(QString source, QString sourceId,
                                            QString categoryId);
    Q_INVOKABLE void createCategory(QString source, QString name, QString color = {});
    Q_INVOKABLE void updateCategory(QString source, qint64 id, QString name, QString color);
    Q_INVOKABLE void moveCategory(QString source, qint64 id, int direction);
    Q_INVOKABLE void deleteCategory(QString source, qint64 id, QString mode,
                                    QString targetCategoryId = {});
    Q_INVOKABLE void refreshNews();
    Q_INVOKABLE void openExternal(QString source, QString url);
    void setDashboardNewsModel(models::DashboardNewsModel *model) noexcept { m_dashboardNews = model; }
    void loadNewsImages(QVariantList items);

signals:
    void activeSourceChanged();
    void channelReferenceChanged(QString source, QString reference);
    void telegramStateChanged();
    void youTubeStateChanged();
    void stateChanged();
    void newsChanged();
    void newsScanFinished(QString error, bool partial);
    void externalOpenFailed(QString message);

private:
    enum class PendingKind {
        Snapshot, Mutation, AddFavoriteCategory, FavoriteAdd, FavoriteAssignment,
        ChannelRequest, ChannelParse,
        NewsPrepare, NewsRequest, NewsParse, NewsCommit, MediaRequest, MediaDecode
    };
    struct Pending final {
        PendingKind kind{PendingKind::Snapshot};
        QString source;
        QString reference;
        QString channelId;
        QString cacheKey;
        QString itemId;
        QString imageUrl;
        qint64 categoryId{0};
        int imageIndex{-1};
        quint64 before{0};
        bool refresh{false};
        bool append{false};
        bool resolveHandle{false};
        bool dashboardImage{false};
        bool preserveMutationError{false};
        quint64 scanGeneration{0};
        QVariantMap sourcePlan;
    };
    struct NewsScan final {
        quint64 generation{0};
        bool appOpen{false};
        QString scanStartedAt;
        QVector<QVariantMap> waiting;
        int nextIndex{0};
        int activeFetches{0};
        QVector<storage::FavoriteNewsSourceScan> completed;
        QVector<QVariantMap> errors;
    };

    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void onHttpsFinished(quint64 operationId, const QByteArray &body,
                         domain::AppError error);
    void onValueJobFinished(quint64 operationId, const QVariantMap &values,
                            domain::AppError error);
    quint64 submitStorage(Pending pending, storage::StorageCommand command,
                          QString freshnessKey = {});
    quint64 submitNetwork(Pending pending, providers::ProviderRequest request,
                          QString freshnessKey = {});
    quint64 submitParse(Pending pending, QByteArray body,
                        QString freshnessKey = {});
    void loadSnapshot(const QString &source, bool preserveMutationError = false);
    void startTelegramRequest(QString channel, quint64 before, bool refresh, bool append);
    void startYouTubeRequest(QString reference, bool refresh);
    void startYouTubeFeed(QString channelId, QString originalReference, bool refresh);
    void setChannelFailure(const QString &source, const QString &message);
    void cancelChannelOperations(const QString &source);
    void startNewsScan(bool appOpen);
    void dispatchNewsFetches();
    void completeNewsFetch(storage::FavoriteNewsSourceScan batch,
                           const QVariantMap &plan, quint64 generation);
    void finishNewsScanIfDrained();
    void cancelNewsScan();
    void startMediaLoads(const QString &source, const QVariantList &items,
                         models::ProviderItemsModel *model);
    void startDashboardMediaLoads(const QVariantList &items);
    void dispatchMediaRequests();
    void discardPendingPageMedia(const QString &source);
    void applyMediaSource(const Pending &pending, const QString &localSource);
    void cancelPending(quint64 id);
    void setMutationFailure(domain::AppError error);
    models::ProviderItemsModel *contentModel(const QString &source);
    models::FavoriteChannelsModel *favoriteModel(const QString &source);
    models::FavoriteCategoriesModel *categoryModel(const QString &source);

    application::ApplicationServiceGraph *m_graph{nullptr};
    const domain::Clock *m_clock{nullptr};
    providers::ProviderImageProvider *m_images{nullptr};
    models::DashboardNewsModel *m_dashboardNews{nullptr};
    models::ProviderItemsModel m_telegramPosts;
    models::ProviderItemsModel m_youTubeVideos;
    models::FavoriteChannelsModel m_telegramFavorites;
    models::FavoriteChannelsModel m_youTubeFavorites;
    models::FavoriteCategoriesModel m_telegramCategories;
    models::FavoriteCategoriesModel m_youTubeCategories;
    QHash<quint64, Pending> m_pending;
    QHash<QString, quint64> m_channelOperations;
    QHash<QString, quint64> m_snapshotOperations;
    QQueue<Pending> m_waitingMediaRequests;
    int m_activeMediaRequests{0};
    std::unique_ptr<NewsScan> m_newsScan;
    quint64 m_newsPrepareOperation{0};
    quint64 m_newsCommitOperation{0};
    quint64 m_nextScanGeneration{1};
    QString m_activeSource{QStringLiteral("telegram")};
    QString m_telegramChannel{QStringLiteral("toporlive")};
    QString m_youTubeChannel{QStringLiteral("T2X2_latest_news")};
    QString m_youTubeChannelId;
    bool m_telegramLoading{false};
    bool m_youTubeLoading{false};
    bool m_telegramHasOlder{false};
    bool m_favoritesLoading{false};
    bool m_mutationBusy{false};
    bool m_newsScanning{false};
    bool m_newsScanPartial{false};
    bool m_openScanStarted{false};
    QString m_telegramError;
    QString m_youTubeError;
    QString m_mutationError;
    QString m_newsScanError;
    QString m_externalOpenError;
};

} // namespace something::controllers

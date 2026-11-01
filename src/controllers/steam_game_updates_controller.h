#pragma once

#include "../application/application_service_graph.h"
#include "../domain/steam_game_updates.h"
#include "../providers/network_worker.h"
#include "../services/steam_artwork_store.h"
#include "../storage/repository_types.h"

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QVariantList>

#include <optional>

namespace something::controllers {

class SteamGameUpdatesController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList games READ games NOTIFY stateChanged)
    Q_PROPERTY(QVariantList newsItems READ newsItems NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool snapshotLoaded READ snapshotLoaded NOTIFY stateChanged)
    Q_PROPERTY(bool addBusy READ addBusy NOTIFY stateChanged)
    Q_PROPERTY(QString addError READ addError NOTIFY stateChanged)
    Q_PROPERTY(bool removeBusy READ removeBusy NOTIFY stateChanged)
    Q_PROPERTY(bool refreshBusy READ refreshBusy NOTIFY stateChanged)
    Q_PROPERTY(QString refreshStatus READ refreshStatus NOTIFY stateChanged)
    Q_PROPERTY(bool newsLoading READ newsLoading NOTIFY stateChanged)
    Q_PROPERTY(bool newsHasMore READ newsHasMore NOTIFY stateChanged)
    Q_PROPERTY(QString newsError READ newsError NOTIFY stateChanged)
    Q_PROPERTY(bool newsMutationBusy READ newsMutationBusy NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY stateChanged)
    Q_PROPERTY(quint32 selectedAppId READ selectedAppId NOTIFY stateChanged)
    Q_PROPERTY(bool unreadOnly READ unreadOnly NOTIFY stateChanged)
    Q_PROPERTY(int totalUnreadCount READ totalUnreadCount NOTIFY stateChanged)

public:
    explicit SteamGameUpdatesController(application::ApplicationServiceGraph *graph,
                                        QObject *parent = nullptr);

    QVariantList games() const { return m_games; }
    QVariantList newsItems() const { return m_newsItems; }
    bool loading() const noexcept { return m_snapshotBusy; }
    bool snapshotLoaded() const noexcept { return m_snapshotLoaded; }
    bool addBusy() const noexcept { return m_addBusy; }
    QString addError() const { return m_addError; }
    bool removeBusy() const noexcept { return m_removeBusy; }
    bool refreshBusy() const noexcept { return m_refreshBusy; }
    QString refreshStatus() const { return m_refreshStatus; }
    bool newsLoading() const noexcept { return m_newsLoading; }
    bool newsHasMore() const noexcept { return m_newsHasMore; }
    QString newsError() const { return m_newsError; }
    bool newsMutationBusy() const noexcept { return m_newsMutationBusy; }
    QString errorMessage() const { return m_errorMessage; }
    QString notification() const { return m_notification; }
    quint32 selectedAppId() const noexcept { return m_selectedAppId; }
    bool unreadOnly() const noexcept { return m_unreadOnly; }
    int totalUnreadCount() const;

    Q_INVOKABLE void addGame(QString storeUrl);
    Q_INVOKABLE void removeGame(quint32 appId);
    Q_INVOKABLE void setOwnershipStatus(quint32 appId, bool owned);
    Q_INVOKABLE void setSelectedAppId(quint32 appId);
    Q_INVOKABLE void setUnreadOnly(bool unreadOnly);
    Q_INVOKABLE void setSearchText(QString text);
    Q_INVOKABLE void loadMoreNews();
    Q_INVOKABLE void refreshGame(quint32 appId);
    Q_INVOKABLE void refreshAll();
    Q_INVOKABLE void loadOlderFromSteam(quint32 appId);
    Q_INVOKABLE void cancelRefresh();
    Q_INVOKABLE void setNewsRead(quint32 appId, QString newsId, bool isRead);
    Q_INVOKABLE void markAllRead(quint32 appId);
    Q_INVOKABLE void openArticle(quint32 appId, QString newsId);
    Q_INVOKABLE void openStore(quint32 appId);
    Q_INVOKABLE void retryLoad();
    Q_INVOKABLE void clearMessages();

signals:
    void stateChanged();
    void artworkInvalidated(const QString &filename);
    void newNewsDiscovered(quint32 appId, const QVariantList &articles,
                           const QString &timestampUtc);

private:
    enum class Kind {
        Snapshot, AddNetwork, AddParse, AddInsert, RemoveFollow, SetOwnership,
        NewsPage, MarkRead, MarkAllRead, OldestNewsTimestamp, RefreshNetwork, RefreshParse,
        SaveNewsPage, RecordNewsCheck, ArtworkNetwork, ArtworkStage, ArtworkSave
    };
    struct Pending final {
        Kind kind{Kind::Snapshot};
        quint32 appId{0};
        QString storeUrl;
        QString newsId;
        QString attemptedAtUtc;
        QString safeError;
        QString artworkUrl;
        QVariantList sourceArticles;
        bool success{false};
        bool appendPage{false};
        quint64 generation{0};
    };
    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    void onHttpsFinished(quint64 id, const QByteArray &body, domain::AppError error);
    void onValueJobFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    quint64 submitStorage(Kind kind, storage::StorageCommand command, Pending pending);
    quint64 submitNetwork(Kind kind, providers::ProviderRequest request, Pending pending);
    quint64 submitValueJob(Kind kind, QByteArray body, Pending pending);
    quint64 submitArtworkStage(QByteArray body, Pending pending);
    void requestSnapshot();
    void requestNewsPage(bool append);
    void reloadNews();
    void startRefresh(std::optional<quint32> appId = std::nullopt,
                      std::optional<qint64> olderThanUnixSeconds = std::nullopt);
    void dispatchRefreshQueue();
    void scheduleRefreshDispatch();
    void recordRefreshCheck(quint32 appId, const QString &attemptedAtUtc,
                            bool succeeded, const QString &safeError,
                            quint64 generation);
    void completeRefreshItem(bool succeeded, quint64 generation);
    void finishRefresh();
    void cancelAll();
    void cancelPending(quint64 id);
    void updateGame(QVariantMap game);
    void setLocalRefreshFailure(quint32 appId, const QString &attemptedAtUtc,
                                const QString &error);
    void setGameUnreadCount(quint32 appId, int count);
    int gameIndex(quint32 appId) const;
    void queueArtwork(quint32 appId);
    void dispatchArtworkQueue();
    void scheduleArtworkDispatch();
    void finishArtwork(quint32 appId, const QString &url);
    QString safeNewsError(domain::AppError error) const;
    static QVariantMap articleMap(const domain::SteamNewsArticle &article);
    static domain::SteamNewsArticle articleFromMap(const QVariantMap &map);
    static QString currentTimestamp();

    application::ApplicationServiceGraph *m_graph{nullptr};
    QHash<quint64, Pending> m_pending;
    QSet<quint32> m_reservedAppIds;
    QHash<quint32, QString> m_activeArtworkUrls;
    QVariantList m_games;
    QVariantList m_newsItems;
    QString m_addError;
    QString m_newsError;
    QString m_errorMessage;
    QString m_notification;
    QString m_searchText;
    QString m_refreshStatus;
    std::optional<quint32> m_newsAppFilter;
    std::optional<quint32> m_refreshSingleAppId;
    storage::SteamGameUpdateNewsCursor m_newsCursor;
    std::optional<qint64> m_refreshOlderThanUnixSeconds;
    quint64 m_newsGeneration{0};
    quint64 m_refreshGeneration{0};
    quint64 m_artworkGeneration{0};
    quint32 m_selectedAppId{0};
    int m_refreshNextGameIndex{0};
    int m_activeRefreshRequests{0};
    int m_refreshTotal{0};
    int m_refreshFinishedCount{0};
    int m_refreshSuccessCount{0};
    int m_refreshFailureCount{0};
    int m_activeArtworkRequests{0};
    int m_nextArtworkGameIndex{0};
    bool m_ready{false};
    bool m_snapshotStarted{false};
    bool m_snapshotBusy{false};
    bool m_snapshotLoaded{false};
    bool m_addBusy{false};
    bool m_removeBusy{false};
    bool m_newsLoading{false};
    bool m_newsHasMore{false};
    bool m_newsMutationBusy{false};
    bool m_unreadOnly{false};
    bool m_refreshBusy{false};
    bool m_refreshDispatchScheduled{false};
    bool m_artworkDispatchScheduled{false};
};

} // namespace something::controllers

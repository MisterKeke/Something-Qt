#pragma once

#include "../application/application_service_graph.h"
#include "../domain/steam_game.h"
#include "../models/steam_games_model.h"
#include "../services/steam_artwork_store.h"
#include "../providers/network_worker.h"

#include <QObject>
#include <QHash>
#include <QSet>
#include <QUrl>
#include <QVariantList>
#include <QVector>

namespace something::controllers {

class SteamGamesController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::SteamGamesModel *games READ games CONSTANT)
    Q_PROPERTY(QString countryCode READ countryCode NOTIFY settingsChanged)
    Q_PROPERTY(QString selectedCountry READ selectedCountry NOTIFY settingsChanged)
    Q_PROPERTY(int stalePriceCount READ stalePriceCount NOTIFY stateChanged)
    Q_PROPERTY(QVariantList countries READ countries NOTIFY settingsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool snapshotLoaded READ snapshotLoaded NOTIFY stateChanged)
    Q_PROPERTY(bool addBusy READ addBusy NOTIFY stateChanged)
    Q_PROPERTY(QString addError READ addError NOTIFY stateChanged)
    Q_PROPERTY(bool refreshBusy READ refreshBusy NOTIFY stateChanged)
    Q_PROPERTY(QString refreshStatus READ refreshStatus NOTIFY stateChanged)
    Q_PROPERTY(bool countryBusy READ countryBusy NOTIFY stateChanged)
    Q_PROPERTY(bool deleteBusy READ deleteBusy NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY stateChanged)
    Q_PROPERTY(bool historyLoading READ historyLoading NOTIFY stateChanged)
    Q_PROPERTY(QString historyError READ historyError NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap historyGame READ historyGame NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap historyStatistics READ historyStatistics NOTIFY stateChanged)
    Q_PROPERTY(QVariantList historyPoints READ historyPoints NOTIFY stateChanged)
    Q_PROPERTY(QString historyRange READ historyRange NOTIFY stateChanged)

public:
    explicit SteamGamesController(application::ApplicationServiceGraph *graph,
                                  QObject *parent = nullptr);

    models::SteamGamesModel *games() noexcept { return &m_games; }
    QString countryCode() const { return m_countryCode; }
    QString selectedCountry() const { return m_selectedCountry; }
    int stalePriceCount() const;
    QVariantList countries() const { return m_countries; }
    bool loading() const noexcept { return m_snapshotBusy; }
    bool snapshotLoaded() const noexcept { return m_snapshotLoaded; }
    bool addBusy() const noexcept { return m_addBusy; }
    QString addError() const { return m_addError; }
    bool refreshBusy() const noexcept { return m_refreshBusy; }
    QString refreshStatus() const { return m_refreshStatus; }
    bool countryBusy() const noexcept { return m_countryBusy; }
    bool deleteBusy() const noexcept { return m_deleteBusy; }
    QString errorMessage() const { return m_errorMessage; }
    QString notification() const { return m_notification; }
    bool historyLoading() const noexcept { return m_historyLoading; }
    QString historyError() const { return m_historyError; }
    QVariantMap historyGame() const { return m_historyGame; }
    QVariantMap historyStatistics() const { return m_historyStatistics; }
    QVariantList historyPoints() const { return m_historyPoints; }
    QString historyRange() const { return m_historyRange; }

    Q_INVOKABLE void setSelectedCountry(QString countryCode);
    Q_INVOKABLE void saveCountry();
    Q_INVOKABLE void addGame(QString storeUrl);
    Q_INVOKABLE void refreshPrices();
    Q_INVOKABLE void deleteGame(qint64 gameId);
    Q_INVOKABLE void openStore(qint64 gameId);
    Q_INVOKABLE void retryLoad();
    Q_INVOKABLE void clearNotification();
    Q_INVOKABLE void openPriceHistory(qint64 gameId);
    Q_INVOKABLE void setPriceHistoryRange(QString range);
    Q_INVOKABLE void reloadPriceHistory();
    Q_INVOKABLE void closePriceHistory();
    Q_INVOKABLE QString formatPrice(qint64 amountMinor, QString currencyCode) const;

signals:
    void stateChanged();
    void settingsChanged();
    void artworkInvalidated(const QString &filename);

private:
    enum class Kind { Snapshot, CountrySave, AddNetwork, AddParse, AddInsert,
                      Delete, RefreshNetwork, RefreshParse, RefreshCommit,
                      ArtworkNetwork, ArtworkStage, ArtworkSave, PriceHistory };
    struct RefreshWork final {
        QVariantMap game;
        QString attemptedAt;
    };
    struct Pending final {
        Kind kind{Kind::Snapshot};
        QString canonicalUrl;
        QString countryCode;
        QString attemptedAt;
        qint64 gameId{0};
        quint32 appId{0};
        int refreshIndex{-1};
        quint64 refreshGeneration{0};
        quint64 artworkGeneration{0};
        quint64 historyGeneration{0};
        QVariantMap game;
        bool autoRefreshAfterSnapshot{false};
        bool manualRefreshAfterSnapshot{false};
    };

    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    void onHttpsFinished(quint64 id, const QByteArray &body, domain::AppError error);
    void onValueJobFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    quint64 submitStorage(Kind kind, storage::StorageCommand command, Pending pending);
    quint64 submitNetwork(Kind kind, providers::ProviderRequest request, Pending pending);
    quint64 submitParse(Kind kind, QByteArray body, Pending pending);
    quint64 submitArtworkStage(QByteArray body, Pending pending);
    void requestSnapshot(bool autoRefresh, bool manualRefreshAfterLoad = false);
    void beginRefresh();
    void dispatchRefreshQueue();
    void completeRefreshItem(int index, const domain::SteamGameDetails *details,
                             QString safeError);
    void finishRefreshBatch();
    void requestPriceHistory();
    void queueArtwork(qint64 gameId, quint32 appId, const QString &url);
    void cancelAll();
    void cancelPending(quint64 id);
    void cancelArtworkForGame(qint64 gameId);
    void releaseAddReservation(quint32 appId);
    void setSafeError(domain::AppError error, const QString &fallback);
    static QString safeRefreshError(domain::AppError error);
    static domain::SteamGameDetails detailsFromMap(const QVariantMap &map);
    static QVariantMap detailsToMap(const domain::SteamGameDetails &details);
    static QUrl appDetailsUrl(quint32 appId, const QString &countryCode);
    static QString currentTimestamp();

    application::ApplicationServiceGraph *m_graph{nullptr};
    models::SteamGamesModel m_games;
    QHash<quint64, Pending> m_pending;
    QHash<qint64, quint64> m_artworkGenerationByGame;
    QSet<quint32> m_reservedAppIds;
    QVariantList m_countries;
    QVector<RefreshWork> m_refreshWork;
    QVector<storage::SteamRefreshUpdate> m_refreshUpdates;
    QVector<bool> m_refreshFinished;
    QString m_countryCode{QStringLiteral("TR")};
    QString m_selectedCountry{QStringLiteral("TR")};
    QString m_refreshCountry;
    QString m_addError;
    QString m_refreshStatus;
    QString m_errorMessage;
    QString m_notification;
    QVariantMap m_historyGame;
    QVariantMap m_historyStatistics;
    QVariantList m_historyPoints;
    QString m_historyError;
    QString m_historyRange{QStringLiteral("all")};
    quint64 m_refreshGeneration{0};
    quint64 m_historyGeneration{0};
    int m_nextRefreshIndex{0};
    int m_activeRefreshRequests{0};
    int m_refreshFinishedCount{0};
    int m_refreshUpdatedCount{0};
    int m_refreshFailedCount{0};
    bool m_ready{false};
    bool m_snapshotBusy{false};
    bool m_snapshotLoaded{false};
    bool m_autoRefreshStarted{false};
    bool m_manualRefreshAfterLoad{false};
    bool m_addBusy{false};
    bool m_refreshBusy{false};
    bool m_refreshCommitPending{false};
    bool m_countryBusy{false};
    bool m_deleteBusy{false};
    bool m_historyLoading{false};
    qint64 m_historyGameId{0};
};

} // namespace something::controllers

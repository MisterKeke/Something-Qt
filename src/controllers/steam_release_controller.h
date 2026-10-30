#pragma once

#include "../application/application_service_graph.h"
#include "../domain/steam_release.h"
#include "../models/steam_releases_model.h"
#include "../services/steam_artwork_store.h"

#include <QObject>
#include <QHash>
#include <QQueue>
#include <QSet>
#include <QVariantList>
#include <QVector>

namespace something::controllers {

class SteamReleaseController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::SteamReleasesModel *releases READ releases CONSTANT)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool snapshotLoaded READ snapshotLoaded NOTIFY stateChanged)
    Q_PROPERTY(bool addBusy READ addBusy NOTIFY stateChanged)
    Q_PROPERTY(QString addError READ addError NOTIFY stateChanged)
    Q_PROPERTY(bool refreshBusy READ refreshBusy NOTIFY stateChanged)
    Q_PROPERTY(QString refreshStatus READ refreshStatus NOTIFY stateChanged)
    Q_PROPERTY(bool deleteBusy READ deleteBusy NOTIFY stateChanged)
    Q_PROPERTY(bool dateChangeAcknowledgeBusy READ dateChangeAcknowledgeBusy NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY stateChanged)

public:
    explicit SteamReleaseController(application::ApplicationServiceGraph *graph,
                                    QObject *parent = nullptr);

    models::SteamReleasesModel *releases() noexcept { return &m_releases; }
    bool loading() const noexcept { return m_snapshotBusy; }
    bool snapshotLoaded() const noexcept { return m_snapshotLoaded; }
    bool addBusy() const noexcept { return m_addBusy; }
    QString addError() const { return m_addError; }
    bool refreshBusy() const noexcept { return m_refreshBusy; }
    QString refreshStatus() const { return m_refreshStatus; }
    bool deleteBusy() const noexcept { return m_deleteBusy; }
    bool dateChangeAcknowledgeBusy() const noexcept { return m_dateChangeAcknowledgeBusy; }
    QString errorMessage() const { return m_errorMessage; }
    QString notification() const { return m_notification; }

    Q_INVOKABLE void addRelease(QString storeUrl);
    Q_INVOKABLE void refreshReleases();
    Q_INVOKABLE void removeRelease(quint32 appId);
    Q_INVOKABLE void acknowledgeDateChange(quint32 appId);
    Q_INVOKABLE void openStore(quint32 appId);
    Q_INVOKABLE void retryLoad();
    Q_INVOKABLE void clearNotification();
    Q_INVOKABLE void setSearchText(QString text);
    Q_INVOKABLE void setStatusFilter(QString filter);
    Q_INVOKABLE void setSortOrder(QString order);
    Q_INVOKABLE void ensureArtwork(quint32 appId);

signals:
    void stateChanged();
    void artworkInvalidated(const QString &filename);

private:
    enum class Kind { Snapshot, AddNetwork, AddParse, AddInsert, Delete,
                      RefreshNetwork, RefreshParse, RefreshCommit, DateChangeAcknowledge,
                      ArtworkNetwork, ArtworkStage, ArtworkSave };
    struct Pending final {
        Kind kind{Kind::Snapshot};
        quint32 appId{0};
        QString canonicalStoreUrl;
        QString artworkUrl;
        int refreshIndex{-1};
        quint64 refreshGeneration{0};
        QVariantMap release;
    };
    struct RefreshWork final {
        quint32 appId{0};
        QString canonicalStoreUrl;
        QString attemptedAtUtc;
    };
    struct ArtworkWork final {
        quint32 appId{0};
        QString artworkUrl;
    };

    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    void onHttpsFinished(quint64 id, const QByteArray &body, domain::AppError error);
    void onValueJobFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    quint64 submitStorage(Kind kind, storage::StorageCommand command, Pending pending);
    quint64 submitNetwork(Kind kind, providers::ProviderRequest request, Pending pending);
    quint64 submitParse(Kind kind, QByteArray body, Pending pending);
    quint64 submitArtworkStage(QByteArray body, Pending pending);
    void requestSnapshot();
    void dispatchRefreshQueue();
    void completeRefreshItem(int index, const domain::SteamReleaseDetails *details,
                             QString safeError);
    void finishRefreshBatch();
    void releaseAddReservation(quint32 appId);
    void dispatchArtworkQueue();
    void finishArtwork(quint32 appId, const QString &artworkUrl);
    void cancelAll();
    void cancelPending(quint64 id);
    void setSafeError(domain::AppError error, const QString &fallback);
    static QString safeProviderMessage(domain::AppError error, const QString &fallback);
    static QString currentTimestamp();
    static domain::SteamReleaseDetails detailsFromMap(const QVariantMap &map);
    static QVariantMap detailsToMap(const domain::SteamReleaseDetails &details);

    application::ApplicationServiceGraph *m_graph{nullptr};
    models::SteamReleasesModel m_releases;
    QHash<quint64, Pending> m_pending;
    QSet<quint32> m_reservedAppIds;
    QVector<RefreshWork> m_refreshWork;
    QVector<storage::SteamReleaseRefreshUpdate> m_refreshUpdates;
    QVector<bool> m_refreshFinished;
    QVector<bool> m_refreshInFlight;
    QQueue<ArtworkWork> m_artworkQueue;
    QHash<quint32, QString> m_queuedArtworkUrls;
    QHash<quint32, QString> m_activeArtworkUrls;
    QString m_addError;
    QString m_refreshStatus;
    QString m_errorMessage;
    QString m_notification;
    quint64 m_refreshGeneration{0};
    int m_nextRefreshIndex{0};
    int m_activeRefreshRequests{0};
    int m_refreshFinishedCount{0};
    int m_refreshUpdatedCount{0};
    int m_refreshFailedCount{0};
    int m_activeArtworkRequests{0};
    bool m_ready{false};
    bool m_snapshotStarted{false};
    bool m_snapshotBusy{false};
    bool m_snapshotLoaded{false};
    bool m_addBusy{false};
    bool m_refreshBusy{false};
    bool m_refreshCommitPending{false};
    bool m_deleteBusy{false};
    bool m_dateChangeAcknowledgeBusy{false};
};

} // namespace something::controllers

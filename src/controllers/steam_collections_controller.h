#pragma once

#include "../application/application_service_graph.h"
#include "../domain/steam_collections.h"
#include "../providers/network_worker.h"

#include <QObject>
#include <QHash>
#include <QSet>
#include <QVariantList>
#include <QVector>

namespace something::controllers {

class SteamCollectionsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList collections READ collections NOTIFY stateChanged)
    Q_PROPERTY(QVariantList games READ games NOTIFY stateChanged)
    Q_PROPERTY(QVariantList catalog READ catalog NOTIFY stateChanged)
    Q_PROPERTY(int allGamesCount READ allGamesCount NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool snapshotLoaded READ snapshotLoaded NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool addBusy READ addBusy NOTIFY stateChanged)
    Q_PROPERTY(qint64 selectedCollectionId READ selectedCollectionId NOTIFY stateChanged)
    Q_PROPERTY(QString selectedCollectionName READ selectedCollectionName NOTIFY stateChanged)
    Q_PROPERTY(QString searchText READ searchText NOTIFY stateChanged)
    Q_PROPERTY(QString sortOrder READ sortOrder NOTIFY stateChanged)
    Q_PROPERTY(QString statusFilter READ statusFilter NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString addError READ addError NOTIFY stateChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY stateChanged)

public:
    explicit SteamCollectionsController(
        application::ApplicationServiceGraph *graph, QObject *parent = nullptr);

    QVariantList collections() const { return m_collections; }
    QVariantList games() const { return m_games; }
    QVariantList catalog() const { return m_catalog; }
    int allGamesCount() const;
    bool loading() const noexcept { return m_loading; }
    bool snapshotLoaded() const noexcept { return m_snapshotLoaded; }
    bool busy() const noexcept { return m_busy; }
    bool addBusy() const noexcept { return m_addBusy; }
    qint64 selectedCollectionId() const noexcept { return m_selectedCollectionId; }
    QString selectedCollectionName() const;
    QString searchText() const { return m_searchText; }
    QString sortOrder() const { return m_sortOrder; }
    QString statusFilter() const { return m_statusFilter; }
    QString errorMessage() const { return m_errorMessage; }
    QString addError() const { return m_addError; }
    QString notification() const { return m_notification; }

    Q_INVOKABLE void selectCollection(qint64 collectionId);
    Q_INVOKABLE void setSearchText(QString text);
    Q_INVOKABLE void setSortOrder(QString order);
    Q_INVOKABLE void setStatusFilter(QString filter);
    Q_INVOKABLE void createCollection(QString name, QString description,
                                      QString color, QString icon);
    Q_INVOKABLE void updateCollection(qint64 collectionId, QString name,
                                      QString description, QString color,
                                      QString icon);
    Q_INVOKABLE void moveCollection(qint64 collectionId, int direction);
    Q_INVOKABLE void deleteCollection(qint64 collectionId);
    Q_INVOKABLE void addGame(QString storeUrl);
    Q_INVOKABLE void addExistingGame(quint32 appId, qint64 collectionId);
    Q_INVOKABLE void updateGame(qint64 collectionId, quint32 appId,
                                QString status, int rating, QString comment);
    Q_INVOKABLE void removeGame(qint64 collectionId, quint32 appId);
    Q_INVOKABLE void copyGame(qint64 sourceCollectionId,
                              qint64 targetCollectionId, quint32 appId);
    Q_INVOKABLE void moveGame(qint64 sourceCollectionId,
                              qint64 targetCollectionId, quint32 appId);
    Q_INVOKABLE QVariantList availableCatalogGames(qint64 collectionId) const;
    Q_INVOKABLE void openStore(quint32 appId);
    Q_INVOKABLE void refreshSnapshot();
    Q_INVOKABLE void retryLoad();
    Q_INVOKABLE void clearNotification();
    Q_INVOKABLE void clearMessages();

signals:
    void stateChanged();

private:
    enum class Kind {
        Snapshot, Mutation, AddNetwork, AddParse, AddInsert,
        ArtworkNetwork, ArtworkStage, ArtworkSave
    };
    struct Pending final {
        Kind kind{Kind::Snapshot};
        QString mutationType;
        QString canonicalUrl;
        QString addReservation;
        QString artworkUrl;
        qint64 collectionId{0};
        quint32 appId{0};
    };

    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 id, const QVariantMap &values,
                           domain::AppError error);
    void onHttpsFinished(quint64 id, const QByteArray &body,
                         domain::AppError error);
    void onValueJobFinished(quint64 id, const QVariantMap &values,
                            domain::AppError error);
    quint64 submitStorage(Kind kind, storage::StorageCommand command,
                          Pending pending);
    quint64 submitNetwork(Kind kind, providers::ProviderRequest request,
                          Pending pending);
    quint64 submitParse(QByteArray body, Pending pending);
    quint64 submitArtworkStage(QByteArray body, Pending pending);
    bool submitMutation(storage::StorageCommand command, QString mutationType,
                        qint64 collectionId = 0);
    void requestSnapshot();
    void applySnapshot(const QVariantMap &values);
    void rebuildGames();
    void queueArtworkScan();
    void dispatchArtworkScan();
    void scheduleArtworkScan();
    void finishArtwork(quint32 appId);
    void finishMutation(const Pending &pending, const QVariantMap &values,
                        domain::AppError error);
    void cancelAll();
    void cancelPending(quint64 id);
    bool hasMembership(qint64 collectionId, quint32 appId) const;
    QString reservationKey(qint64 collectionId, quint32 appId) const;

    application::ApplicationServiceGraph *m_graph{nullptr};
    QHash<quint64, Pending> m_pending;
    QSet<QString> m_reservedMemberships;
    QSet<quint32> m_activeArtworkApps;
    QVariantList m_collections;
    QVariantList m_memberships;
    QVariantList m_catalog;
    QVariantList m_games;
    QString m_searchText;
    QString m_sortOrder{QStringLiteral("name_asc")};
    QString m_statusFilter{QStringLiteral("all")};
    QString m_errorMessage;
    QString m_addError;
    QString m_notification;
    qint64 m_selectedCollectionId{0};
    bool m_ready{false};
    bool m_loading{false};
    bool m_snapshotStarted{false};
    bool m_snapshotLoaded{false};
    bool m_busy{false};
    bool m_addBusy{false};
    int m_nextArtworkIndex{0};
    bool m_artworkScanActive{false};
    bool m_artworkDispatchScheduled{false};
};

} // namespace something::controllers

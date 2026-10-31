#pragma once

#include "../application/application_service_graph.h"
#include "../platform/application_usage_platform.h"
#include "../storage/application_usage_repository.h"

#include <QElapsedTimer>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QStringList>

namespace something::services {

class ApplicationUsageService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool trackingEnabled READ trackingEnabled NOTIFY stateChanged)
    Q_PROPERTY(bool trackingActive READ trackingActive NOTIFY stateChanged)
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(int retentionDays READ retentionDays NOTIFY stateChanged)
    Q_PROPERTY(QString trackingStatus READ trackingStatus NOTIFY stateChanged)
    Q_PROPERTY(QVariantList exclusions READ exclusions NOTIFY stateChanged)
    Q_PROPERTY(QVariantList liveApplications READ liveApplications NOTIFY liveApplicationsChanged)

public:
    ApplicationUsageService(something::application::ApplicationServiceGraph *graph,
                            something::platform::ApplicationUsagePlatform *platform,
                            QObject *parent = nullptr);
    ~ApplicationUsageService() override;

    bool ready() const noexcept { return m_configurationLoaded; }
    bool trackingEnabled() const noexcept { return m_enabled; }
    bool trackingActive() const noexcept { return m_trackingActive; }
    bool supported() const noexcept { return m_platform && m_platform->supported(); }
    int retentionDays() const noexcept { return m_retentionDays; }
    QString trackingStatus() const;
    QVariantList exclusions() const;
    QVariantList liveApplications() const;
    QString liveState(const QString &applicationId) const;
    void prepareShutdown();

    Q_INVOKABLE void setTrackingEnabled(bool enabled);
    Q_INVOKABLE void setRetentionDays(int days);
    Q_INVOKABLE void addApplicationExclusion(const QString &applicationId,
                                              const QString &canonicalPath,
                                              const QString &executableName);
    Q_INVOKABLE void addFilenameExclusion(const QString &executableName);
    Q_INVOKABLE void removeExclusion(const QString &applicationId);
    Q_INVOKABLE void deleteHistory(const QString &applicationId = {});

signals:
    void stateChanged();
    void liveApplicationsChanged();
    void statisticsChanged();
    // Stable internal lifecycle events for future automation consumers.
    void applicationSessionStarted(QString applicationId, QString sessionId,
                                   QString timestampUtc);
    void applicationStateChanged(QString applicationId, QString sessionId,
                                 QString state, QString timestampUtc);
    void applicationSessionEnded(QString applicationId, QString sessionId,
                                 QString timestampUtc, QString reason);

private:
    struct ActiveSession final {
        platform::ApplicationUsageObservation application;
        QString sessionId;
        QString startedUtc;
        QString lastSeenUtc;
        QString lastCheckpointUtc;
        QString state;
        qint64 lastMonotonicMs{0};
        QDateTime lastWallUtc;
        qint64 durationMs{0};
        int lastUtcOffsetSeconds{0};
    };
    struct PendingOperation final {
        enum class Kind { LoadConfiguration, SetEnabled, SetRetention, AddExclusion,
                          RemoveExclusion, Checkpoint, DeleteHistory } kind;
        QString applicationId;
        storage::ApplicationUsageCheckpoint checkpoint;
    };

    bool submitStorage(PendingOperation operation,
                       something::storage::StorageCommand command);
    void loadConfiguration();
    void onStorageFinished(quint64 operationId, QVariantMap values,
                           something::domain::AppError error);
    void setTrackingActive(bool active);
    void onObservations(const QVector<platform::ApplicationUsageObservation> &applications,
                        bool succeeded, const QString &diagnostic);
    void onSystemSuspendStateChanged(bool suspended);
    void advance(ActiveSession &session, const QString &nextState,
                 qint64 monotonicNow, const QDateTime &wallNowUtc);
    void appendDuration(ActiveSession &session, const QString &state,
                        qint64 durationMs, const QDateTime &startUtc,
                        const QDateTime &endUtc);
    void finishSession(const QString &applicationId, const QString &reason,
                       qint64 monotonicNow, const QDateTime &wallNowUtc);
    bool isExcluded(const platform::ApplicationUsageObservation &application) const;
    void persistExclusion(const storage::ApplicationUsageExclusion &exclusion);
    void checkpoint();
    void submitCheckpointBatch(bool allowParallel);
    storage::ApplicationUsageCheckpoint takeCheckpoint();
    void restoreCheckpoint(storage::ApplicationUsageCheckpoint checkpoint);
    void notifyLiveChanges();
    void processDeferredHistoryDeletes();
    void submitDeleteHistory(const QString &applicationId);

    something::application::ApplicationServiceGraph *m_graph{nullptr};
    something::platform::ApplicationUsagePlatform *m_platform{nullptr};
    QTimer m_checkpointTimer;
    QElapsedTimer m_monotonic;
    QHash<QString, ActiveSession> m_active;
    QVector<storage::ApplicationUsageExclusion> m_exclusions;
    QVector<storage::ApplicationUsageMetadata> m_pendingApplications;
    QVector<storage::ApplicationUsageSession> m_pendingSessions;
    QVector<storage::ApplicationUsageSegment> m_pendingSegments;
    QStringList m_deferredHistoryDeletes;
    QHash<quint64, PendingOperation> m_operations;
    QString m_storageDiagnostic;
    QString m_observationDiagnostic;
    QString m_configurationDiagnostic;
    int m_retentionDays{365};
    qint64 m_omittedGapCount{0};
    qint64 m_omittedGapDurationMs{0};
    bool m_enabled{true};
    bool m_configurationLoaded{false};
    bool m_trackingActive{false};
    int m_checkpointsInFlight{0};
    bool m_deleteInFlight{false};
    bool m_systemLocked{false};
    bool m_systemSuspended{false};
    bool m_checkpointRequested{false};
};

} // namespace something::services

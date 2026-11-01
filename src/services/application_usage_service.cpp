#include "application_usage_service.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QLoggingCategory>
#include <QMetaType>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace something::services {
namespace {

constexpr qsizetype maximumPendingCheckpointRecords = 10000;

Q_LOGGING_CATEGORY(lcUsage, "something.applicationUsage", QtWarningMsg)

QString utcString(const QDateTime &value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QString normalizedPath(QString path)
{
    if (path.isEmpty()) return {};
    path = QDir::cleanPath(path).toCaseFolded();
    return path;
}

bool isKnownState(const QString &state)
{
    return state == QStringLiteral("ForegroundFullscreen")
        || state == QStringLiteral("ForegroundWindowed")
        || state == QStringLiteral("BackgroundVisible")
        || state == QStringLiteral("Minimized")
        || state == QStringLiteral("Unknown");
}

QVariantMap exclusionMap(const storage::ApplicationUsageExclusion &exclusion)
{
    return {{QStringLiteral("applicationId"), exclusion.applicationId},
            {QStringLiteral("canonicalPath"), exclusion.canonicalPath},
            {QStringLiteral("executableName"), exclusion.executableName},
            {QStringLiteral("matchByName"), exclusion.matchByName},
            {QStringLiteral("createdAtUtc"), exclusion.createdAtUtc}};
}

} // namespace

ApplicationUsageService::ApplicationUsageService(
    application::ApplicationServiceGraph *graph,
    platform::ApplicationUsagePlatform *platform, QObject *parent)
    : QObject(parent), m_graph(graph), m_platform(platform)
{
    qRegisterMetaType<QVector<platform::ApplicationUsageObservation>>();
    m_checkpointTimer.setInterval(15000);
    m_checkpointTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_checkpointTimer, &QTimer::timeout, this,
            [this] { checkpoint(); });
    if (m_graph) {
        m_profileMaintenance = m_graph->isProfileMaintenance();
        connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
                this, &ApplicationUsageService::onStorageFinished);
        connect(m_graph, &application::ApplicationServiceGraph::profileMaintenanceChanged,
                this, &ApplicationUsageService::onProfileMaintenanceChanged);
        connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
                this, [this](application::ApplicationLifecycle::State state) {
            if (state == application::ApplicationLifecycle::State::Running)
                loadConfiguration();
            else if (state == application::ApplicationLifecycle::State::Failed)
                setTrackingActive(false);
        });
        if (m_graph->lifecycle().state() == application::ApplicationLifecycle::State::Running
            && !m_profileMaintenance)
            loadConfiguration();
    }
    if (m_platform) {
        connect(m_platform, &platform::ApplicationUsagePlatform::observationsReady,
                this, &ApplicationUsageService::onObservations);
        connect(m_platform, &platform::ApplicationUsagePlatform::suspendStateChanged,
                this, &ApplicationUsageService::onSystemSuspendStateChanged);
    }
    m_monotonic.start();
}

ApplicationUsageService::~ApplicationUsageService()
{
    if (m_platform) m_platform->stop();
    m_checkpointTimer.stop();
}

QString ApplicationUsageService::trackingStatus() const
{
    if (!supported()) return tr("Application usage tracking is available on Windows.");
    if (!m_configurationLoaded)
        return m_configurationDiagnostic.isEmpty()
            ? tr("Waiting for local storage.") : m_configurationDiagnostic;
    if (!m_enabled) return tr("Tracking is paused.");
    if (!m_storageDiagnostic.isEmpty()) return m_storageDiagnostic;
    if (!m_configurationDiagnostic.isEmpty()) return m_configurationDiagnostic;
    if (m_systemSuspended) return tr("Tracking is paused while Windows is suspended.");
    if (m_systemLocked) return tr("Tracking is paused while the Windows session is locked.");
    if (!m_observationDiagnostic.isEmpty()) return m_observationDiagnostic;
    if (!m_trackingActive) return tr("Tracking is not active.");
    if (m_omittedGapCount > 0)
        return tr("%1 long observation gaps (%2 seconds) were omitted from tracked totals.")
            .arg(m_omittedGapCount).arg(m_omittedGapDurationMs / 1000);
    return tr("Automatically tracking eligible taskbar applications in the background.");
}

QVariantList ApplicationUsageService::exclusions() const
{
    QVariantList result;
    result.reserve(m_exclusions.size());
    for (const auto &entry : m_exclusions) result.push_back(exclusionMap(entry));
    return result;
}

QVariantList ApplicationUsageService::liveApplications() const
{
    QVariantList result;
    result.reserve(m_active.size());
    for (auto it = m_active.cbegin(); it != m_active.cend(); ++it) {
        result.push_back(QVariantMap{
            {QStringLiteral("applicationId"), it.key()},
            {QStringLiteral("name"), it->application.displayName},
            {QStringLiteral("executableName"), it->application.executableName},
            {QStringLiteral("executablePath"), it->application.executablePath},
            {QStringLiteral("state"), it->state},
            {QStringLiteral("sessionId"), it->sessionId},
            {QStringLiteral("startedUtc"), it->startedUtc},
            {QStringLiteral("durationMs"), it->durationMs}});
    }
    return result;
}

QString ApplicationUsageService::liveState(const QString &applicationId) const
{
    const auto it = m_active.constFind(applicationId);
    return it == m_active.cend() ? QString() : it->state;
}

bool ApplicationUsageService::submitStorage(PendingOperation operation,
                                             storage::StorageCommand command)
{
    if (!m_graph) return false;
    const auto context = m_graph->beginOperation(QStringLiteral("application-usage"));
    if (!context.isValid()) return false;
    m_operations.insert(context.id.value, std::move(operation));
    if (!m_graph->submitStorage(context, std::move(command))) {
        // Storage rejection can be reported synchronously by the graph. If it
        // was rejected before dispatch, discard the local routing entry.
        if (m_graph->operations().acceptsResult(context.id))
            m_operations.remove(context.id.value);
        return false;
    }
    return true;
}

void ApplicationUsageService::loadConfiguration()
{
    if (m_configurationLoaded || !m_graph) return;
    submitStorage({PendingOperation::Kind::LoadConfiguration, {}},
        [](storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
            storage::StorageReply reply;
            if (cancelled()) { reply.error = domain::AppError::cancelled(); return reply; }
            bool enabled = true;
            int retentionDays = 365;
            QVector<storage::ApplicationUsageExclusion> exclusions;
            QString diagnostic;
            const QString now = utcString(QDateTime::currentDateTimeUtc());
            if (!repositories.applicationUsage.recoverInterruptedSessions(now, &diagnostic)
                || !repositories.applicationUsage.loadConfiguration(
                    &enabled, &retentionDays, &exclusions, &diagnostic)
                || !repositories.applicationUsage.recordCheckpoint({}, retentionDays,
                                                                     &diagnostic)) {
                qCWarning(lcUsage) << "configuration_load_failed" << diagnostic;
                reply.error = domain::AppError(domain::ErrorCategory::Storage);
                return reply;
            }
            QVariantList serialized;
            for (const auto &exclusion : exclusions) serialized.push_back(exclusionMap(exclusion));
            reply.values.insert(QStringLiteral("enabled"), enabled);
            reply.values.insert(QStringLiteral("retentionDays"), retentionDays);
            reply.values.insert(QStringLiteral("exclusions"), serialized);
            return reply;
        });
}

void ApplicationUsageService::onProfileMaintenanceChanged(bool active)
{
    if (m_profileMaintenance == active) return;
    m_profileMaintenance = active;
    if (active) {
        m_checkpointTimer.stop();
        const QStringList activeIds = m_active.keys();
        const qint64 nowMono = m_monotonic.elapsed();
        const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
        for (const QString &id : activeIds)
            finishSession(id, QStringLiteral("profile-maintenance"), nowMono, nowUtc);
        setTrackingActive(false);
        emit liveApplicationsChanged();
        return;
    }

    if (!m_graph || m_graph->lifecycle().state()
            != application::ApplicationLifecycle::State::Running) return;
    if (m_configurationDiagnostic == tr("Usage settings cannot change during profile maintenance.")
        || m_configurationDiagnostic == tr("Usage history cannot change during profile maintenance."))
        m_configurationDiagnostic.clear();
    if (!m_configurationLoaded) {
        loadConfiguration();
        return;
    }
    checkpoint();
    if (m_enabled && supported() && !m_storageBackpressurePaused) setTrackingActive(true);
    emit stateChanged();
}

void ApplicationUsageService::setTrackingEnabled(bool enabled)
{
    if (m_profileMaintenance) {
        m_configurationDiagnostic = tr("Usage settings cannot change during profile maintenance.");
        emit stateChanged();
        return;
    }
    if (!m_configurationLoaded || m_enabled == enabled) return;
    m_enabled = enabled;
    if (enabled) {
        if (m_platform && m_platform->supported()) {
            setTrackingActive(true);
            m_checkpointTimer.start();
        }
    } else {
        const qint64 nowMono = m_monotonic.elapsed();
        const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
        const QStringList activeIds = m_active.keys();
        for (const QString &id : activeIds)
            finishSession(id, QStringLiteral("paused"), nowMono, nowUtc);
        setTrackingActive(false);
        checkpoint();
    }

    submitStorage({PendingOperation::Kind::SetEnabled, {}},
        [enabled](storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
            storage::StorageReply reply;
            if (!cancelled() && !repositories.applicationUsage.setEnabled(enabled))
                reply.error = domain::AppError(domain::ErrorCategory::Storage);
            else if (cancelled()) reply.error = domain::AppError::cancelled();
            return reply;
        });
    emit stateChanged();
}

void ApplicationUsageService::setRetentionDays(int days)
{
    if (m_profileMaintenance) {
        m_configurationDiagnostic = tr("Usage settings cannot change during profile maintenance.");
        emit stateChanged();
        return;
    }
    if (!m_configurationLoaded || days < 0 || days > 3650 || m_retentionDays == days) return;
    m_retentionDays = days;
    submitStorage({PendingOperation::Kind::SetRetention, {}},
        [days](storage::StorageRepositories &repositories, const std::function<bool()> &cancelled) {
            storage::StorageReply reply;
            if (cancelled()) reply.error = domain::AppError::cancelled();
            else if (!repositories.applicationUsage.setRetentionDays(days))
                reply.error = domain::AppError(domain::ErrorCategory::Storage);
            return reply;
        });
    emit stateChanged();
}

void ApplicationUsageService::addApplicationExclusion(const QString &applicationId,
                                                       const QString &canonicalPath,
                                                       const QString &executableName)
{
    if (m_profileMaintenance) {
        m_configurationDiagnostic = tr("Usage settings cannot change during profile maintenance.");
        emit stateChanged();
        return;
    }
    if (!m_configurationLoaded || applicationId.trimmed().isEmpty()) return;
    const QString path = canonicalPath.trimmed();
    const QString name = executableName.trimmed();
    if (path.isEmpty() && name.isEmpty()) {
        m_configurationDiagnostic = tr("This application has no stable path or executable name, so its exclusion cannot be saved.");
        emit stateChanged();
        return;
    }
    const bool matchByName = path.isEmpty();
    const QString stableId = matchByName
        ? QStringLiteral("filename:%1").arg(name.toCaseFolded()) : applicationId;
    storage::ApplicationUsageExclusion exclusion{
        stableId, path, name, matchByName,
        utcString(QDateTime::currentDateTimeUtc())};
    persistExclusion(exclusion);
}

void ApplicationUsageService::addFilenameExclusion(const QString &executableName)
{
    if (m_profileMaintenance) {
        m_configurationDiagnostic = tr("Usage settings cannot change during profile maintenance.");
        emit stateChanged();
        return;
    }
    if (!m_configurationLoaded) return;
    const QString name = executableName.trimmed();
    if (name.isEmpty()) return;
    const QString id = QStringLiteral("filename:%1").arg(name.toCaseFolded());
    storage::ApplicationUsageExclusion exclusion{
        id, {}, name, true, utcString(QDateTime::currentDateTimeUtc())};
    persistExclusion(exclusion);
}

void ApplicationUsageService::removeExclusion(const QString &applicationId)
{
    if (m_profileMaintenance) {
        m_configurationDiagnostic = tr("Usage settings cannot change during profile maintenance.");
        emit stateChanged();
        return;
    }
    if (std::none_of(m_exclusions.cbegin(), m_exclusions.cend(),
            [&applicationId](const auto &item) { return item.applicationId == applicationId; })) return;
    const bool accepted = submitStorage({PendingOperation::Kind::RemoveExclusion, applicationId},
        [applicationId](storage::StorageRepositories &repositories,
                        const std::function<bool()> &cancelled) {
            storage::StorageReply reply;
            if (cancelled()) reply.error = domain::AppError::cancelled();
            else if (!repositories.applicationUsage.removeExclusion(applicationId))
                reply.error = domain::AppError(domain::ErrorCategory::Storage);
            return reply;
        });
    if (!accepted) {
        m_storageDiagnostic = tr("The application exclusion could not be queued for removal.");
    }
    emit stateChanged();
}

void ApplicationUsageService::deleteHistory(const QString &applicationId)
{
    if (m_profileMaintenance) {
        m_configurationDiagnostic = tr("Usage history cannot change during profile maintenance.");
        emit stateChanged();
        return;
    }
    if (!m_configurationLoaded) return;
    if (applicationId.isEmpty()) {
        const QStringList activeIds = m_active.keys();
        for (const QString &id : activeIds)
            finishSession(id, QStringLiteral("history-deleted"), m_monotonic.elapsed(),
                          QDateTime::currentDateTimeUtc());
    } else if (m_active.contains(applicationId)) {
        finishSession(applicationId, QStringLiteral("history-deleted"), m_monotonic.elapsed(),
                      QDateTime::currentDateTimeUtc());
    }
    m_deferredHistoryDeletes.push_back(applicationId);
    checkpoint();
    processDeferredHistoryDeletes();
}

void ApplicationUsageService::submitDeleteHistory(const QString &applicationId)
{
    m_deleteInFlight = true;
    submitStorage({PendingOperation::Kind::DeleteHistory, applicationId},
        [applicationId](storage::StorageRepositories &repositories,
                        const std::function<bool()> &cancelled) {
            storage::StorageReply reply;
            if (cancelled()) reply.error = domain::AppError::cancelled();
            else if (!repositories.applicationUsage.deleteHistory(applicationId))
                reply.error = domain::AppError(domain::ErrorCategory::Storage);
            return reply;
        });
}

void ApplicationUsageService::prepareShutdown()
{
    if (m_shutdownPreparing) return;
    m_shutdownPreparing = true;
    m_checkpointTimer.stop();
    if (m_platform) m_platform->stop();
    const QStringList activeIds = m_active.keys();
    const qint64 nowMono = m_monotonic.elapsed();
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    for (const QString &id : activeIds)
        finishSession(id, QStringLiteral("application-exit"), nowMono, nowUtc);
    setTrackingActive(false);
    m_checkpointRequested = false;
    submitCheckpointBatch(m_checkpointsInFlight > 0);
    emit liveApplicationsChanged();
    if (m_checkpointsInFlight == 0) {
        const bool hasPending = !m_pendingApplications.isEmpty() || !m_pendingSessions.isEmpty()
            || !m_pendingSegments.isEmpty();
        completeShutdownFlush(!hasPending && !m_shutdownCheckpointFailed,
            hasPending || m_shutdownCheckpointFailed
                ? tr("The final application usage checkpoint could not be confirmed.") : QString());
    }
}

void ApplicationUsageService::persistExclusion(
    const storage::ApplicationUsageExclusion &exclusion)
{
    PendingOperation operation{PendingOperation::Kind::AddExclusion, exclusion.applicationId};
    operation.exclusion = exclusion;
    const bool accepted = submitStorage(std::move(operation),
        [exclusion](storage::StorageRepositories &repositories,
                    const std::function<bool()> &cancelled) {
            storage::StorageReply reply;
            if (cancelled()) reply.error = domain::AppError::cancelled();
            else if (!repositories.applicationUsage.addExclusion(exclusion))
                reply.error = domain::AppError(domain::ErrorCategory::Storage);
            return reply;
        });
    if (!accepted) {
        m_configurationDiagnostic = tr("The application exclusion could not be queued for saving.");
        emit stateChanged();
    }
}

void ApplicationUsageService::onStorageFinished(quint64 operationId,
                                                 QVariantMap values,
                                                 domain::AppError error)
{
    auto it = m_operations.find(operationId);
    if (it == m_operations.end()) return;
    const PendingOperation operation = std::move(it.value());
    m_operations.erase(it);

    if (operation.kind == PendingOperation::Kind::Checkpoint) {
        m_checkpointsInFlight = std::max(0, m_checkpointsInFlight - 1);
        if (error.isError()) {
            restoreCheckpoint(operation.checkpoint);
            m_storageDiagnostic = m_shutdownPreparing
                ? tr("The final usage checkpoint could not be saved.")
                : tr("A usage checkpoint could not be saved. It will be retried.");
            if (m_shutdownPreparing) m_shutdownCheckpointFailed = true;
            else m_checkpointTimer.start();
            if (m_storageBackpressurePaused)
                m_storageDiagnostic = tr("Usage storage is unavailable; tracking is paused while accepted history remains buffered.");
            qCWarning(lcUsage) << "checkpoint_failed" << operation.applicationId;
        } else {
            m_storageDiagnostic.clear();
            emit statisticsChanged();
            if (m_storageBackpressurePaused && !m_shutdownPreparing
                && pendingCheckpointRecordCount() == 0) {
                m_storageBackpressurePaused = false;
                if (m_enabled && m_configurationLoaded && supported()) setTrackingActive(true);
            }
        }
        if (m_shutdownPreparing && m_checkpointsInFlight == 0) {
            const bool hasPending = !m_pendingApplications.isEmpty() || !m_pendingSessions.isEmpty()
                || !m_pendingSegments.isEmpty();
            completeShutdownFlush(!hasPending && !m_shutdownCheckpointFailed,
                hasPending || m_shutdownCheckpointFailed
                    ? tr("The final application usage checkpoint could not be confirmed.") : QString());
        } else if (m_checkpointRequested && m_checkpointsInFlight == 0) {
            m_checkpointRequested = false;
            checkpoint();
        }
        // A failed checkpoint is retried by the timer. Trying again here can
        // recurse synchronously when the storage worker rejects a full queue.
        if (!error.isError()) processDeferredHistoryDeletes();
        emit stateChanged();
        return;
    }

    if (operation.kind == PendingOperation::Kind::AddExclusion) {
        if (!error.isError()) {
            m_storageDiagnostic.clear();
            m_configurationDiagnostic.clear();
            applyExclusion(operation.exclusion);
            checkpoint();
            emit liveApplicationsChanged();
        } else {
            m_storageDiagnostic = tr("The application exclusion could not be saved; tracking settings were left unchanged.");
        }
        emit stateChanged();
        return;
    }

    if (operation.kind == PendingOperation::Kind::RemoveExclusion) {
        if (!error.isError()) {
            m_storageDiagnostic.clear();
            m_configurationDiagnostic.clear();
            m_exclusions.erase(std::remove_if(m_exclusions.begin(), m_exclusions.end(),
                [&operation](const auto &item) {
                    return item.applicationId == operation.applicationId;
                }), m_exclusions.end());
        } else {
            m_storageDiagnostic = tr("The application exclusion could not be removed; the saved setting was left unchanged.");
        }
        emit stateChanged();
        return;
    }

    if (error.isError() && !error.isCancelled()) {
        m_storageDiagnostic = tr("Usage tracking could not save a local setting.");
        qCWarning(lcUsage) << "storage_operation_failed" << int(operation.kind);
    } else if (!error.isError()) {
        m_storageDiagnostic.clear();
    }

    if (operation.kind == PendingOperation::Kind::LoadConfiguration) {
        if (error.isError()) {
            m_configurationDiagnostic = tr("Tracking configuration could not be read from local storage.");
            m_configurationLoaded = false;
        } else {
            m_configurationDiagnostic.clear();
            m_enabled = values.value(QStringLiteral("enabled"), true).toBool();
            m_retentionDays = values.value(QStringLiteral("retentionDays"), 365).toInt();
            m_exclusions.clear();
            for (const QVariant &item : values.value(QStringLiteral("exclusions")).toList()) {
                const QVariantMap map = item.toMap();
                m_exclusions.push_back({map.value(QStringLiteral("applicationId")).toString(),
                    map.value(QStringLiteral("canonicalPath")).toString(),
                    map.value(QStringLiteral("executableName")).toString(),
                    map.value(QStringLiteral("matchByName")).toBool(),
                    map.value(QStringLiteral("createdAtUtc")).toString()});
            }
            m_configurationLoaded = true;
            if (m_enabled && supported()) {
                setTrackingActive(true);
                m_checkpointTimer.start();
            }
        }
    } else if (operation.kind == PendingOperation::Kind::DeleteHistory) {
        m_deleteInFlight = false;
        if (!error.isError()) emit statisticsChanged();
        processDeferredHistoryDeletes();
    }
    emit stateChanged();
}

void ApplicationUsageService::setTrackingActive(bool active)
{
    active = active && supported() && m_enabled && m_configurationLoaded && !m_profileMaintenance;
    if (m_trackingActive == active) return;
    m_trackingActive = active;
    if (m_platform) {
        if (active) m_platform->start();
        else m_platform->stop();
    }
    if (active) m_checkpointTimer.start();
    else if (m_active.isEmpty()) m_checkpointTimer.stop();
    emit stateChanged();
}

void ApplicationUsageService::onObservations(
    const QVector<platform::ApplicationUsageObservation> &applications,
    bool succeeded, const QString &diagnostic)
{
    if (!m_trackingActive || m_systemSuspended) return;
    const qint64 nowMono = m_monotonic.elapsed();
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    if (!succeeded) {
        if (diagnostic.contains(QStringLiteral("session is locked"), Qt::CaseInsensitive)) {
            const QStringList activeIds = m_active.keys();
            for (const QString &id : activeIds) {
                auto active = m_active.find(id);
                if (active != m_active.end()) {
                    active->lastMonotonicMs = nowMono;
                    active->lastWallUtc = nowUtc;
                    active->lastUtcOffsetSeconds = nowUtc.toLocalTime().offsetFromUtc();
                }
                finishSession(id, QStringLiteral("locked"), nowMono, nowUtc);
            }
            m_systemLocked = true;
            m_observationDiagnostic.clear();
            checkpoint();
            notifyLiveChanges();
            return;
        }
        for (auto it = m_active.begin(); it != m_active.end(); ++it) {
            const QString previousState = it->state;
            it->state = QStringLiteral("Unknown");
            advance(it.value(), QStringLiteral("Unknown"), nowMono, nowUtc);
            if (previousState != it->state) {
                emit applicationStateChanged(it.key(), it->sessionId,
                                             it->state, utcString(nowUtc));
            }
        }
        m_observationDiagnostic = tr("Window state is temporarily unavailable; the interval is marked unknown.");
        emit stateChanged();
        notifyLiveChanges();
        return;
    }

    if (m_systemLocked) {
        // Omit the final locked interval as well as every earlier locked poll.
        for (auto it = m_active.begin(); it != m_active.end(); ++it) {
            it->lastMonotonicMs = nowMono;
            it->lastWallUtc = nowUtc;
            it->lastUtcOffsetSeconds = nowUtc.toLocalTime().offsetFromUtc();
        }
        m_systemLocked = false;
    }

    QHash<QString, platform::ApplicationUsageObservation> observed;
    for (const auto &application : applications) {
        if (!isExcluded(application)) observed.insert(application.applicationId, application);
    }

    const QStringList previouslyActive = m_active.keys();
    for (const QString &applicationId : previouslyActive) {
        auto current = observed.find(applicationId);
        if (current == observed.end()) {
            const auto existing = m_active.constFind(applicationId);
            const QString reason = existing != m_active.cend() && isExcluded(existing->application)
                ? QStringLiteral("excluded") : QStringLiteral("closed");
            finishSession(applicationId, reason, nowMono, nowUtc);
            continue;
        }
        auto active = m_active.find(applicationId);
        if (active != m_active.end()) {
            const QString previousState = active->state;
            advance(active.value(), current->state, nowMono, nowUtc);
            active->application = *current;
            active->lastSeenUtc = utcString(nowUtc);
            if (previousState != active->state) {
                emit applicationStateChanged(applicationId, active->sessionId,
                                             active->state, utcString(nowUtc));
            }
        }
        observed.erase(current);
    }

    for (auto it = observed.cbegin(); it != observed.cend(); ++it) {
        ActiveSession active;
        active.application = it.value();
        active.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        active.startedUtc = utcString(nowUtc);
        active.lastSeenUtc = active.startedUtc;
        active.lastCheckpointUtc = active.startedUtc;
        active.state = isKnownState(it->state) ? it->state : QStringLiteral("Unknown");
        active.lastMonotonicMs = nowMono;
        active.lastWallUtc = nowUtc;
        active.lastUtcOffsetSeconds = nowUtc.toLocalTime().offsetFromUtc();
        m_active.insert(it.key(), active);
        m_pendingApplications.push_back({it.key(), it->executablePath, it->executableName,
                                         it->displayName, active.startedUtc, active.startedUtc});
        emit applicationSessionStarted(it.key(), active.sessionId, active.startedUtc);
        emit applicationStateChanged(it.key(), active.sessionId, active.state, active.startedUtc);
    }
    m_observationDiagnostic.clear();
    if (!m_active.isEmpty() && !m_checkpointTimer.isActive())
        m_checkpointTimer.start();
    notifyLiveChanges();
}

void ApplicationUsageService::onSystemSuspendStateChanged(bool suspended)
{
    if (!m_trackingActive || m_systemSuspended == suspended) return;
    m_systemSuspended = suspended;
    if (suspended) {
        const QStringList activeIds = m_active.keys();
        const qint64 nowMono = m_monotonic.elapsed();
        const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
        for (const QString &id : activeIds)
            finishSession(id, QStringLiteral("suspended"), nowMono, nowUtc);
        checkpoint();
    }
    emit stateChanged();
}

void ApplicationUsageService::advance(ActiveSession &session, const QString &nextState,
                                       qint64 monotonicNow, const QDateTime &wallNowUtc)
{
    const qint64 elapsed = monotonicNow - session.lastMonotonicMs;
    if (elapsed > 0 && elapsed <= 15000) {
        QString state = isKnownState(session.state) ? session.state : QStringLiteral("Unknown");
        const qint64 wallSpan = session.lastWallUtc.msecsTo(wallNowUtc);
        const int currentOffset = wallNowUtc.toLocalTime().offsetFromUtc();
        if (wallSpan <= 0 || std::llabs(wallSpan - elapsed) > 3000
            || currentOffset != session.lastUtcOffsetSeconds) state = QStringLiteral("Unknown");
        appendDuration(session, state, elapsed, session.lastWallUtc, wallNowUtc);
        session.durationMs += elapsed;
    } else if (elapsed > 15000) {
        ++m_omittedGapCount;
        m_omittedGapDurationMs += elapsed;
    }
    // Longer gaps are treated as observation gaps (typically suspend/resume or
    // a stalled event loop) and omitted rather than credited to a guessed state.
    session.lastMonotonicMs = monotonicNow;
    session.lastWallUtc = wallNowUtc;
    session.lastUtcOffsetSeconds = wallNowUtc.toLocalTime().offsetFromUtc();
    session.state = isKnownState(nextState) ? nextState : QStringLiteral("Unknown");
}

void ApplicationUsageService::appendDuration(ActiveSession &session, const QString &state,
                                               qint64 durationMs,
                                               const QDateTime &startUtc,
                                               const QDateTime &endUtc)
{
    if (durationMs <= 0) return;
    const qint64 wallSpan = startUtc.msecsTo(endUtc);
    if (wallSpan <= 0 || std::llabs(wallSpan - durationMs) > 3000) {
        const QDateTime estimatedStart = endUtc.addMSecs(-durationMs);
        const QDateTime local = endUtc.toLocalTime();
        storage::ApplicationUsageSegment segment;
        segment.sessionId = session.sessionId;
        segment.applicationId = session.application.applicationId;
        segment.state = QStringLiteral("Unknown");
        segment.startedUtc = utcString(estimatedStart);
        segment.endedUtc = utcString(endUtc);
        segment.localDay = local.date().toString(Qt::ISODate);
        segment.localHour = local.time().hour();
        segment.durationMs = durationMs;
        segment.segmentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_pendingSegments.push_back(std::move(segment));
        return;
    }

    qint64 allocated = 0;
    QDateTime cursor = startUtc;
    while (cursor < endUtc) {
        const QDateTime localCursor = cursor.toLocalTime();
        const QDateTime nextHour = localCursor.time().hour() == 23
            ? QDateTime(localCursor.date().addDays(1), QTime(0, 0), Qt::LocalTime)
            : QDateTime(localCursor.date(),
                        QTime(localCursor.time().hour() + 1, 0), Qt::LocalTime);
        QDateTime pieceEnd = endUtc;
        const QDateTime nextHourUtc = nextHour.toUTC();
        if (nextHourUtc > cursor && nextHourUtc < endUtc) pieceEnd = nextHourUtc;
        qint64 pieceDuration = pieceEnd == endUtc
            ? durationMs - allocated
            : qRound64(double(durationMs) * double(cursor.msecsTo(pieceEnd)) / double(wallSpan));
        pieceDuration = std::max<qint64>(0, pieceDuration);
        allocated += pieceDuration;
        if (pieceDuration > 0) {
            storage::ApplicationUsageSegment segment;
            segment.sessionId = session.sessionId;
            segment.applicationId = session.application.applicationId;
            segment.state = state;
            segment.startedUtc = utcString(cursor);
            segment.endedUtc = utcString(pieceEnd);
            segment.localDay = localCursor.date().toString(Qt::ISODate);
            segment.localHour = localCursor.time().hour();
            segment.durationMs = pieceDuration;
            segment.segmentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (!m_pendingSegments.isEmpty()) {
                auto &previous = m_pendingSegments.last();
                if (previous.sessionId == segment.sessionId && previous.state == segment.state
                    && previous.localDay == segment.localDay && previous.localHour == segment.localHour
                    && previous.endedUtc == segment.startedUtc) {
                    previous.endedUtc = segment.endedUtc;
                    previous.durationMs += segment.durationMs;
                } else {
                    m_pendingSegments.push_back(std::move(segment));
                }
            } else {
                m_pendingSegments.push_back(std::move(segment));
            }
        }
        if (pieceEnd <= cursor) break;
        cursor = pieceEnd;
    }
}

void ApplicationUsageService::finishSession(const QString &applicationId,
                                             const QString &reason,
                                             qint64 monotonicNow,
                                             const QDateTime &wallNowUtc)
{
    auto it = m_active.find(applicationId);
    if (it == m_active.end()) return;
    advance(it.value(), it->state, monotonicNow, wallNowUtc);
    storage::ApplicationUsageSession closed;
    closed.sessionId = it->sessionId;
    closed.applicationId = applicationId;
    closed.startedUtc = it->startedUtc;
    closed.endedUtc = utcString(wallNowUtc);
    closed.lastCheckpointUtc = closed.endedUtc;
    closed.endReason = reason;
    closed.durationMs = it->durationMs;
    closed.active = false;
    const QString sessionId = it->sessionId;
    const QString endedUtc = utcString(wallNowUtc);
    m_pendingSessions.push_back(closed);
    m_pendingApplications.push_back({applicationId, it->application.executablePath,
        it->application.executableName, it->application.displayName, it->startedUtc,
        it->lastSeenUtc});
    m_active.erase(it);
    emit applicationSessionEnded(applicationId, sessionId, endedUtc, reason);
    notifyLiveChanges();
}

bool ApplicationUsageService::isExcluded(
    const platform::ApplicationUsageObservation &application) const
{
    const QString path = normalizedPath(application.executablePath);
    for (const auto &exclusion : m_exclusions) {
        if (!exclusion.canonicalPath.isEmpty()
            && normalizedPath(exclusion.canonicalPath) == path) return true;
        if (exclusion.matchByName
            && exclusion.executableName.compare(application.executableName,
                                                  Qt::CaseInsensitive) == 0) return true;
        if (!exclusion.matchByName && exclusion.applicationId == application.applicationId)
            return true;
    }
    return false;
}

void ApplicationUsageService::checkpoint()
{
    if (!m_active.isEmpty()) m_checkpointTimer.start();
    if (m_checkpointsInFlight > 0) {
        m_checkpointRequested = true;
        return;
    }
    submitCheckpointBatch(false);
}

void ApplicationUsageService::submitCheckpointBatch(bool allowParallel)
{
    if (m_checkpointsInFlight > 0 && !allowParallel) {
        m_checkpointRequested = true;
        return;
    }
    storage::ApplicationUsageCheckpoint batch = takeCheckpoint();
    if (batch.applications.isEmpty() && batch.sessions.isEmpty() && batch.segments.isEmpty()) {
        if (m_active.isEmpty() && m_checkpointsInFlight == 0) m_checkpointTimer.stop();
        return;
    }
    PendingOperation pending{PendingOperation::Kind::Checkpoint, {}};
    pending.checkpoint = batch;
    const int retentionDays = m_retentionDays;
    const int checkpointsBeforeSubmit = m_checkpointsInFlight;
    ++m_checkpointsInFlight;
    const bool accepted = submitStorage(std::move(pending),
        [batch, retentionDays](storage::StorageRepositories &repositories,
                               const std::function<bool()> &cancelled) {
            storage::StorageReply reply;
            QString diagnostic;
            if (cancelled()) reply.error = domain::AppError::cancelled();
            else if (!repositories.applicationUsage.recordCheckpoint(batch, retentionDays,
                                                                       &diagnostic)) {
                qCWarning(lcUsage) << "checkpoint_transaction_failed" << diagnostic;
                reply.error = domain::AppError(domain::ErrorCategory::Storage);
            }
            return reply;
        });
    if (!accepted && m_checkpointsInFlight > checkpointsBeforeSubmit) {
        m_checkpointsInFlight = std::max(0, m_checkpointsInFlight - 1);
        restoreCheckpoint(std::move(batch));
        m_storageDiagnostic = tr("A usage checkpoint could not be queued.");
        if (m_shutdownPreparing) m_shutdownCheckpointFailed = true;
        else m_checkpointTimer.start();
        emit stateChanged();
    }
}

storage::ApplicationUsageCheckpoint ApplicationUsageService::takeCheckpoint()
{
    storage::ApplicationUsageCheckpoint batch;
    QHash<QString, storage::ApplicationUsageMetadata> metadata;
    for (const auto &record : std::as_const(m_pendingApplications)) {
        auto it = metadata.find(record.applicationId);
        if (it == metadata.end()) metadata.insert(record.applicationId, record);
        else {
            if (record.firstSeenUtc < it->firstSeenUtc) it->firstSeenUtc = record.firstSeenUtc;
            if (record.lastSeenUtc > it->lastSeenUtc) it->lastSeenUtc = record.lastSeenUtc;
            if (it->executablePath.isEmpty()) it->executablePath = record.executablePath;
            if (it->executableName.isEmpty()) it->executableName = record.executableName;
            if (it->displayName.isEmpty()) it->displayName = record.displayName;
        }
    }
    m_pendingApplications.clear();
    const QString checkpointUtc = utcString(QDateTime::currentDateTimeUtc());
    QHash<QString, storage::ApplicationUsageSession> sessions;
    for (const auto &session : std::as_const(m_pendingSessions))
        sessions.insert(session.sessionId, session);
    for (auto it = m_active.begin(); it != m_active.end(); ++it) {
        storage::ApplicationUsageMetadata record{it.key(), it->application.executablePath,
            it->application.executableName, it->application.displayName,
            it->startedUtc, it->lastSeenUtc};
        auto existing = metadata.find(it.key());
        if (existing == metadata.end()) metadata.insert(it.key(), record);
        else {
            if (record.firstSeenUtc < existing->firstSeenUtc) existing->firstSeenUtc = record.firstSeenUtc;
            if (record.lastSeenUtc > existing->lastSeenUtc) existing->lastSeenUtc = record.lastSeenUtc;
            if (existing->executablePath.isEmpty()) existing->executablePath = record.executablePath;
            if (existing->executableName.isEmpty()) existing->executableName = record.executableName;
            if (existing->displayName.isEmpty()) existing->displayName = record.displayName;
        }
        it->lastCheckpointUtc = checkpointUtc;
        sessions.insert(it->sessionId, {it->sessionId, it.key(), it->startedUtc, {}, checkpointUtc,
                                        {}, it->durationMs, true});
    }
    m_pendingSessions.clear();
    for (auto it = sessions.cbegin(); it != sessions.cend(); ++it)
        batch.sessions.push_back(it.value());
    batch.segments = std::move(m_pendingSegments);
    m_pendingSegments.clear();
    for (auto it = metadata.cbegin(); it != metadata.cend(); ++it)
        batch.applications.push_back(it.value());
    return batch;
}

void ApplicationUsageService::restoreCheckpoint(storage::ApplicationUsageCheckpoint checkpoint)
{
    m_pendingApplications += checkpoint.applications;
    m_pendingSessions += checkpoint.sessions;
    m_pendingSegments = checkpoint.segments + m_pendingSegments;
    if (pendingCheckpointRecordCount() >= maximumPendingCheckpointRecords)
        pauseForStorageBackpressure();
}

void ApplicationUsageService::notifyLiveChanges()
{
    emit liveApplicationsChanged();
    emit stateChanged();
}

void ApplicationUsageService::processDeferredHistoryDeletes()
{
    if (m_deleteInFlight || m_checkpointsInFlight > 0 || m_deferredHistoryDeletes.isEmpty()) return;
    if (!m_pendingApplications.isEmpty() || !m_pendingSessions.isEmpty()
        || !m_pendingSegments.isEmpty()) {
        checkpoint();
        return;
    }
    const QString applicationId = m_deferredHistoryDeletes.takeFirst();
    submitDeleteHistory(applicationId);
}

void ApplicationUsageService::applyExclusion(
    const storage::ApplicationUsageExclusion &exclusion)
{
    const auto existing = std::find_if(m_exclusions.begin(), m_exclusions.end(),
        [&exclusion](const auto &item) { return item.applicationId == exclusion.applicationId; });
    if (existing == m_exclusions.end()) m_exclusions.push_back(exclusion);
    else *existing = exclusion;

    const QStringList activeIds = m_active.keys();
    const qint64 nowMono = m_monotonic.elapsed();
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    for (const QString &activeId : activeIds) {
        const auto active = m_active.constFind(activeId);
        if (active != m_active.cend() && isExcluded(active->application))
            finishSession(activeId, QStringLiteral("excluded"), nowMono, nowUtc);
    }
}

void ApplicationUsageService::completeShutdownFlush(bool success, QString diagnostic)
{
    if (!m_shutdownPreparing || m_shutdownFlushReported) return;
    m_shutdownFlushReported = true;
    emit shutdownFlushFinished(success, std::move(diagnostic));
}

qsizetype ApplicationUsageService::pendingCheckpointRecordCount() const noexcept
{
    return m_pendingApplications.size() + m_pendingSessions.size() + m_pendingSegments.size();
}

void ApplicationUsageService::pauseForStorageBackpressure()
{
    if (m_storageBackpressurePaused) return;
    m_storageBackpressurePaused = true;
    m_storageDiagnostic = tr("Usage storage is unavailable; tracking is paused while accepted history remains buffered.");
    m_checkpointTimer.start();
    if (m_platform) m_platform->stop();
    m_trackingActive = false;
    const QStringList activeIds = m_active.keys();
    const qint64 nowMono = m_monotonic.elapsed();
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    for (const QString &id : activeIds)
        finishSession(id, QStringLiteral("storage-backpressure"), nowMono, nowUtc);
    qCWarning(lcUsage) << "tracking_paused_storage_backpressure"
                       << "pending_records=" << pendingCheckpointRecordCount();
    emit liveApplicationsChanged();
    emit stateChanged();
}

} // namespace something::services

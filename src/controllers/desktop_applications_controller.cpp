#include "desktop_applications_controller.h"

#include "../application/application_lifecycle.h"
#include "../domain/desktop_application_rules.h"
#include "../services/desktop_applications_service.h"

#include <QFileInfo>
#include <QBuffer>
#include <QImage>
#include <QIODevice>
#include <QThread>

#include <optional>
#include <utility>

namespace something::controllers {
namespace {

QString safeError(domain::AppError error, const QString &fallback)
{
    switch (error.category()) {
    case domain::ErrorCategory::Conflict: return QStringLiteral("That executable is already saved.");
    case domain::ErrorCategory::Validation: return fallback.isEmpty()
        ? QStringLiteral("Check the selected application and try again.") : fallback;
    case domain::ErrorCategory::NotFound: return QStringLiteral("The saved application could not be found.");
    case domain::ErrorCategory::CapabilityUnavailable:
        return QStringLiteral("Saved application launching is supported only on Windows.");
    case domain::ErrorCategory::Platform: return QStringLiteral("The application action could not be completed.");
    default: return error.userMessage().isEmpty() ? fallback : error.userMessage();
    }
}

QVariantMap stagedIconMap(const services::StagedDesktopAppIcon &icon)
{
    return {{QStringLiteral("filename"), icon.asset.filename},
            {QStringLiteral("mimeType"), icon.asset.mimeType},
            {QStringLiteral("byteSize"), icon.asset.byteSize},
            {QStringLiteral("stagingFilename"), icon.stagingFilename}};
}

services::StagedDesktopAppIcon stagedIconFromMap(const QVariantMap &map)
{
    return {{map.value(QStringLiteral("filename")).toString(),
             map.value(QStringLiteral("mimeType")).toString(),
             map.value(QStringLiteral("byteSize")).toLongLong()},
            map.value(QStringLiteral("stagingFilename")).toString()};
}

QString iconDataUrl(const QImage &image)
{
    if (image.isNull()) return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {};
    return QStringLiteral("data:image/png;base64,%1").arg(QString::fromLatin1(bytes.toBase64()));
}

} // namespace

DesktopApplicationsController::DesktopApplicationsController(
    application::ApplicationServiceGraph *graph,
    platform::DesktopApplicationsPlatform *platform,
    providers::ApplicationIconImageProvider *images,
    QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_platform(platform)
    , m_images(images)
    , m_applications(this)
{
    Q_ASSERT(m_graph && m_platform);
    Q_ASSERT(QThread::currentThread() == thread());
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &DesktopApplicationsController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &DesktopApplicationsController::onValueFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &DesktopApplicationsController::onLifecycleChanged);
    onLifecycleChanged(m_graph->lifecycle().state());
}

application::OperationContext DesktopApplicationsController::begin(
    Kind kind, QString purpose, qint64 applicationId, bool mutation, QString fallback)
{
    const auto operation = m_graph->beginOperation(std::move(purpose));
    if (!operation.isValid()) return {};
    Pending pending;
    pending.kind = kind;
    pending.operation = operation;
    pending.applicationId = applicationId;
    pending.mutation = mutation;
    pending.failureFallback = std::move(fallback);
    m_pending.insert(operation.id.value, std::move(pending));
    if (mutation) m_busy = true;
    emit stateChanged();
    return operation;
}

bool DesktopApplicationsController::submitStorage(quint64 id, storage::StorageCommand command)
{
    const auto it = m_pending.constFind(id);
    if (it == m_pending.cend()) return false;
    const auto operation = it->operation;
    const bool accepted = m_graph->submitStorage(operation, std::move(command));
    if (!accepted) fail(id, domain::AppError(domain::ErrorCategory::Busy));
    return accepted;
}

bool DesktopApplicationsController::submitValue(quint64 id,
                                               services::BoundedWorkerPool::ValueWork work)
{
    const auto it = m_pending.constFind(id);
    if (it == m_pending.cend()) return false;
    const auto operation = it->operation;
    const bool accepted = m_graph->submitExpensiveValue(operation, std::move(work));
    if (!accepted) fail(id, domain::AppError(domain::ErrorCategory::Busy));
    return accepted;
}

quint64 DesktopApplicationsController::continuePending(quint64 previousId)
{
    const auto it = m_pending.constFind(previousId);
    if (it == m_pending.cend()) return 0;
    Pending pending = it.value();
    // The graph retires each operation before delivering its result. A new
    // storage/worker stage must own a fresh context and cancellation token.
    const auto operation = m_graph->beginOperation(pending.operation.purpose);
    if (!operation.isValid()) {
        fail(previousId, domain::AppError(domain::ErrorCategory::Busy));
        return 0;
    }
    pending.operation = operation;
    m_pending.remove(previousId);
    m_pending.insert(operation.id.value, std::move(pending));
    if (m_listOperation == previousId) m_listOperation = operation.id.value;
    return operation.id.value;
}

void DesktopApplicationsController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    const bool ready = state == application::ApplicationLifecycle::State::Running;
    if (m_ready != ready) m_ready = ready;
    if (state == application::ApplicationLifecycle::State::Failed) {
        setError(m_graph->lifecycle().startupError().userMessage());
    } else if (!ready) {
        cancelAll();
    } else {
        if (!m_stagingCleanupDone) {
            // Running is entered only after acquiring the profile lock.
            services::DesktopAppIconStore::removeAbandonedStagingFiles(m_graph->profileDirectory());
            m_stagingCleanupDone = true;
        }
        if (m_pageVisible && m_applications.rowCount() == 0) reload();
    }
    emit stateChanged();
}

void DesktopApplicationsController::setPageVisible(bool visible)
{
    if (m_pageVisible == visible) return;
    m_pageVisible = visible;
    if (!visible) {
        if (m_listOperation) {
            const quint64 id = m_listOperation;
            m_listOperation = 0;
            auto it = m_pending.find(id);
            if (it != m_pending.end()) {
                m_pending.erase(it);
                m_graph->cancelOperation({id});
            }
            m_loading = false;
        }
    } else if (m_ready) {
        reload();
    }
    emit stateChanged();
}

void DesktopApplicationsController::reload()
{
    if (!m_ready || !m_pageVisible) return;
    if (m_listOperation) {
        const quint64 old = m_listOperation;
        m_listOperation = 0;
        m_pending.remove(old);
        m_graph->cancelOperation({old});
    }
    const auto operation = begin(Kind::List, QStringLiteral("list saved applications"));
    if (!operation.isValid()) return;
    m_listOperation = operation.id.value;
    m_loading = true;
    m_errorMessage.clear();
    emit stateChanged();
    submitStorage(operation.id.value, services::DesktopApplicationsService::list());
}

void DesktopApplicationsController::addFromFile(const QUrl &selectedFile)
{
    if (!selectedFile.isLocalFile() || selectedFile.toLocalFile().isEmpty()) {
        setError(QStringLiteral("Choose a local application file."));
        return;
    }
    addFromPath(selectedFile.toLocalFile());
}

void DesktopApplicationsController::relocateFromFile(qint64 id, const QUrl &selectedFile)
{
    if (!selectedFile.isLocalFile() || selectedFile.toLocalFile().isEmpty()) {
        setError(QStringLiteral("Choose a local application file."));
        return;
    }
    relocateApplication(id, selectedFile.toLocalFile());
}

void DesktopApplicationsController::importIconFile(qint64 id, const QUrl &selectedFile)
{
    if (!selectedFile.isLocalFile() || selectedFile.toLocalFile().isEmpty()) {
        setError(QStringLiteral("Choose a local icon file."));
        return;
    }
    importIcon(id, selectedFile.toLocalFile());
}

void DesktopApplicationsController::addFromPath(QString selectedPath)
{
    if (m_busy || !m_ready) return;
    if (!supported()) {
        setError(QStringLiteral("Saved application launching is supported only on Windows."));
        return;
    }
    const auto operation = begin(Kind::AddPrepare, QStringLiteral("prepare saved application"),
        0, true, QStringLiteral("Choose an existing regular Windows .exe file."));
    if (!operation.isValid()) return;
    auto *native = m_platform;
    submitValue(operation.id.value,
        [selectedPath = std::move(selectedPath), native](
            const application::CancellationToken &cancellation) {
            services::BoundedWorkerPool::ValueResult result;
            QString canonical;
            result.error = native->resolveExecutable(selectedPath, &canonical);
            if (result.error.isError()) return result;
            if (cancellation.isCancellationRequested()) {
                result.error = domain::AppError::cancelled();
                return result;
            }
            const QString name = domain::defaultDesktopApplicationName(canonical);
            const auto normalized = domain::normalizeDesktopApplicationName(name);
            if (!normalized) {
                result.error = domain::AppError(domain::ErrorCategory::Validation);
                return result;
            }
            result.values.insert(QStringLiteral("path"), canonical);
            result.values.insert(QStringLiteral("name"), *normalized);
            return result;
        });
}

void DesktopApplicationsController::renameApplication(qint64 id, QString name)
{
    if (m_busy || !m_ready) return;
    const auto operation = begin(Kind::Rename, QStringLiteral("rename saved application"), id,
        true, QStringLiteral("Application name must contain 1 to 120 characters."));
    if (!operation.isValid()) return;
    submitStorage(operation.id.value, services::DesktopApplicationsService::rename(id, std::move(name)));
}

void DesktopApplicationsController::relocateApplication(qint64 id, QString selectedPath)
{
    if (m_busy || !m_ready) return;
    if (!supported()) {
        setError(QStringLiteral("Saved application launching is supported only on Windows."));
        return;
    }
    const auto operation = begin(Kind::RelocatePrepare, QStringLiteral("validate application location"),
        id, true, QStringLiteral("Choose an existing regular Windows .exe file."));
    if (!operation.isValid()) return;
    m_pending[operation.id.value].selectedPath = selectedPath;
    auto *native = m_platform;
    submitValue(operation.id.value,
        [selectedPath = std::move(selectedPath), native](const application::CancellationToken &cancellation) {
            services::BoundedWorkerPool::ValueResult result;
            if (cancellation.isCancellationRequested()) {
                result.error = domain::AppError::cancelled();
                return result;
            }
            QString canonical;
            result.error = native->resolveExecutable(selectedPath, &canonical);
            if (!result.error.isError()) result.values.insert(QStringLiteral("path"), canonical);
            return result;
        });
}

void DesktopApplicationsController::launchApplication(qint64 id)
{
    if (m_busy || !m_ready) return;
    if (!supported()) {
        setError(QStringLiteral("Saved application launching is supported only on Windows."));
        return;
    }
    const auto operation = begin(Kind::LaunchGet, QStringLiteral("launch saved application"), id,
        true, QStringLiteral("This executable is unavailable. Locate its current .exe file."));
    if (operation.isValid()) submitStorage(operation.id.value, services::DesktopApplicationsService::get(id));
}

void DesktopApplicationsController::importIcon(qint64 id, QString selectedPath)
{
    if (m_busy || !m_ready) return;
    const auto operation = begin(Kind::IconGet, QStringLiteral("load application icon target"), id,
        true, QStringLiteral("Choose a valid ICO, JPEG, PNG or WebP image no larger than 5 MiB."));
    if (!operation.isValid()) return;
    m_pending[operation.id.value].selectedPath = std::move(selectedPath);
    submitStorage(operation.id.value, services::DesktopApplicationsService::get(id));
}

void DesktopApplicationsController::resetIcon(qint64 id)
{
    if (m_busy || !m_ready) return;
    const auto operation = begin(Kind::IconRemoveGet, QStringLiteral("reset application icon"), id,
        true, QStringLiteral("The application icon could not be reset."));
    if (operation.isValid()) submitStorage(operation.id.value, services::DesktopApplicationsService::get(id));
}

void DesktopApplicationsController::removeApplication(qint64 id)
{
    if (m_busy || !m_ready) return;
    const auto operation = begin(Kind::DeleteGet, QStringLiteral("remove saved application"), id,
        true, QStringLiteral("The saved application could not be removed."));
    if (operation.isValid()) submitStorage(operation.id.value, services::DesktopApplicationsService::get(id));
}

void DesktopApplicationsController::clearMessages()
{
    if (m_errorMessage.isEmpty() && m_notification.isEmpty()) return;
    m_errorMessage.clear();
    m_notification.clear();
    emit stateChanged();
}

void DesktopApplicationsController::onStorageFinished(quint64 id, const QVariantMap &values,
                                                        domain::AppError error)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    if (error.isError() || error.isCancelled()) {
        fail(id, error);
        return;
    }
    switch (it->kind) {
    case Kind::List:
    case Kind::LaunchGet:
    case Kind::IconGet:
    case Kind::IconRemoveGet:
    case Kind::DeleteGet:
        id = continuePending(id);
        if (!id) return;
        it = m_pending.find(id);
        break;
    default:
        break;
    }
    const QVariantMap record = values.value(QStringLiteral("application")).toMap();
    switch (it->kind) {
    case Kind::List: {
        it->kind = Kind::ListAvailability;
        const QVariantList rows = values.value(QStringLiteral("applications")).toList();
        auto *native = m_platform;
        submitValue(id, [rows, native](const application::CancellationToken &cancellation) {
            services::BoundedWorkerPool::ValueResult result;
            QVariantList updated;
            updated.reserve(rows.size());
            for (const QVariant &item : rows) {
                if (cancellation.isCancellationRequested()) {
                    result.error = domain::AppError::cancelled();
                    return result;
                }
                QVariantMap row = item.toMap();
                QString resolved;
                const bool available = native->resolveExecutable(
                    row.value(QStringLiteral("executablePath")).toString(), &resolved).category()
                        == domain::ErrorCategory::None;
                row.insert(QStringLiteral("available"), available);
                row.insert(QStringLiteral("nativeIconDataUrl"), available
                    ? iconDataUrl(native->extractExecutableIcon(resolved, cancellation)) : QString());
                updated.push_back(row);
            }
            result.values.insert(QStringLiteral("applications"), updated);
            return result;
        });
        return;
    }
    case Kind::AddCommit:
    case Kind::Rename:
    case Kind::RelocateCommit:
    case Kind::IconCommit:
    case Kind::IconRemoveCommit:
        updateFromRecord(record);
        if (it->kind == Kind::IconCommit && !it->oldIconFilename.isEmpty()) {
            if (m_images) m_images->invalidate(it->oldIconFilename);
            services::DesktopAppIconStore::remove(m_graph->profileDirectory(), it->oldIconFilename);
        }
        if (it->kind == Kind::IconRemoveCommit && it->stagedDeletion.moved) {
            services::DesktopAppIconStore::finishDeletion(m_graph->profileDirectory(), it->stagedDeletion);
            if (m_images) m_images->invalidate(it->oldIconFilename);
        }
        succeed(id);
        return;
    case Kind::LaunchGet:
        startLaunch(id, record);
        return;
    case Kind::IconGet:
        startIconImport(id, record);
        return;
    case Kind::IconRemoveGet:
        stageIconDeletion(id, record, false);
        return;
    case Kind::DeleteGet:
        stageIconDeletion(id, record, true);
        return;
    case Kind::DeleteCommit:
        if (it->stagedDeletion.moved) {
            services::DesktopAppIconStore::finishDeletion(m_graph->profileDirectory(), it->stagedDeletion);
            if (m_images) m_images->invalidate(it->oldIconFilename);
        }
        m_applications.removeById(it->applicationId);
        succeed(id, QStringLiteral("Saved entry removed. The executable remains on your computer."));
        return;
    default:
        fail(id, domain::AppError(domain::ErrorCategory::Internal));
        return;
    }
}

void DesktopApplicationsController::onValueFinished(quint64 id, const QVariantMap &values,
                                                     domain::AppError error)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    if (error.isError() || error.isCancelled()) {
        fail(id, error);
        return;
    }
    if (it->kind == Kind::IconPrepare)
        it->stagedIcon = stagedIconFromMap(values.value(QStringLiteral("icon")).toMap());
    if (it->kind == Kind::AddPrepare || it->kind == Kind::RelocatePrepare
        || it->kind == Kind::IconPrepare) {
        id = continuePending(id);
        if (!id) return;
        it = m_pending.find(id);
    }
    switch (it->kind) {
    case Kind::ListAvailability:
        m_applications.replace(values.value(QStringLiteral("applications")).toList());
        m_errorMessage.clear();
        m_loading = false;
        m_listOperation = 0;
        succeed(id);
        return;
    case Kind::AddPrepare: {
        it->selectedPath = values.value(QStringLiteral("path")).toString();
        it->kind = Kind::AddCommit;
        submitStorage(id, services::DesktopApplicationsService::add(
            values.value(QStringLiteral("name")).toString(), it->selectedPath));
        return;
    }
    case Kind::RelocatePrepare:
        it->kind = Kind::RelocateCommit;
        submitStorage(id, services::DesktopApplicationsService::relocate(
            it->applicationId, values.value(QStringLiteral("path")).toString()));
        return;
    case Kind::LaunchRun:
        succeed(id, QStringLiteral("Application opened."));
        reload();
        return;
    case Kind::IconPrepare: {
        if (!services::DesktopAppIconStore::install(m_graph->profileDirectory(), it->stagedIcon)) {
            fail(id, domain::AppError(domain::ErrorCategory::Storage));
            return;
        }
        it->installedIcon = true;
        it->kind = Kind::IconCommit;
        submitStorage(id, services::DesktopApplicationsService::setIcon(
            it->applicationId, it->stagedIcon.asset));
        return;
    }
    default:
        fail(id, domain::AppError(domain::ErrorCategory::Internal));
        return;
    }
}

void DesktopApplicationsController::startLaunch(quint64 id, QVariantMap record)
{
    auto *native = m_platform;
    auto it = m_pending.find(id);
    if (it == m_pending.cend()) return;
    it->kind = Kind::LaunchRun;
    submitValue(id, [record = std::move(record), native](const application::CancellationToken &cancellation) {
        services::BoundedWorkerPool::ValueResult result;
        result.error = services::DesktopApplicationsService::launchSavedRecord(
            record, native, cancellation);
        return result;
    });
}

void DesktopApplicationsController::startIconImport(quint64 id, QVariantMap record)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    it->kind = Kind::IconPrepare;
    it->oldIconFilename = record.value(QStringLiteral("iconFilename")).toString();
    const QString path = it->selectedPath;
    const QString profile = m_graph->profileDirectory();
    submitValue(id, [path, profile](const application::CancellationToken &cancellation) {
        services::BoundedWorkerPool::ValueResult result;
        services::StagedDesktopAppIcon icon;
        result.error = services::DesktopAppIconStore::stageImport(path, profile, cancellation, &icon);
        if (!result.error.isError()) result.values.insert(QStringLiteral("icon"), stagedIconMap(icon));
        return result;
    });
}

void DesktopApplicationsController::stageIconDeletion(quint64 id, QVariantMap record,
                                                       bool deleteApplication)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    it->oldIconFilename = record.value(QStringLiteral("iconFilename")).toString();
    if (!it->oldIconFilename.isEmpty()
        && !services::DesktopAppIconStore::stageDeletion(
            m_graph->profileDirectory(), it->oldIconFilename, &it->stagedDeletion)) {
        fail(id, domain::AppError(domain::ErrorCategory::Storage));
        return;
    }
    it->kind = deleteApplication ? Kind::DeleteCommit : Kind::IconRemoveCommit;
    submitStorage(id, deleteApplication
        ? services::DesktopApplicationsService::remove(it->applicationId)
        : services::DesktopApplicationsService::removeIcon(it->applicationId));
}

void DesktopApplicationsController::updateFromRecord(const QVariantMap &record)
{
    const qint64 id = record.value(QStringLiteral("id")).toLongLong();
    if (id <= 0) return;
    QVariantMap row = record;
    const QVariantMap previous = m_applications.rowForId(id);
    const bool relocated = previous.value(QStringLiteral("executablePath"))
        != row.value(QStringLiteral("executablePath"));
    row.insert(QStringLiteral("available"), relocated ? true
        : previous.value(QStringLiteral("available"), false).toBool());
    if (!relocated && previous.contains(QStringLiteral("nativeIconDataUrl")))
        row.insert(QStringLiteral("nativeIconDataUrl"),
                   previous.value(QStringLiteral("nativeIconDataUrl")));
    m_applications.upsert(std::move(row));
}

void DesktopApplicationsController::fail(quint64 id, domain::AppError error, QString message)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    Pending pending = it.value();
    m_pending.erase(it);
    if (!pending.stagedIcon.stagingFilename.isEmpty() && !pending.installedIcon)
        services::DesktopAppIconStore::discard(m_graph->profileDirectory(), pending.stagedIcon);
    if (pending.installedIcon)
        services::DesktopAppIconStore::remove(m_graph->profileDirectory(), pending.stagedIcon.asset.filename);
    if (pending.stagedDeletion.moved)
        services::DesktopAppIconStore::restoreDeletion(m_graph->profileDirectory(), pending.stagedDeletion);
    if (m_listOperation == id) m_listOperation = 0;
    if (pending.kind == Kind::List || pending.kind == Kind::ListAvailability) m_loading = false;
    if (pending.mutation) {
        m_busy = false;
        emit mutationFinished(false);
    }
    if (!error.isCancelled()) setError(message.isEmpty()
        ? safeError(error, pending.failureFallback) : std::move(message));
    m_graph->completeOperation({id}, error);
    emit stateChanged();
}

void DesktopApplicationsController::succeed(quint64 id, QString notification)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const bool mutation = it->mutation;
    const Kind completedKind = it->kind;
    m_pending.erase(it);
    if (m_listOperation == id) m_listOperation = 0;
    if (mutation) {
        m_busy = false;
        m_errorMessage.clear();
        m_notification = std::move(notification);
        emit mutationFinished(true);
    }
    m_graph->completeOperation({id});
    emit stateChanged();
    if (mutation && completedKind != Kind::LaunchRun && m_pageVisible) reload();
}

void DesktopApplicationsController::cancelAll()
{
    const auto pending = m_pending;
    m_pending.clear();
    m_listOperation = 0;
    m_loading = false;
    m_busy = false;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        if (!it->stagedIcon.stagingFilename.isEmpty() && !it->installedIcon)
            services::DesktopAppIconStore::discard(m_graph->profileDirectory(), it->stagedIcon);
        // An icon may already be referenced by a repository command still draining on
        // the storage thread. Preserve an installed file across shutdown ambiguity;
        // an unreferenced orphan is recoverable, while a dangling DB reference is not.
        if (it->stagedDeletion.moved)
            services::DesktopAppIconStore::restoreDeletion(m_graph->profileDirectory(), it->stagedDeletion);
        m_graph->cancelOperation({it.key()});
    }
    emit stateChanged();
}

void DesktopApplicationsController::setError(QString message)
{
    m_errorMessage = std::move(message);
    m_notification.clear();
    emit stateChanged();
}

} // namespace something::controllers

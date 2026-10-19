#include "setups_controller.h"

#include "../application/application_lifecycle.h"
#include "../services/desktop_applications_service.h"

#include <QThread>

#include <utility>

namespace something::controllers {
namespace {

QString safeError(domain::AppError error)
{
    switch (error.category()) {
    case domain::ErrorCategory::Conflict:
        return QStringLiteral("A setup with that name already exists.");
    case domain::ErrorCategory::Validation:
        return QStringLiteral("Check the setup name, description, application order, and icon.");
    case domain::ErrorCategory::NotFound:
        return QStringLiteral("The setup or one of its saved applications could not be found.");
    case domain::ErrorCategory::CapabilityUnavailable:
        return QStringLiteral("Application launching is unavailable on this device.");
    case domain::ErrorCategory::Storage:
        return QStringLiteral("The setup could not be saved. Your existing setup is unchanged.");
    case domain::ErrorCategory::Busy:
        return QStringLiteral("Background work is busy. Try again shortly.");
    default:
        return QStringLiteral("The setup action could not be completed.");
    }
}

storage::Setup setupFromDraft(qint64 id, QString name, QString description,
                              const QVariantList &applicationIds)
{
    storage::Setup setup;
    setup.id = id;
    setup.name = std::move(name);
    setup.description = std::move(description);
    setup.applicationIds.reserve(applicationIds.size());
    for (const QVariant &value : applicationIds) setup.applicationIds.push_back(value.toLongLong());
    return setup;
}

services::StagedSetupIcon stagedIconFromMap(const QVariantMap &map)
{
    return {{map.value(QStringLiteral("filename")).toString(),
             map.value(QStringLiteral("mimeType")).toString(),
             map.value(QStringLiteral("byteSize")).toLongLong()},
            map.value(QStringLiteral("stagingFilename")).toString()};
}

QVariantMap stagedIconMap(const services::StagedSetupIcon &icon)
{
    return {{QStringLiteral("filename"), icon.asset.filename},
            {QStringLiteral("mimeType"), icon.asset.mimeType},
            {QStringLiteral("byteSize"), icon.asset.byteSize},
            {QStringLiteral("stagingFilename"), icon.stagingFilename}};
}

} // namespace

SetupsController::SetupsController(application::ApplicationServiceGraph *graph,
    platform::DesktopApplicationsPlatform *platform,
    providers::SetupIconImageProvider *images, QObject *parent)
    : QObject(parent), m_graph(graph), m_platform(platform), m_images(images), m_setups(this)
{
    Q_ASSERT(m_graph && m_platform);
    Q_ASSERT(QThread::currentThread() == thread());
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &SetupsController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &SetupsController::onValueFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &SetupsController::onLifecycleChanged);
    onLifecycleChanged(m_graph->lifecycle().state());
}

application::OperationContext SetupsController::begin(Kind kind, QString purpose, bool mutation)
{
    const auto operation = m_graph->beginOperation(std::move(purpose));
    if (!operation.isValid()) return {};
    Pending pending;
    pending.kind = kind;
    pending.operation = operation;
    pending.mutation = mutation;
    m_pending.insert(operation.id.value, std::move(pending));
    if (mutation) m_busy = true;
    emit stateChanged();
    return operation;
}

bool SetupsController::submitStorage(quint64 id, storage::StorageCommand command)
{
    const auto it = m_pending.constFind(id);
    return it != m_pending.cend() && m_graph->submitStorage(it->operation, std::move(command));
}

bool SetupsController::submitValue(quint64 id, services::BoundedWorkerPool::ValueWork work)
{
    const auto it = m_pending.constFind(id);
    return it != m_pending.cend() && m_graph->submitExpensiveValue(it->operation, std::move(work));
}

void SetupsController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    m_ready = state == application::ApplicationLifecycle::State::Running;
    if (m_ready) {
        if (!m_stagingCleanupDone) {
            services::SetupIconStore::removeAbandonedStagingFiles(m_graph->profileDirectory());
            m_stagingCleanupDone = true;
        }
        if (m_setups.count() == 0) reload();
    } else {
        cancelAll();
        if (state == application::ApplicationLifecycle::State::Failed)
            m_errorMessage = m_graph->lifecycle().startupError().userMessage();
    }
    emit stateChanged();
}

void SetupsController::setPageVisible(bool visible)
{
    if (m_pageVisible == visible) return;
    m_pageVisible = visible;
    if (visible && m_ready) reload();
}

void SetupsController::reload()
{
    if (!m_ready) return;
    if (m_listOperation) {
        const quint64 old = m_listOperation;
        m_listOperation = 0;
        m_pending.remove(old);
        m_graph->cancelOperation({old});
    }
    const auto operation = begin(Kind::List, QStringLiteral("list setups"));
    if (!operation.isValid()) return;
    m_listOperation = operation.id.value;
    m_loading = true;
    m_errorMessage.clear();
    m_notification.clear();
    emit stateChanged();
    if (!submitStorage(operation.id.value, services::SetupsService::list()))
        fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
}

void SetupsController::saveSetupFromFile(qint64 id, QString name, QString description,
    QVariantList applicationIds, const QUrl &iconFile, bool removeIcon)
{
    if (m_busy || !m_ready) return;
    // An empty URL preserves the existing icon (or removes it when requested).
    if (!iconFile.isEmpty() && (!iconFile.isLocalFile() || iconFile.toLocalFile().isEmpty())) {
        m_errorMessage = QStringLiteral("Choose a local icon file.");
        emit stateChanged();
        emit mutationFinished(false);
        return;
    }
    saveSetup(id, std::move(name), std::move(description), std::move(applicationIds),
              iconFile.toLocalFile(), removeIcon);
}

void SetupsController::saveSetup(qint64 id, QString name, QString description,
    QVariantList applicationIds, QString iconPath, bool removeIcon)
{
    if (m_busy || !m_ready) return;
    if (id < 0) {
        m_errorMessage = QStringLiteral("The selected setup is no longer available.");
        emit stateChanged();
        return;
    }
    Pending draft;
    draft.kind = Kind::Save;
    draft.mutation = true;
    draft.draft = setupFromDraft(id, std::move(name), std::move(description), applicationIds);
    draft.removeIcon = removeIcon;
    draft.iconPath = std::move(iconPath);
    draft.oldIconFilename = m_setups.rowForId(id).value(QStringLiteral("iconFilename")).toString();
    if (removeIcon && !draft.iconPath.isEmpty()) {
        m_errorMessage = QStringLiteral("Choose a new icon or remove the existing icon, not both.");
        emit stateChanged();
        emit mutationFinished(false);
        return;
    }

    if (!draft.iconPath.isEmpty()) {
        const auto operation = begin(Kind::IconPrepare, QStringLiteral("prepare setup icon"), true);
        if (!operation.isValid()) return;
        draft.operation = operation;
        const QString path = draft.iconPath;
        const QString profile = m_graph->profileDirectory();
        if (!submitValue(operation.id.value, [path, profile](const application::CancellationToken &cancellation) {
            services::BoundedWorkerPool::ValueResult result;
            services::StagedSetupIcon icon;
            result.error = services::SetupIconStore::stageImport(path, profile, cancellation, &icon);
            if (!result.error.isError() && !result.error.isCancelled())
                result.values.insert(QStringLiteral("icon"), stagedIconMap(icon));
            return result;
        })) fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
        auto it = m_pending.find(operation.id.value);
        if (it != m_pending.end()) {
            it->draft = std::move(draft.draft);
            it->removeIcon = draft.removeIcon;
            it->iconPath = std::move(draft.iconPath);
            it->oldIconFilename = std::move(draft.oldIconFilename);
        }
        return;
    }

    const auto operation = begin(Kind::Save, QStringLiteral("save setup"), true);
    if (!operation.isValid()) return;
    draft.operation = operation;
    m_pending[operation.id.value] = std::move(draft);
    const Pending pending = m_pending.value(operation.id.value);
    if (!submitStorage(operation.id.value, services::SetupsService::save(
        pending.draft, pending.removeIcon, m_graph->profileDirectory())))
        fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
}

void SetupsController::removeSetup(qint64 id)
{
    if (m_busy || !m_ready || id <= 0) return;
    const auto operation = begin(Kind::Remove, QStringLiteral("delete setup"), true);
    if (!operation.isValid()) return;
    if (!submitStorage(operation.id.value,
        services::SetupsService::remove(id, m_graph->profileDirectory())))
        fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
}

void SetupsController::startSetup(qint64 id)
{
    if (m_busy || !m_ready || id <= 0) return;
    const auto operation = begin(Kind::LaunchPrepare, QStringLiteral("prepare setup launch"), true);
    if (!operation.isValid()) return;
    m_pending[operation.id.value].userCancellation = application::CancellationSource();
    m_activeLaunchOperation = operation.id.value;
    m_busy = true;
    m_launching = true;
    m_launchSummary.clear();
    m_errorMessage.clear();
    emit stateChanged();
    if (!submitStorage(operation.id.value, services::SetupsService::launchData(id)))
        fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
}

void SetupsController::cancelLaunch()
{
    auto it = m_pending.find(m_activeLaunchOperation);
    if (it == m_pending.end()
        || (it->kind != Kind::LaunchPrepare && it->kind != Kind::LaunchRun)) return;
    it->userCancellation.requestCancellation();
    emit stateChanged();
}

void SetupsController::clearMessages()
{
    m_errorMessage.clear();
    m_notification.clear();
    emit stateChanged();
}

void SetupsController::onStorageFinished(quint64 id, const QVariantMap &values,
                                          domain::AppError error)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    if (error.isError() || error.isCancelled()) { fail(id, error); return; }
    switch (it->kind) {
    case Kind::List:
        m_setups.replace(values.value(QStringLiteral("setups")).toList(),
                         values.value(QStringLiteral("applications")).toList());
        m_applications = values.value(QStringLiteral("applications")).toList();
        m_loading = false;
        m_listOperation = 0;
        complete(id);
        return;
    case Kind::Save: {
        const QVariantMap row = values.value(QStringLiteral("setup")).toMap();
        const QString previous = it->oldIconFilename;
        const QString current = row.value(QStringLiteral("iconFilename")).toString();
        if (!previous.isEmpty() && previous != current && m_images) m_images->invalidate(previous);
        m_setups.upsert(row, m_applications);
        if (it->stagedIcon && m_images) m_images->invalidate(it->stagedIcon->asset.filename);
        m_errorMessage.clear();
        m_notification = QStringLiteral("Setup saved.");
        complete(id);
        emit mutationFinished(true);
        return;
    }
    case Kind::Remove: {
        const qint64 setupId = values.value(QStringLiteral("setupId")).toLongLong();
        const QVariantMap row = m_setups.rowForId(setupId);
        const QString icon = row.value(QStringLiteral("iconFilename")).toString();
        m_setups.removeById(setupId);
        if (!icon.isEmpty() && m_images) m_images->invalidate(icon);
        m_errorMessage.clear();
        m_notification = QStringLiteral("Setup deleted. Saved applications were kept.");
        complete(id);
        emit mutationFinished(true);
        return;
    }
    case Kind::LaunchPrepare:
        startLaunchWorker(id, it.value(), values.value(QStringLiteral("applications")).toList());
        return;
    default:
        fail(id, domain::AppError(domain::ErrorCategory::Internal));
        return;
    }
}

void SetupsController::onValueFinished(quint64 id, const QVariantMap &values,
                                        domain::AppError error)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    if (error.isError() || error.isCancelled()) { fail(id, error); return; }
    if (it->kind == Kind::IconPrepare) {
        Pending draft = it.value();
        draft.stagedIcon = stagedIconFromMap(values.value(QStringLiteral("icon")).toMap());
        m_pending.erase(it);
        commitDraft(id, std::move(draft));
        return;
    }
    if (it->kind == Kind::LaunchRun) {
        m_launchSummary = values.value(QStringLiteral("summary")).toMap();
        m_launching = false;
        m_activeLaunchOperation = 0;
        m_errorMessage.clear();
        const QString launched = QString::number(m_launchSummary.value(QStringLiteral("launched")).toInt());
        const QString attempted = QString::number(m_launchSummary.value(QStringLiteral("attempted")).toInt());
        m_notification = QStringLiteral("Started %1 of %2 applications.").arg(launched, attempted);
        complete(id);
        emit launchFinished(m_launchSummary);
        return;
    }
    fail(id, domain::AppError(domain::ErrorCategory::Internal));
}

void SetupsController::commitDraft(quint64 previousOperationId, Pending pending)
{
    Q_UNUSED(previousOperationId);
    if (!m_ready) {
        if (pending.stagedIcon) services::SetupIconStore::discard(
            m_graph->profileDirectory(), *pending.stagedIcon);
        return;
    }
    const auto operation = begin(Kind::Save, QStringLiteral("save setup"), true);
    if (!operation.isValid()) {
        if (pending.stagedIcon) services::SetupIconStore::discard(
            m_graph->profileDirectory(), *pending.stagedIcon);
        m_busy = false;
        return;
    }
    pending.operation = operation;
    pending.kind = Kind::Save;
    m_pending[operation.id.value] = std::move(pending);
    const Pending saved = m_pending.value(operation.id.value);
    if (!submitStorage(operation.id.value, services::SetupsService::save(
        saved.draft, saved.removeIcon, m_graph->profileDirectory(), saved.stagedIcon)))
        fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
}

void SetupsController::startLaunchWorker(quint64 previousOperationId, Pending pending,
                                           QVariantList orderedApplications)
{
    Q_UNUSED(previousOperationId);
    m_pending.remove(previousOperationId);
    const auto operation = begin(Kind::LaunchRun, QStringLiteral("launch setup members"), true);
    if (!operation.isValid()) {
        m_activeLaunchOperation = 0;
        m_launching = false;
        m_busy = false;
        emit stateChanged();
        return;
    }
    pending.operation = operation;
    pending.kind = Kind::LaunchRun;
    pending.mutation = true;
    m_pending[operation.id.value] = pending;
    m_activeLaunchOperation = operation.id.value;
    auto *native = m_platform;
    const auto userCancellation = pending.userCancellation;
    if (!submitValue(operation.id.value,
        [orderedApplications = std::move(orderedApplications), native, userCancellation]
        (const application::CancellationToken &cancellation) {
            services::BoundedWorkerPool::ValueResult result;
            const auto summary = services::SetupsService::launchMembers(
                orderedApplications, cancellation, userCancellation,
                [native](const QVariantMap &record, const application::CancellationToken &token,
                         const application::CancellationToken &userToken) {
                    return services::DesktopApplicationsService::launchSavedRecord(
                        record, native, token, &userToken);
                });
            result.values.insert(QStringLiteral("summary"), summary.toVariantMap());
            return result;
        })) fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
}

void SetupsController::fail(quint64 id, domain::AppError error)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    Pending pending = it.value();
    m_pending.erase(it);
    if (pending.stagedIcon) services::SetupIconStore::discard(
        m_graph->profileDirectory(), *pending.stagedIcon);
    if (m_listOperation == id) { m_listOperation = 0; m_loading = false; }
    if (m_activeLaunchOperation == id) { m_activeLaunchOperation = 0; m_launching = false; }
    if (pending.mutation) {
        m_busy = false;
        emit mutationFinished(false);
    }
    if (!error.isCancelled()) m_errorMessage = safeError(error);
    m_graph->completeOperation({id}, error);
    emit stateChanged();
}

void SetupsController::complete(quint64 id)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const bool mutation = it->mutation;
    m_pending.erase(it);
    if (m_listOperation == id) m_listOperation = 0;
    if (m_activeLaunchOperation == id) m_activeLaunchOperation = 0;
    if (mutation) m_busy = false;
    m_graph->completeOperation({id});
    emit stateChanged();
}

void SetupsController::cancelAll()
{
    const auto pending = m_pending;
    m_pending.clear();
    m_listOperation = 0;
    m_activeLaunchOperation = 0;
    m_loading = false;
    m_busy = false;
    m_launching = false;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        it->userCancellation.requestCancellation();
        if (it->stagedIcon) services::SetupIconStore::discard(
            m_graph->profileDirectory(), *it->stagedIcon);
        m_graph->cancelOperation({it.key()});
    }
    emit stateChanged();
}

} // namespace something::controllers

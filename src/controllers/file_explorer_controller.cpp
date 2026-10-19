#include "file_explorer_controller.h"

#include <QThread>

#include <utility>

namespace something::controllers {
namespace {

QString errorText(const QString &code)
{
    if (code == QStringLiteral("invalid-path") || code == QStringLiteral("invalid-query")
        || code == QStringLiteral("invalid-page"))
        return QStringLiteral("The location or search text is not valid.");
    if (code == QStringLiteral("invalid-root"))
        return QStringLiteral("Choose a folder again. This session location is no longer available.");
    if (code == QStringLiteral("unavailable-root"))
        return QStringLiteral("This drive or folder is currently unavailable.");
    if (code == QStringLiteral("permission-denied"))
        return QStringLiteral("Windows denied access to this folder or file.");
    if (code == QStringLiteral("not-directory"))
        return QStringLiteral("This location is no longer a folder.");
    if (code == QStringLiteral("not-found"))
        return QStringLiteral("This item is no longer available.");
    if (code == QStringLiteral("not-file"))
        return QStringLiteral("Only regular files can be opened or moved to the Recycle Bin.");
    if (code == QStringLiteral("unsafe-link") || code == QStringLiteral("outside-root"))
        return QStringLiteral("This link leaves the approved folder and cannot be opened.");
    if (code == QStringLiteral("unsupported"))
        return QStringLiteral("Native file actions are unavailable on this platform.");
    if (code == QStringLiteral("open-failed"))
        return QStringLiteral("Windows could not open this file.");
    if (code == QStringLiteral("recycle-failed"))
        return QStringLiteral("The file could not be moved to the Recycle Bin.");
    return QStringLiteral("The folder could not be read. It may have moved or become unavailable.");
}

} // namespace

FileExplorerController::FileExplorerController(
    application::ApplicationServiceGraph *graph, services::FileExplorerService *service,
    platform::FileExplorerShell *shell, QObject *parent)
    : QObject(parent), m_graph(graph), m_service(service), m_shell(shell), m_entries(this)
{
    Q_ASSERT(m_graph && m_service && m_shell);
    Q_ASSERT(QThread::currentThread() == thread());
    m_queryTimer.setSingleShot(true);
    m_queryTimer.setInterval(200);
    connect(&m_queryTimer, &QTimer::timeout, this, [this] {
        if (!m_rootId.isEmpty() && m_pageVisible) {
            m_hasMore = false;
            m_truncated = false;
            requestListing(0, false);
        }
    });
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &FileExplorerController::onWorkerFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &FileExplorerController::onLifecycleChanged);
    onLifecycleChanged(m_graph->lifecycle().state());
}

void FileExplorerController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    m_ready = state == application::ApplicationLifecycle::State::Running;
    if (!m_ready) cancelAll();
    else if (m_pageVisible && m_hasDeferredNavigation) openDeferredNavigation();
    else if (!m_placesLoaded) requestPlaces();
    else if (m_pageVisible && !m_rootId.isEmpty()) requestListing(0, false);
    else if (m_pageVisible && !m_defaultRootId.isEmpty())
        navigate(m_defaultRootId, {}, HistoryMode::Replace);
    emit stateChanged();
}

void FileExplorerController::setPageVisible(bool visible)
{
    if (m_pageVisible == visible) return;
    m_pageVisible = visible;
    if (!visible) {
        m_queryTimer.stop();
        const auto pending = m_pending.constFind(m_listOperation);
        if (pending != m_pending.cend() && pending->historyMode != HistoryMode::None) {
            m_deferredLocation = {pending->rootId, pending->relativePath};
            m_deferredHistoryMode = pending->historyMode;
            m_deferredHistoryIndex = pending->historyIndex;
            m_hasDeferredNavigation = true;
        }
        cancelListing();
    } else if (m_ready) {
        if (m_hasDeferredNavigation) openDeferredNavigation();
        else if (!m_placesLoaded) requestPlaces();
        else if (!m_rootId.isEmpty()) requestListing(0, false);
        else if (!m_defaultRootId.isEmpty())
            navigate(m_defaultRootId, {}, HistoryMode::Replace);
    }
    emit stateChanged();
}

void FileExplorerController::openDeferredNavigation()
{
    if (!m_hasDeferredNavigation) return;
    const Location location = m_deferredLocation;
    const HistoryMode historyMode = m_deferredHistoryMode;
    const int historyIndex = m_deferredHistoryIndex;
    m_hasDeferredNavigation = false;
    m_deferredLocation = {};
    m_deferredHistoryMode = HistoryMode::None;
    m_deferredHistoryIndex = -1;
    navigate(location.rootId, location.relativePath, historyMode, historyIndex);
}

void FileExplorerController::requestPlaces()
{
    if (!m_ready || m_placesOperation) return;
    const auto operation = m_graph->beginOperation(QStringLiteral("load explorer places"),
                                                    QStringLiteral("file-explorer-places"));
    if (!operation.isValid()) return;
    Pending pending;
    pending.kind = Kind::Places;
    pending.operation = operation;
    m_pending.insert(operation.id.value, pending);
    m_placesOperation = operation.id.value;
    auto *service = m_service;
    if (!m_graph->submitExpensiveValue(operation,
        [service](const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            result.values = service->loadStandardPlaces(token);
            if (result.values.value(QStringLiteral("errorCode")).toString()
                == QStringLiteral("cancelled")) result.error = domain::AppError::cancelled();
            return result;
        })) {
        fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
        return;
    }
    emit stateChanged();
}

void FileExplorerController::addSessionRootFromUrl(const QUrl &folder)
{
    if (!folder.isLocalFile() || folder.toLocalFile().isEmpty()) {
        reportErrorCode(QStringLiteral("invalid-path"));
        emit stateChanged();
        return;
    }
    addSessionRoot(folder.toLocalFile());
}

void FileExplorerController::addSessionRoot(const QString &path)
{
    if (!m_ready || m_addingRoot || path.isEmpty()) return;
    cancelListing();
    const auto operation = m_graph->beginOperation(QStringLiteral("add file explorer folder"),
                                                    QStringLiteral("file-explorer-add-root"));
    if (!operation.isValid()) return;
    Pending pending;
    pending.kind = Kind::AddRoot;
    pending.operation = operation;
    m_pending.insert(operation.id.value, pending);
    m_addingRoot = true;
    m_errorMessage.clear();
    auto *service = m_service;
    if (!m_graph->submitExpensiveValue(operation,
        [service, path](const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            result.values = service->addSessionRoot(path, token);
            if (result.values.value(QStringLiteral("errorCode")).toString()
                == QStringLiteral("cancelled")) result.error = domain::AppError::cancelled();
            return result;
        })) {
        fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
        return;
    }
    emit stateChanged();
}

bool FileExplorerController::containsPlace(const QString &id) const
{
    for (const QVariant &value : m_places)
        if (value.toMap().value(QStringLiteral("rootId")).toString() == id) return true;
    return false;
}

void FileExplorerController::openPlace(const QString &id)
{
    if (!containsPlace(id)) {
        reportErrorCode(QStringLiteral("invalid-root"));
        emit stateChanged();
        return;
    }
    navigate(id, {}, HistoryMode::Push);
}

void FileExplorerController::navigateToPath(const QString &path)
{
    if (m_rootId.isEmpty()) return;
    navigate(m_rootId, path, HistoryMode::Push);
}

void FileExplorerController::navigate(QString rootId, QString relativePath,
                                      HistoryMode historyMode, int targetHistoryIndex)
{
    if (!m_ready || rootId.isEmpty()) return;
    m_queryTimer.stop();
    m_errorMessage.clear();
    m_notification.clear();
    relativePath.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!m_pageVisible) {
        m_deferredLocation = {std::move(rootId), std::move(relativePath)};
        m_deferredHistoryMode = historyMode;
        m_deferredHistoryIndex = targetHistoryIndex;
        m_hasDeferredNavigation = true;
        emit stateChanged();
        return;
    }
    requestListingFor(rootId, relativePath, {}, 0, false, historyMode,
                      targetHistoryIndex);
}

void FileExplorerController::goParent()
{
    if (!hasParent()) return;
    navigate(m_rootId, m_parentRelativePath, HistoryMode::Push);
}

void FileExplorerController::goBack()
{
    if (!canGoBack()) return;
    const int targetIndex = m_historyIndex - 1;
    const Location location = m_history.at(targetIndex);
    navigate(location.rootId, location.relativePath, HistoryMode::SelectIndex,
             targetIndex);
    emit stateChanged();
}

void FileExplorerController::goForward()
{
    if (!canGoForward()) return;
    const int targetIndex = m_historyIndex + 1;
    const Location location = m_history.at(targetIndex);
    navigate(location.rootId, location.relativePath, HistoryMode::SelectIndex,
             targetIndex);
    emit stateChanged();
}

void FileExplorerController::refresh()
{
    if (m_rootId.isEmpty()) return;
    m_queryTimer.stop();
    m_errorMessage.clear();
    m_notification.clear();
    requestListing(0, false);
}

void FileExplorerController::setQuery(const QString &query)
{
    if (query.toUcs4().size() > 255) {
        m_errorMessage = QStringLiteral("Search text must be 255 Unicode characters or fewer.");
        emit stateChanged();
        return;
    }
    if (m_query == query) {
        if (!m_errorMessage.isEmpty()) {
            m_errorMessage.clear();
            emit stateChanged();
        }
        return;
    }
    m_query = query;
    m_errorMessage.clear();
    cancelListing();
    m_queryTimer.stop();
    if (!m_rootId.isEmpty() && m_pageVisible) m_queryTimer.start();
    emit stateChanged();
}

void FileExplorerController::requestListing(int offset, bool append)
{
    if (!m_ready || !m_pageVisible || m_rootId.isEmpty()) return;
    if (append && (m_loading || !m_hasMore)) return;
    requestListingFor(m_rootId, m_relativePath, m_query, offset, append);
}

void FileExplorerController::requestListingFor(const QString &rootId,
                                               const QString &relativePath,
                                               const QString &query, int offset,
                                               bool append, HistoryMode historyMode,
                                               int targetHistoryIndex)
{
    if (!m_ready || !m_pageVisible || rootId.isEmpty()) return;
    if (append && (m_loading || !m_hasMore)) return;
    cancelListing();
    const auto operation = m_graph->beginOperation(QStringLiteral("list file explorer directory"),
                                                    QStringLiteral("file-explorer-list"));
    if (!operation.isValid()) return;
    Pending pending;
    pending.kind = Kind::Listing;
    pending.operation = operation;
    pending.rootId = rootId;
    pending.relativePath = relativePath;
    pending.query = query;
    pending.offset = offset;
    pending.historyMode = historyMode;
    pending.historyIndex = targetHistoryIndex;
    pending.append = append;
    m_pending.insert(operation.id.value, pending);
    m_listOperation = operation.id.value;
    m_loading = true;
    auto *service = m_service;
    const int requestedOffset = offset;
    if (!m_graph->submitExpensiveValue(operation,
        [service, rootId, relativePath, query, requestedOffset](
            const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            result.values = service->listDirectory(rootId, relativePath, query,
                                                    requestedOffset, 250, token);
            if (result.values.value(QStringLiteral("errorCode")).toString()
                == QStringLiteral("cancelled")) result.error = domain::AppError::cancelled();
            return result;
        })) {
        fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
        return;
    }
    emit stateChanged();
}

void FileExplorerController::loadMore()
{
    if (m_hasMore && !m_loading && !m_queryTimer.isActive())
        requestListing(m_entries.count(), true);
}

void FileExplorerController::selectEntry(const QString &path)
{
    if (!m_entries.selectPath(path)) return;
    m_errorMessage.clear();
    emit stateChanged();
}

void FileExplorerController::activateEntry(const QString &path)
{
    const int row = m_entries.rowForPath(path);
    if (row < 0) return;
    const models::FileExplorerEntry entry = m_entries.entryAt(row);
    if (entry.directory && entry.canNavigate) {
        navigate(m_rootId, path, HistoryMode::Push);
        return;
    }
    if (!entry.canOpen) {
        reportErrorCode(entry.directory ? QStringLiteral("unsafe-link")
                                        : QStringLiteral("not-file"));
        emit stateChanged();
        return;
    }
    if (!nativeActionsSupported()) {
        reportErrorCode(QStringLiteral("unsupported"));
        emit stateChanged();
        return;
    }
    if (!m_ready || m_actionBusy) return;
    const auto operation = m_graph->beginOperation(QStringLiteral("open explorer file"),
                                                    QStringLiteral("file-explorer-action"));
    if (!operation.isValid()) return;
    Pending pending;
    pending.kind = Kind::OpenFile;
    pending.operation = operation;
    pending.rootId = m_rootId;
    pending.relativePath = path;
    m_pending.insert(operation.id.value, pending);
    m_actionBusy = true;
    m_errorMessage.clear();
    auto *service = m_service;
    const QString rootId = m_rootId;
    if (!m_graph->submitExpensiveValue(operation,
        [service, rootId, path](const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            result.values = service->openFile(rootId, path, token);
            if (result.values.value(QStringLiteral("errorCode")).toString()
                == QStringLiteral("cancelled")) result.error = domain::AppError::cancelled();
            return result;
        })) {
        fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
        return;
    }
    emit stateChanged();
}

void FileExplorerController::activateSelected()
{
    activateEntry(m_entries.selectedPath());
}

void FileExplorerController::recycleFile(const QString &path)
{
    const int row = m_entries.rowForPath(path);
    if (row < 0 || !m_entries.entryAt(row).canRecycle || !m_ready || m_actionBusy) return;
    if (!nativeActionsSupported()) {
        reportErrorCode(QStringLiteral("unsupported"));
        emit stateChanged();
        return;
    }
    const auto operation = m_graph->beginOperation(QStringLiteral("move explorer file to recycle bin"),
                                                    QStringLiteral("file-explorer-action"));
    if (!operation.isValid()) return;
    Pending pending;
    pending.kind = Kind::RecycleFile;
    pending.operation = operation;
    pending.rootId = m_rootId;
    pending.relativePath = path;
    m_pending.insert(operation.id.value, pending);
    m_actionBusy = true;
    m_errorMessage.clear();
    auto *service = m_service;
    const QString rootId = m_rootId;
    if (!m_graph->submitExpensiveValue(operation,
        [service, rootId, path](const application::CancellationToken &token) {
            services::BoundedWorkerPool::ValueResult result;
            result.values = service->recycleFile(rootId, path, token);
            if (result.values.value(QStringLiteral("errorCode")).toString()
                == QStringLiteral("cancelled")) result.error = domain::AppError::cancelled();
            return result;
        })) {
        fail(operation.id.value, domain::AppError(domain::ErrorCategory::Internal));
        return;
    }
    emit stateChanged();
}

void FileExplorerController::onWorkerFinished(quint64 id, const QVariantMap &values,
                                              domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Pending pending = it.value();
    if (error.isCancelled()) {
        finish(id, error);
        return;
    }
    if (error.isError()) {
        fail(id, error);
        return;
    }
    const QString errorCode = values.value(QStringLiteral("errorCode")).toString();
    if (!values.value(QStringLiteral("ok")).toBool()) {
        fail(id, appErrorForCode(errorCode), errorCode);
        return;
    }

    switch (pending.kind) {
    case Kind::Places: {
        m_places = values.value(QStringLiteral("places")).toList();
        m_placesLoaded = true;
        m_errorMessage.clear();
        QString initialRoot;
        for (const QVariant &place : m_places) {
            const QVariantMap item = place.toMap();
            if (item.value(QStringLiteral("kind")).toString() == QStringLiteral("home")) {
                initialRoot = item.value(QStringLiteral("rootId")).toString();
                break;
            }
        }
        if (initialRoot.isEmpty() && !m_places.isEmpty())
            initialRoot = m_places.first().toMap().value(QStringLiteral("rootId")).toString();
        m_defaultRootId = initialRoot;
        finish(id);
        if (!initialRoot.isEmpty() && m_history.isEmpty() && m_pageVisible
            && !m_listOperation && m_rootId.isEmpty() && !m_hasDeferredNavigation)
            navigate(initialRoot, {}, HistoryMode::Replace);
        break;
    }
    case Kind::AddRoot: {
        const QVariantMap root = values.value(QStringLiteral("root")).toMap();
        const QString newRootId = root.value(QStringLiteral("rootId")).toString();
        bool found = false;
        for (const QVariant &place : std::as_const(m_places))
            if (place.toMap().value(QStringLiteral("rootId")).toString() == newRootId) found = true;
        if (!found) m_places.push_back(root);
        finish(id);
        navigate(newRootId, {}, HistoryMode::Push);
        break;
    }
    case Kind::Listing: {
        if (pending.operation.id.value != m_listOperation) {
            finish(id);
            break;
        }
        const QVector<models::FileExplorerEntry> page =
            entriesFromValue(values.value(QStringLiteral("entries")).toList());
        if (!pending.append
            && (m_rootId != pending.rootId || m_relativePath != pending.relativePath))
            m_entries.selectPath({});
        if (pending.append) m_entries.append(page);
        else m_entries.replace(page);
        m_parentRelativePath = values.value(QStringLiteral("parentRelativePath")).toString();
        m_breadcrumbs = values.value(QStringLiteral("breadcrumbs")).toList();
        m_hasMore = values.value(QStringLiteral("hasMore")).toBool();
        m_truncated = values.value(QStringLiteral("truncated")).toBool();
        if (!pending.append) {
            m_rootId = pending.rootId;
            m_relativePath = pending.relativePath;
            m_query = pending.query;
            rememberLocation({m_rootId, m_relativePath}, pending.historyMode,
                             pending.historyIndex);
        }
        m_errorMessage.clear();
        finish(id);
        break;
    }
    case Kind::OpenFile:
        m_notification = QStringLiteral("File opened.");
        finish(id);
        break;
    case Kind::RecycleFile:
        m_notification = QStringLiteral("File moved to the Recycle Bin.");
        finish(id);
        if (!m_rootId.isEmpty() && m_pageVisible) requestListing(0, false);
        break;
    }
}

void FileExplorerController::rememberLocation(const Location &location,
                                              HistoryMode historyMode,
                                              int targetHistoryIndex)
{
    if (historyMode == HistoryMode::None) return;
    if (historyMode == HistoryMode::SelectIndex) {
        if (targetHistoryIndex >= 0 && targetHistoryIndex < m_history.size())
            m_historyIndex = targetHistoryIndex;
        return;
    }
    if (historyMode == HistoryMode::Replace) {
        m_history = {location};
        m_historyIndex = 0;
        return;
    }
    const Location current = m_historyIndex >= 0 && m_historyIndex < m_history.size()
        ? m_history.at(m_historyIndex) : Location{};
    if (current.rootId == location.rootId && current.relativePath == location.relativePath)
        return;
    while (m_history.size() > m_historyIndex + 1) m_history.removeLast();
    m_history.push_back(location);
    if (m_history.size() > 50) m_history.removeFirst();
    m_historyIndex = m_history.size() - 1;
}

QVector<models::FileExplorerEntry> FileExplorerController::entriesFromValue(
    const QVariantList &values)
{
    QVector<models::FileExplorerEntry> result;
    result.reserve(values.size());
    for (const QVariant &value : values) {
        const QVariantMap row = value.toMap();
        result.push_back({row.value(QStringLiteral("name")).toString(),
                          row.value(QStringLiteral("relativePath")).toString(),
                          row.value(QStringLiteral("type")).toString(),
                          row.value(QStringLiteral("modified")).toString(),
                          row.value(QStringLiteral("size")).toULongLong(),
                          row.value(QStringLiteral("directory")).toBool(),
                          row.value(QStringLiteral("canNavigate")).toBool(),
                          row.value(QStringLiteral("canOpen")).toBool(),
                          row.value(QStringLiteral("canRecycle")).toBool()});
    }
    return result;
}

domain::AppError FileExplorerController::appErrorForCode(const QString &code)
{
    if (code == QStringLiteral("cancelled")) return domain::AppError::cancelled();
    if (code == QStringLiteral("invalid-path") || code == QStringLiteral("invalid-page")
        || code == QStringLiteral("invalid-query"))
        return domain::AppError(domain::ErrorCategory::Validation);
    if (code == QStringLiteral("not-found") || code == QStringLiteral("invalid-root")
        || code == QStringLiteral("unavailable-root"))
        return domain::AppError(domain::ErrorCategory::NotFound);
    if (code == QStringLiteral("permission-denied") || code == QStringLiteral("unsupported"))
        return domain::AppError(domain::ErrorCategory::CapabilityUnavailable);
    return domain::AppError(domain::ErrorCategory::Platform);
}

void FileExplorerController::reportErrorCode(const QString &code)
{
    m_errorMessage = errorText(code);
}

void FileExplorerController::finish(quint64 id, domain::AppError error)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    const Kind kind = it->kind;
    m_pending.erase(it);
    if (m_listOperation == id) m_listOperation = 0;
    if (m_placesOperation == id) m_placesOperation = 0;
    if (kind == Kind::Listing) m_loading = false;
    if (kind == Kind::AddRoot) m_addingRoot = false;
    if (kind == Kind::OpenFile || kind == Kind::RecycleFile) m_actionBusy = false;
    m_graph->completeOperation({id}, error);
    emit stateChanged();
}

void FileExplorerController::fail(quint64 id, domain::AppError error,
                                  const QString &errorCode)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    if (!error.isCancelled()) {
        if (!errorCode.isEmpty()) reportErrorCode(errorCode);
        else m_errorMessage = error.userMessage();
    }
    finish(id, error);
}

void FileExplorerController::cancelListing()
{
    const quint64 id = m_listOperation;
    if (!id) return;
    m_listOperation = 0;
    m_pending.remove(id);
    m_graph->cancelOperation({id});
    m_loading = false;
}

void FileExplorerController::cancelAll()
{
    m_queryTimer.stop();
    const auto pending = m_pending;
    m_pending.clear();
    m_placesOperation = 0;
    m_listOperation = 0;
    m_loading = false;
    m_addingRoot = false;
    m_actionBusy = false;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        m_graph->cancelOperation({it.key()});
    emit stateChanged();
}

void FileExplorerController::clearMessages()
{
    if (m_errorMessage.isEmpty() && m_notification.isEmpty()) return;
    m_errorMessage.clear();
    m_notification.clear();
    emit stateChanged();
}

} // namespace something::controllers

#include "wallpaper_controller.h"

#include "../application/application_lifecycle.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>

#include <utility>

namespace something::controllers {
namespace {

QVariantList builtInWallpapers()
{
    return {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("builtin:original")},
                    {QStringLiteral("label"), QStringLiteral("Original")},
                    {QStringLiteral("description"), QStringLiteral("Midnight glow")},
                    {QStringLiteral("imageSource"), QString()},
                    {QStringLiteral("builtIn"), true},
                    {QStringLiteral("available"), true}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("builtin:sandrone")},
                    {QStringLiteral("label"), QStringLiteral("Sandrone")},
                    {QStringLiteral("description"), QStringLiteral("Warm crimson")},
                    {QStringLiteral("imageSource"), QStringLiteral("qrc:/something/assets/wallpapers/sandrone.jpg")},
                    {QStringLiteral("builtIn"), true},
                    {QStringLiteral("available"), true}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("builtin:hu-tao")},
                    {QStringLiteral("label"), QStringLiteral("Hu Tao")},
                    {QStringLiteral("description"), QStringLiteral("Dusky violet")},
                    {QStringLiteral("imageSource"), QStringLiteral("qrc:/something/assets/wallpapers/hu-tao.jpg")},
                    {QStringLiteral("builtIn"), true},
                    {QStringLiteral("available"), true}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("builtin:skirk")},
                    {QStringLiteral("label"), QStringLiteral("Skirk")},
                    {QStringLiteral("description"), QStringLiteral("Deep ocean")},
                    {QStringLiteral("imageSource"), QStringLiteral("qrc:/something/assets/wallpapers/skirk.jpg")},
                    {QStringLiteral("builtIn"), true},
                    {QStringLiteral("available"), true}},
    };
}

QString safeErrorMessage(domain::AppError error)
{
    if (error.category() == domain::ErrorCategory::Validation) {
        return QStringLiteral("Choose a readable JPEG, PNG, or WebP image between 1 byte and 20 MB.");
    }
    return error.userMessage();
}

} // namespace

WallpaperController::WallpaperController(application::ApplicationServiceGraph *graph,
                                         providers::WallpaperImageProvider *imageProvider,
                                         QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_imageProvider(imageProvider)
    , m_wallpapers(builtInWallpapers())
{
    Q_ASSERT(m_graph);
    Q_ASSERT(QThread::currentThread() == thread());
    applySelection(services::WallpaperService::defaultSelection());

    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &WallpaperController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveJobFinished,
            this, &WallpaperController::onExpensiveFinished);
    connect(&m_assetWatcher, &QFileSystemWatcher::fileChanged, this,
            [this](const QString &) {
                if (m_selectedWallpaper.startsWith(QStringLiteral("custom:"))) {
                    const QVariantMap selected = wallpaperRecord(m_selectedWallpaper.mid(7));
                    if (selected.isEmpty()
                        || !QFileInfo(QDir(m_graph->profileDirectory()).filePath(
                               QStringLiteral("assets/wallpapers/")
                               + selected.value(QStringLiteral("filename")).toString())).isFile()) {
                        fallbackIfMissing(m_selectedWallpaper);
                    }
                }
            });
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, [this](application::ApplicationLifecycle::State state) {
                if (state == application::ApplicationLifecycle::State::Running) {
                    setReady(true);
                    services::WallpaperAssetStore::removeAbandonedStagingFiles(
                        m_graph->profileDirectory());
                    reload();
                } else {
                    setReady(false);
                    if (state == application::ApplicationLifecycle::State::Failed) {
                        setBusy(false);
                        setError(safeErrorMessage(m_graph->lifecycle().startupError()));
                    }
                }
            });
}

void WallpaperController::reload()
{
    if (!m_ready || m_busy
        || m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running) {
        return;
    }
    const auto operation = m_graph->beginOperation(QStringLiteral("load wallpapers"),
                                                    QStringLiteral("wallpaper-catalog"));
    if (!operation.isValid()) return;
    setBusy(true);
    setError({});
    submitStorage(operation, StorageAction::Load, services::WallpaperService::loadCatalog());
}

void WallpaperController::selectWallpaper(const QString &selection)
{
    if (!m_ready || m_busy
        || m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running) {
        return;
    }
    const QString normalized = selection.trimmed();
    const domain::AppError validation = services::WallpaperService::validateSelection(
        normalized, m_customWallpapers, m_graph->profileDirectory());
    if (validation.isError()) {
        setError(QStringLiteral("That wallpaper is no longer available."));
        fallbackIfMissing(normalized);
        return;
    }
    if (normalized == m_selectedWallpaper) return;

    const QString previous = m_selectedWallpaper;
    applySelection(normalized);
    setError({});
    const auto operation = m_graph->beginOperation(QStringLiteral("select wallpaper"),
                                                    QStringLiteral("wallpaper-selection"));
    if (!operation.isValid()) {
        applySelection(previous);
        return;
    }
    m_previousSelections.insert(operation.id.value, previous);
    setBusy(true);
    if (!submitStorage(operation, StorageAction::Select,
                       services::WallpaperService::persistSelection(normalized))) {
        m_previousSelections.remove(operation.id.value);
        applySelection(previous);
    }
}

void WallpaperController::importWallpaper(const QUrl &sourceUrl)
{
    if (!m_ready || m_busy || !sourceUrl.isLocalFile()
        || m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running) {
        return;
    }
    const auto operation = m_graph->beginOperation(QStringLiteral("import wallpaper"),
                                                    QStringLiteral("wallpaper-import"));
    if (!operation.isValid()) return;
    auto staged = std::make_shared<services::StagedWallpaper>();
    const QString sourcePath = sourceUrl.toLocalFile();
    const QString profileDirectory = m_graph->profileDirectory();
    m_expensiveActions.insert(operation.id.value, ExpensiveAction::ImportStage);
    m_stagedImports.insert(operation.id.value, staged);
    setBusy(true);
    setError({});
    const bool submitted = m_graph->submitExpensive(operation,
        [sourcePath, profileDirectory, staged](const application::CancellationToken &cancellation) {
            return services::WallpaperAssetStore::stageImport(
                sourcePath, profileDirectory, cancellation, staged.get());
        });
    if (!submitted) {
        m_expensiveActions.remove(operation.id.value);
        m_stagedImports.remove(operation.id.value);
        setBusy(false);
        setError(QStringLiteral("Wallpaper import could not be started."));
    }
}

void WallpaperController::deleteWallpaper(const QString &id)
{
    if (!m_ready || m_busy
        || m_graph->lifecycle().state() != application::ApplicationLifecycle::State::Running) {
        return;
    }
    const QVariantMap row = wallpaperRecord(id);
    if (row.isEmpty() || row.value(QStringLiteral("builtIn")).toBool()) return;
    const auto operation = m_graph->beginOperation(QStringLiteral("delete wallpaper"),
                                                    QStringLiteral("wallpaper-delete"));
    if (!operation.isValid()) return;
    setBusy(true);
    setError({});
    const QString profileDirectory = m_graph->profileDirectory();
    const QString databaseId = row.value(QStringLiteral("id")).toString().mid(7);
    const storage::Wallpaper wallpaper{
        databaseId,
        row.value(QStringLiteral("label")).toString(),
        {row.value(QStringLiteral("filename")).toString(),
         row.value(QStringLiteral("mimeType")).toString(),
         row.value(QStringLiteral("byteSize")).toLongLong()},
        row.value(QStringLiteral("createdAt")).toString(),
    };
    submitStorage(operation, StorageAction::Delete,
                  services::WallpaperService::deleteWallpaper(profileDirectory, wallpaper));
}

void WallpaperController::fallbackIfMissing(const QString &selection)
{
    if (!m_ready || !services::WallpaperService::isCustomSelection(selection)) return;
    const QVariantMap record = wallpaperRecord(selection.mid(7));
    if (record.isEmpty()) return;
    const QString path = QDir(m_graph->profileDirectory()).filePath(
        QStringLiteral("assets/wallpapers/")
        + record.value(QStringLiteral("filename")).toString());
    const QFileInfo info(path);
    if (info.isFile() && !info.isSymLink()) return;

    QVariantList updated = m_wallpapers;
    for (qsizetype i = 0; i < updated.size(); ++i) {
        QVariantMap row = updated.at(i).toMap();
        if (row.value(QStringLiteral("id")).toString() != selection) continue;
        if (!row.value(QStringLiteral("available")).toBool()) break;
        row.insert(QStringLiteral("available"), false);
        row.insert(QStringLiteral("description"), QStringLiteral("Image file is unavailable"));
        updated[i] = row;
        m_wallpapers = std::move(updated);
        emit wallpapersChanged();
        break;
    }

    if (m_busy || selection != m_selectedWallpaper) return;
    selectWallpaper(services::WallpaperService::defaultSelection());
}

void WallpaperController::clearNotification()
{
    setNotification({});
}

void WallpaperController::onStorageFinished(quint64 operationId, const QVariantMap &values,
                                            domain::AppError error)
{
    const auto actionIt = m_storageActions.find(operationId);
    if (actionIt == m_storageActions.end()) return;
    const StorageAction action = actionIt.value();
    m_storageActions.erase(actionIt);

    if (action == StorageAction::ImportCommit) {
        const auto stagedIt = m_stagedImports.find(operationId);
        if (stagedIt != m_stagedImports.end()) {
            if (error.isError()) {
                services::WallpaperAssetStore::discardStaged(m_graph->profileDirectory(),
                                                              *stagedIt.value());
            } else if (stagedIt.value() && m_imageProvider) {
                m_imageProvider->invalidate(stagedIt.value()->id);
            }
            m_stagedImports.erase(stagedIt);
        }
    }

    if (error.isError()) {
        if (action == StorageAction::Select) {
            const QString previous = m_previousSelections.take(operationId);
            if (!previous.isEmpty()) {
                const bool previousAvailable = !services::WallpaperService::validateSelection(
                    previous, m_customWallpapers, m_graph->profileDirectory()).isError();
                applySelection(previousAvailable ? previous
                    : services::WallpaperService::defaultSelection());
            }
        }
        if (action == StorageAction::Delete) watchSelectedAsset();
        setBusy(false);
        setError(safeErrorMessage(error));
        return;
    }
    if (error.isCancelled()) {
        if (action == StorageAction::Select) {
            const QString previous = m_previousSelections.take(operationId);
            if (!previous.isEmpty()) {
                const bool previousAvailable = !services::WallpaperService::validateSelection(
                    previous, m_customWallpapers, m_graph->profileDirectory()).isError();
                applySelection(previousAvailable ? previous
                    : services::WallpaperService::defaultSelection());
            }
        }
        if (action == StorageAction::Delete) watchSelectedAsset();
        setBusy(false);
        return;
    }

    switch (action) {
    case StorageAction::Load: {
        QVariantList combined = builtInWallpapers();
        const QVariantList rows = values.value(QStringLiteral("wallpapers")).toList();
        m_customWallpapers.clear();
        const QString directory = QDir(m_graph->profileDirectory()).filePath(
            QStringLiteral("assets/wallpapers"));
        QStringList persistedFilenames;
        for (const QVariant &value : rows)
            persistedFilenames.push_back(value.toMap().value(QStringLiteral("filename")).toString());
        services::WallpaperAssetStore::recoverStagedDeletions(
            m_graph->profileDirectory(), persistedFilenames);
        services::WallpaperAssetStore::removeUnreferencedWallpaperFiles(
            m_graph->profileDirectory(), persistedFilenames);
        for (const QVariant &value : rows) {
            QVariantMap row = value.toMap();
            const QString id = row.value(QStringLiteral("id")).toString();
            const QString filename = row.value(QStringLiteral("filename")).toString();
            m_customWallpapers.push_back({id, row.value(QStringLiteral("label")).toString(),
                {filename, row.value(QStringLiteral("mimeType")).toString(),
                 row.value(QStringLiteral("byteSize")).toLongLong()},
                row.value(QStringLiteral("createdAt")).toString()});
            static const QRegularExpression idPattern(QStringLiteral("^[a-f0-9]{32}$"));
            static const QRegularExpression filenamePattern(
                QStringLiteral("^[a-f0-9]{32}\\.(jpg|png|webp)$"));
            const bool safeId = idPattern.match(id).hasMatch()
                && filenamePattern.match(filename).hasMatch()
                && filename.startsWith(id + QLatin1Char('.'));
            const QFileInfo assetInfo(QDir(directory).filePath(filename));
            const bool available = safeId && assetInfo.isFile() && !assetInfo.isSymLink();
            row.insert(QStringLiteral("id"), QStringLiteral("custom:") + id);
            row.insert(QStringLiteral("imageSource"), available
                ? QStringLiteral("image://wallpapers/%1").arg(id)
                : QString());
            row.insert(QStringLiteral("description"), available
                ? QStringLiteral("Uploaded image")
                : QStringLiteral("Image file is unavailable"));
            row.insert(QStringLiteral("builtIn"), false);
            row.insert(QStringLiteral("available"), available);
            combined.push_back(row);
        }
        m_wallpapers = combined;
        emit wallpapersChanged();

        const QString storedSelection = values.value(QStringLiteral("selected")).toString();
        QString selected = storedSelection;
        bool valid = false;
        for (const QVariant &value : std::as_const(m_wallpapers)) {
            const QVariantMap row = value.toMap();
            if (row.value(QStringLiteral("id")).toString() == selected
                && row.value(QStringLiteral("available")).toBool()) {
                valid = true;
                break;
            }
        }
        if (!valid) selected = services::WallpaperService::defaultSelection();
        if (!valid) applySelection(storedSelection);
        else applySelection(selected);
        setBusy(false);
        if (selected != storedSelection) {
            selectWallpaper(selected);
        }
        return;
    }
    case StorageAction::Select: {
        m_previousSelections.remove(operationId);
        const QString selected = values.value(QStringLiteral("selected")).toString();
        applySelection(selected);
        setBusy(false);
        if (services::WallpaperService::isCustomSelection(selected)
            && services::WallpaperService::validateSelection(
                   selected, m_customWallpapers, m_graph->profileDirectory()).isError()) {
            fallbackIfMissing(selected);
        }
        return;
    }
    case StorageAction::ImportCommit:
        setBusy(false);
        setNotification(QStringLiteral("Wallpaper imported and selected."));
        reload();
        return;
    case StorageAction::Delete: {
        const QString deletedId = values.value(QStringLiteral("deletedId")).toString();
        if (m_imageProvider) m_imageProvider->invalidate(deletedId);
        applySelection(values.value(QStringLiteral("selected")).toString());
        setBusy(false);
        setNotification(QStringLiteral("Wallpaper deleted."));
        reload();
        return;
    }
    }
}

void WallpaperController::onExpensiveFinished(quint64 operationId, domain::AppError error)
{
    const auto actionIt = m_expensiveActions.find(operationId);
    if (actionIt == m_expensiveActions.end()) return;
    m_expensiveActions.erase(actionIt);
    const auto stagedIt = m_stagedImports.find(operationId);
    if (stagedIt == m_stagedImports.end()) {
        setBusy(false);
        return;
    }
    const std::shared_ptr<services::StagedWallpaper> staged = stagedIt.value();
    m_stagedImports.erase(stagedIt);
    if (error.isError()) {
        services::WallpaperAssetStore::discardStaged(m_graph->profileDirectory(), *staged);
        setBusy(false);
        setError(safeErrorMessage(error));
        return;
    }
    if (error.isCancelled()) {
        services::WallpaperAssetStore::discardStaged(m_graph->profileDirectory(), *staged);
        setBusy(false);
        return;
    }
    beginImportCommit(*staged);
}

bool WallpaperController::submitStorage(application::OperationContext operation,
                                        StorageAction action,
                                        storage::StorageCommand command)
{
    if (!operation.isValid()) return false;
    m_storageActions.insert(operation.id.value, action);
    if (m_graph->submitStorage(operation, std::move(command))) return true;
    m_storageActions.remove(operation.id.value);
    setBusy(false);
    setError(QStringLiteral("Wallpaper data could not be saved."));
    return false;
}

void WallpaperController::beginImportCommit(const services::StagedWallpaper &staged)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("save imported wallpaper"),
                                                    QStringLiteral("wallpaper-import-save"));
    if (!operation.isValid()) {
        services::WallpaperAssetStore::discardStaged(m_graph->profileDirectory(), staged);
        setBusy(false);
        return;
    }
    const QString profileDirectory = m_graph->profileDirectory();
    m_stagedImports.insert(operation.id.value,
        std::make_shared<services::StagedWallpaper>(staged));
    if (!submitStorage(operation, StorageAction::ImportCommit,
        services::WallpaperService::commitImport(profileDirectory, staged))) {
        m_stagedImports.remove(operation.id.value);
        services::WallpaperAssetStore::discardStaged(profileDirectory, staged);
    }
}

void WallpaperController::setBusy(bool busy)
{
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged();
    if (!busy && m_ready) {
        const QString selected = m_selectedWallpaper;
        QTimer::singleShot(0, this, [this, selected] {
            if (m_ready) fallbackIfMissing(selected);
        });
    }
}

void WallpaperController::setReady(bool ready)
{
    if (m_ready == ready) return;
    m_ready = ready;
    emit readyChanged();
}

void WallpaperController::setError(QString message)
{
    if (m_errorMessage == message) return;
    m_errorMessage = std::move(message);
    emit errorMessageChanged();
}

void WallpaperController::setNotification(QString message)
{
    if (m_notification == message) return;
    m_notification = std::move(message);
    emit notificationChanged();
}

void WallpaperController::applySelection(QString selection)
{
    if (selection.isEmpty()) selection = services::WallpaperService::defaultSelection();
    m_selectedWallpaper = std::move(selection);
    QString source;
    for (const QVariant &value : std::as_const(m_wallpapers)) {
        const QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("id")).toString() == m_selectedWallpaper) {
            source = row.value(QStringLiteral("imageSource")).toString();
            break;
        }
    }
    m_wallpaperImageSource = source;
    watchSelectedAsset();
    emit selectedWallpaperChanged();
}

void WallpaperController::watchSelectedAsset()
{
    const QStringList watched = m_assetWatcher.files();
    if (!watched.isEmpty()) m_assetWatcher.removePaths(watched);
    if (m_selectedWallpaper.startsWith(QStringLiteral("custom:"))) {
        const QVariantMap record = wallpaperRecord(m_selectedWallpaper.mid(7));
        if (!record.isEmpty()) {
            const QString path = QDir(m_graph->profileDirectory()).filePath(
                QStringLiteral("assets/wallpapers/")
                + record.value(QStringLiteral("filename")).toString());
            const QFileInfo info(path);
            if (info.isFile() && !info.isSymLink()) m_assetWatcher.addPath(path);
        }
    }
}

QVariantMap WallpaperController::wallpaperRecord(const QString &id) const
{
    const QString selection = id.startsWith(QStringLiteral("custom:"))
        ? id : QStringLiteral("custom:") + id;
    for (const QVariant &value : m_wallpapers) {
        const QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("id")).toString() == selection) return row;
    }
    return {};
}

} // namespace something::controllers

#pragma once

#include "../application/application_service_graph.h"
#include "../providers/wallpaper_image_provider.h"
#include "../services/wallpaper_asset_store.h"
#include "../services/wallpaper_service.h"

#include <QHash>
#include <QFileSystemWatcher>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

namespace something::controllers {

class WallpaperController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList wallpapers READ wallpapers NOTIFY wallpapersChanged)
    Q_PROPERTY(QString selectedWallpaper READ selectedWallpaper NOTIFY selectedWallpaperChanged)
    Q_PROPERTY(QString wallpaperImageSource READ wallpaperImageSource NOTIFY selectedWallpaperChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY notificationChanged)

public:
    explicit WallpaperController(application::ApplicationServiceGraph *graph,
                                 providers::WallpaperImageProvider *imageProvider,
                                 QObject *parent = nullptr);

    QVariantList wallpapers() const { return m_wallpapers; }
    QString selectedWallpaper() const { return m_selectedWallpaper; }
    QString wallpaperImageSource() const { return m_wallpaperImageSource; }
    bool ready() const noexcept { return m_ready; }
    bool busy() const noexcept { return m_busy; }
    QString errorMessage() const { return m_errorMessage; }
    QString notification() const { return m_notification; }

    Q_INVOKABLE void reload();
    Q_INVOKABLE void selectWallpaper(const QString &selection);
    Q_INVOKABLE void importWallpaper(const QUrl &sourceUrl);
    Q_INVOKABLE void deleteWallpaper(const QString &id);
    Q_INVOKABLE void fallbackIfMissing(const QString &selection);
    Q_INVOKABLE void clearNotification();

signals:
    void wallpapersChanged();
    void selectedWallpaperChanged();
    void readyChanged();
    void busyChanged();
    void errorMessageChanged();
    void notificationChanged();

private:
    enum class StorageAction { Load, Select, ImportCommit, Delete };
    enum class ExpensiveAction { ImportStage };

    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void onExpensiveFinished(quint64 operationId, domain::AppError error);
    bool submitStorage(application::OperationContext operation,
                       StorageAction action,
                       storage::StorageCommand command);
    void beginImportCommit(const services::StagedWallpaper &staged);
    void setReady(bool ready);
    void setBusy(bool busy);
    void setError(QString message);
    void setNotification(QString message);
    void applySelection(QString selection);
    void watchSelectedAsset();
    QVariantMap wallpaperRecord(const QString &id) const;

    application::ApplicationServiceGraph *m_graph{nullptr};
    providers::WallpaperImageProvider *m_imageProvider{nullptr};
    QVariantList m_wallpapers;
    QString m_selectedWallpaper{QStringLiteral("builtin:hu-tao")};
    QString m_wallpaperImageSource;
    QString m_errorMessage;
    QString m_notification;
    QHash<quint64, StorageAction> m_storageActions;
    QHash<quint64, ExpensiveAction> m_expensiveActions;
    QHash<quint64, std::shared_ptr<services::StagedWallpaper>> m_stagedImports;
    QHash<quint64, QString> m_previousSelections;
    QVector<storage::Wallpaper> m_customWallpapers;
    QFileSystemWatcher m_assetWatcher;
    bool m_ready{false};
    bool m_busy{false};
};

} // namespace something::controllers

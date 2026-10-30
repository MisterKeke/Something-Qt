#include <QGuiApplication>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QCursor>
#include <QQmlContext>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QScreen>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QWindow>

#include <algorithm>

#include "application/application_service_graph.h"
#include "application/route_catalog.h"
#include "controllers/application_controller.h"
#include "controllers/calendar_controller.h"
#include "controllers/currency_controller.h"
#include "controllers/cursor_trail_controller.h"
#include "controllers/dashboard_controller.h"
#include "controllers/desktop_applications_controller.h"
#include "controllers/favorites_controller.h"
#include "controllers/file_explorer_controller.h"
#include "controllers/legacy_data_import_controller.h"
#include "controllers/running_applications_controller.h"
#include "controllers/scheduling_controller.h"
#include "controllers/settings_controller.h"
#include "controllers/setups_controller.h"
#include "controllers/steam_games_controller.h"
#include "controllers/tasks_controller.h"
#include "controllers/task_history_controller.h"
#include "controllers/terminal_controller.h"
#include "controllers/wallpaper_controller.h"
#include "controllers/weather_controller.h"
#include "services/application_mutation_workflow.h"
#include "platform/location_provider.h"
#include "platform/desktop_applications_platform.h"
#include "platform/file_explorer_platform.h"
#include "platform/system_clock.h"
#include "providers/wallpaper_image_provider.h"
#include "providers/provider_image_provider.h"
#include "providers/application_icon_image_provider.h"
#include "providers/setup_icon_image_provider.h"
#include "providers/steam_artwork_image_provider.h"
#include "services/file_explorer_service.h"
#include "services/application_command_service.h"

namespace {

void showCenteredOnLaunch(QWindow &window)
{
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen || screen->availableGeometry().isEmpty()) {
        window.showNormal();
        return;
    }

    window.setScreen(screen);
    // Create the native frame while hidden so its title bar and resize borders
    // can be included in the initial size and position.
    window.create();
    const QRect available = screen->availableGeometry();
    const QMargins frame = window.frameMargins();
    const QSize frameSize(frame.left() + frame.right(), frame.top() + frame.bottom());
    constexpr int edgeGap = 16;
    const QSize clientLimit(
        std::max(1, available.width() - frameSize.width() - 2 * edgeGap),
        std::max(1, available.height() - frameSize.height() - 2 * edgeGap));
    // A scaled or smaller monitor may have less space than the design minimum.
    window.setMinimumSize(window.minimumSize().boundedTo(clientLimit));
    window.resize(window.size().boundedTo(clientLimit));
    const QSize outerSize = window.size() + frameSize;
    window.setFramePosition(available.topLeft()
        + QPoint((available.width() - outerSize.width()) / 2,
                 (available.height() - outerSize.height()) / 2));
    // Position once, rather than binding x/y, so normal title-bar dragging works.
    window.showNormal();
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    const int bundledFontId = QFontDatabase::addApplicationFont(
        QStringLiteral(":/something/assets/fonts/CascadiaCode.ttf"));
    if (bundledFontId >= 0) {
        const QStringList bundledFontFamilies = QFontDatabase::applicationFontFamilies(bundledFontId);
        if (!bundledFontFamilies.isEmpty()) {
            QFont applicationFont(bundledFontFamilies.constFirst());
            applicationFont.setPointSizeF(10.5);
            QGuiApplication::setFont(applicationFont);
        }
    }
    QGuiApplication::setApplicationName(QStringLiteral("Something"));
    QGuiApplication::setOrganizationName(QStringLiteral("Something"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/something/assets/appicon.png")));
    QGuiApplication::setQuitOnLastWindowClosed(false);
    // The QML controls provide their own backgrounds/content. Native Windows
    // styles reject those customizations, so use the supported base style.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    something::application::ApplicationServiceGraph services;
    something::services::ApplicationMutationWorkflow mutationWorkflow(&services);
    something::services::ApplicationCommandService commandService(&services);
    commandService.setMutationWorkflow(&mutationWorkflow);
    something::platform::SystemClock clock;
    commandService.setClock(&clock);
    something::platform::NativeLocationProvider locationProvider;
    something::platform::NativeDesktopApplicationsPlatform desktopApplicationsPlatform;
    something::platform::NativeRunningWindowsPlatform runningWindowsPlatform;
    something::platform::NativeFileExplorerPlatform fileExplorerPlatform;
    something::platform::NativeFileExplorerShell fileExplorerShell;
    something::services::FileExplorerService fileExplorerService(
        &fileExplorerPlatform, &fileExplorerShell);
    something::controllers::ApplicationController controller(&services);
    something::controllers::CursorTrailController cursorTrailController;
    something::controllers::LegacyDataImportController legacyDataImportController(&services);
    auto *wallpaperImages = new something::providers::WallpaperImageProvider(
        services.profileDirectory());
    auto *providerImages = new something::providers::ProviderImageProvider();
    auto *applicationImages = new something::providers::ApplicationIconImageProvider(
        services.profileDirectory());
    auto *setupImages = new something::providers::SetupIconImageProvider(
        services.profileDirectory());
    auto *steamArtworkImages = new something::providers::SteamArtworkImageProvider(
        services.profileDirectory());
    something::controllers::WallpaperController wallpaperController(&services,
                                                                     wallpaperImages);
    something::controllers::TasksController tasksController(&services, &clock, &mutationWorkflow);
    something::controllers::SchedulingController schedulingController(&services, &clock);
    something::controllers::TaskHistoryController taskHistoryController(&services);
    something::controllers::TerminalController terminalController(&commandService);
    something::controllers::CalendarController calendarController(&services, &clock);
    something::controllers::DashboardController dashboardController(&services, &clock);
    something::controllers::FavoritesController favoritesController(
        &services, &clock, providerImages);
    something::controllers::WeatherController weatherController(
        &services, &clock, &locationProvider);
    something::controllers::CurrencyController currencyController(&services);
    something::controllers::DesktopApplicationsController desktopApplicationsController(
        &services, &desktopApplicationsPlatform, applicationImages);
    something::controllers::RunningApplicationsController runningApplicationsController(
        &services, &runningWindowsPlatform);
    something::controllers::SettingsController settingsController(&services, &clock, &mutationWorkflow);
    something::controllers::SetupsController setupsController(
        &services, &desktopApplicationsPlatform, setupImages);
    something::controllers::FileExplorerController fileExplorerController(
        &services, &fileExplorerService, &fileExplorerShell);
    something::controllers::SteamGamesController steamGamesController(&services);
    commandService.setSnapshotProvider(QStringLiteral("settings"), [&settingsController, &wallpaperController,
                                                                     &steamGamesController] {
        QVariantMap fields;
        QVariantList unavailable;
        if (settingsController.loaded()) fields.insert(QStringLiteral("weekStartSundayZero"), settingsController.weekStart());
        else unavailable.push_back(QStringLiteral("weekStartSundayZero"));
        if (wallpaperController.catalogLoaded()) fields.insert(QStringLiteral("selectedWallpaper"), wallpaperController.selectedWallpaper());
        else unavailable.push_back(QStringLiteral("selectedWallpaper"));
        if (steamGamesController.snapshotLoaded()) fields.insert(QStringLiteral("steamCountry"), steamGamesController.countryCode());
        else unavailable.push_back(QStringLiteral("steamCountry"));
        return QVariantMap{{QStringLiteral("available"), !fields.isEmpty()},
                           {QStringLiteral("source"), QStringLiteral("retained-controller-state")},
                           {QStringLiteral("settings"), fields},
                           {QStringLiteral("unavailableFields"), unavailable}};
    });
    commandService.setSnapshotProvider(QStringLiteral("steam"), [&steamGamesController] {
        QVariantMap result;
        if (!steamGamesController.snapshotLoaded())
            return QVariantMap{{QStringLiteral("available"), false},
                               {QStringLiteral("source"), QStringLiteral("loaded-snapshot")},
                               {QStringLiteral("message"), QStringLiteral("Steam snapshot is not loaded.")}};
        QVariantList games;
        auto *model = steamGamesController.games();
        const QStringList fields{QStringLiteral("id"), QStringLiteral("steamAppId"),
            QStringLiteral("name"), QStringLiteral("priceStatus"), QStringLiteral("currency"),
            QStringLiteral("currentPriceMinor"), QStringLiteral("regularPriceMinor"),
            QStringLiteral("lastCheckedAt"), QStringLiteral("priceCountryCode")};
        for (int row = 0; row < model->count(); ++row) {
            const QVariantMap source = model->rowAt(row);
            QVariantMap safe;
            for (const QString &field : fields)
                if (source.contains(field)) safe.insert(field, source.value(field));
            games.push_back(safe);
        }
        return QVariantMap{{QStringLiteral("available"), true},
                           {QStringLiteral("source"), QStringLiteral("loaded-snapshot")},
                           {QStringLiteral("games"), games},
                           {QStringLiteral("total"), games.size()}};
    });
    QObject::connect(&steamGamesController,
                     &something::controllers::SteamGamesController::artworkInvalidated,
                     &steamGamesController,
                     [steamArtworkImages](const QString &filename) {
        steamArtworkImages->invalidate(filename);
    });
    QObject::connect(&desktopApplicationsController,
                     &something::controllers::DesktopApplicationsController::mutationFinished,
                     &setupsController, [&setupsController](bool success) {
        if (success) setupsController.reload();
    });
    favoritesController.setDashboardNewsModel(dashboardController.favoriteNews());
    QObject::connect(&tasksController, &something::controllers::TasksController::taskMutationCommitted,
                     &calendarController, &something::controllers::CalendarController::onTaskMutation);
    QObject::connect(&tasksController, &something::controllers::TasksController::taskMutationCommitted,
                     &dashboardController, &something::controllers::DashboardController::onTaskMutation);
    QObject::connect(&tasksController, &something::controllers::TasksController::taskMutationCommitted,
                     &schedulingController, &something::controllers::SchedulingController::onTaskMutationCommitted);
    QObject::connect(&tasksController, &something::controllers::TasksController::taskMutationMayHaveCommitted,
                     &calendarController, &something::controllers::CalendarController::refreshSelectedDate);
    QObject::connect(&tasksController, &something::controllers::TasksController::taskMutationMayHaveCommitted,
                     &dashboardController, &something::controllers::DashboardController::refreshTaskSummaries);
    QObject::connect(&tasksController, &something::controllers::TasksController::taskMutationMayHaveCommitted,
                     &schedulingController, &something::controllers::SchedulingController::onTaskMutationMayHaveCommitted);
    QObject::connect(&favoritesController, &something::controllers::FavoritesController::newsChanged,
                     &dashboardController, &something::controllers::DashboardController::reloadFavoriteNews);
    QObject::connect(&settingsController, &something::controllers::SettingsController::weekStartChanged,
                     &dashboardController, [&dashboardController](int weekStart) {
        dashboardController.setWeekStart(weekStart);
    });
    QObject::connect(&dashboardController, &something::controllers::DashboardController::favoriteNewsItemsChanged,
                     &favoritesController, &something::controllers::FavoritesController::loadNewsImages);

    // Keep the context objects alive until the engine has destroyed its QML tree.
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("wallpapers"), wallpaperImages);
    engine.addImageProvider(QStringLiteral("provider-media"), providerImages);
    engine.addImageProvider(QStringLiteral("application-icons"), applicationImages);
    engine.addImageProvider(QStringLiteral("setup-icons"), setupImages);
    engine.addImageProvider(QStringLiteral("steam-artwork"), steamArtworkImages);
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("routeCatalog"), QVariantMap{
        {QStringLiteral("routeTitles"), something::application::RouteCatalog::titleMap()},
        {QStringLiteral("navigationItems"), something::application::RouteCatalog::navigationItems()}});
    engine.rootContext()->setContextProperty(QStringLiteral("cursorTrailController"), &cursorTrailController);
    engine.rootContext()->setContextProperty(QStringLiteral("wallpaperController"),
                                             &wallpaperController);
    engine.rootContext()->setContextProperty(QStringLiteral("legacyDataImportController"),
                                             &legacyDataImportController);
    engine.rootContext()->setContextProperty(QStringLiteral("taskController"),
                                             &tasksController);
    engine.rootContext()->setContextProperty(QStringLiteral("schedulingController"),
                                             &schedulingController);
    engine.rootContext()->setContextProperty(QStringLiteral("taskHistoryController"),
                                             &taskHistoryController);
    engine.rootContext()->setContextProperty(QStringLiteral("terminalController"),
                                             &terminalController);
    engine.rootContext()->setContextProperty(QStringLiteral("calendarController"),
                                             &calendarController);
    engine.rootContext()->setContextProperty(QStringLiteral("dashboardController"),
                                             &dashboardController);
    engine.rootContext()->setContextProperty(QStringLiteral("favoritesController"),
                                             &favoritesController);
    engine.rootContext()->setContextProperty(QStringLiteral("weatherController"),
                                             &weatherController);
    engine.rootContext()->setContextProperty(QStringLiteral("currencyController"),
                                             &currencyController);
    engine.rootContext()->setContextProperty(QStringLiteral("desktopApplicationsController"),
                                             &desktopApplicationsController);
    engine.rootContext()->setContextProperty(QStringLiteral("runningApplicationsController"),
                                             &runningApplicationsController);
    engine.rootContext()->setContextProperty(QStringLiteral("appSettingsController"),
                                             &settingsController);
    engine.rootContext()->setContextProperty(QStringLiteral("setupsController"),
                                             &setupsController);
    engine.rootContext()->setContextProperty(QStringLiteral("fileExplorerController"),
                                             &fileExplorerController);
    engine.rootContext()->setContextProperty(QStringLiteral("steamGamesController"),
                                             &steamGamesController);
    engine.loadFromModule(QStringLiteral("Something.App"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    auto *mainWindow = qobject_cast<QWindow *>(engine.rootObjects().constFirst());
    if (!mainWindow) return 1;
    auto *consoleWindow = mainWindow->findChild<QWindow *>(QStringLiteral("consoleWindow"));
    if (!consoleWindow) return 1;
    cursorTrailController.attachToWindow(mainWindow);

    QObject::connect(&commandService, &something::services::ApplicationCommandService::fullWindowRequested,
                     &controller, &something::controllers::ApplicationController::requestFullWindow);
    QObject::connect(&controller, &something::controllers::ApplicationController::fullWindowRequested,
                     mainWindow, [mainWindow, firstShow = true]() mutable {
        if (firstShow) {
            firstShow = false;
            mainWindow->setProperty("fullUiOpened", true);
            showCenteredOnLaunch(*mainWindow);
        } else if (mainWindow->windowState() == Qt::WindowMinimized) {
            mainWindow->showNormal();
        } else {
            mainWindow->show();
        }
        mainWindow->raise();
        mainWindow->requestActivate();
    });

    QObject::connect(&app, &QGuiApplication::lastWindowClosed,
                     &controller, &something::controllers::ApplicationController::requestShutdown);
    QObject::connect(&controller, &something::controllers::ApplicationController::shutdownFinished,
                     &app, [&app] { app.quit(); });

    // Start with Console only. The full window is fitted and shown on demand.
    showCenteredOnLaunch(*consoleWindow);
    services.start();

    return app.exec();
}

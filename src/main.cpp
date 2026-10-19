#include <QGuiApplication>
#include <QIcon>
#include <QCursor>
#include <QQmlContext>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QScreen>
#include <QString>
#include <QWindow>

#include <algorithm>

#include "application/application_service_graph.h"
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
#include "controllers/settings_controller.h"
#include "controllers/setups_controller.h"
#include "controllers/steam_games_controller.h"
#include "controllers/tasks_controller.h"
#include "controllers/wallpaper_controller.h"
#include "controllers/weather_controller.h"
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
    QGuiApplication::setApplicationName(QStringLiteral("Something"));
    QGuiApplication::setOrganizationName(QStringLiteral("Something"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/something/assets/appicon.png")));
    QGuiApplication::setQuitOnLastWindowClosed(false);
    // The QML controls provide their own backgrounds/content. Native Windows
    // styles reject those customizations, so use the supported base style.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    something::application::ApplicationServiceGraph services;
    something::platform::SystemClock clock;
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
    something::controllers::TasksController tasksController(&services, &clock);
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
    something::controllers::SettingsController settingsController(&services, &clock);
    something::controllers::SetupsController setupsController(
        &services, &desktopApplicationsPlatform, setupImages);
    something::controllers::FileExplorerController fileExplorerController(
        &services, &fileExplorerService, &fileExplorerShell);
    something::controllers::SteamGamesController steamGamesController(&services);
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
    engine.rootContext()->setContextProperty(QStringLiteral("cursorTrailController"), &cursorTrailController);
    engine.rootContext()->setContextProperty(QStringLiteral("wallpaperController"),
                                             &wallpaperController);
    engine.rootContext()->setContextProperty(QStringLiteral("legacyDataImportController"),
                                             &legacyDataImportController);
    engine.rootContext()->setContextProperty(QStringLiteral("taskController"),
                                             &tasksController);
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
    cursorTrailController.attachToWindow(mainWindow);

    QObject::connect(&app, &QGuiApplication::lastWindowClosed,
                     &controller, &something::controllers::ApplicationController::requestShutdown);
    QObject::connect(&controller, &something::controllers::ApplicationController::shutdownFinished,
                     &app, [&app] { app.quit(); });

    showCenteredOnLaunch(*mainWindow);
    services.start();

    return app.exec();
}

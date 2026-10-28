cmake_minimum_required(VERSION 3.21)

project(SomethingQt
    VERSION 0.1.0
    DESCRIPTION "Independent C++ and Qt Quick rewrite of Something"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Qml
    Quick
    QuickControls2
    QuickDialogs2
    Network
    Sql
    OPTIONAL_COMPONENTS Positioning
)

# Telegram/YouTube use structured HTML parsing only. Pin the dependency to an
# upstream release and keep its tests, examples, utilities and shared library
# out of the application build.
include(FetchContent)
set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_TESTS_CPP OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_WASM OFF CACHE BOOL "" FORCE)
set(LEXBOR_INSTALL_HEADERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lexbor
    GIT_REPOSITORY https://github.com/lexbor/lexbor.git
    GIT_TAG v3.0.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lexbor)
# Build the linked static target, but omit the dependency's SDK install rules
# from the application package. Runtime notices are installed explicitly below.
set_property(DIRECTORY "${lexbor_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)

# Qt Gui provides QImage/QImageReader; available formats depend on the Qt
# build's built-in handlers and deployed image-format plugins.

qt_standard_project_setup(REQUIRES 6.5)

# QML consumers import Something.App explicitly for Theme. Relative directory
# imports cannot see this singleton on the Qt 6.5 floor (before QTP0004).
set_source_files_properties(qml/theme/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(SomethingQt WIN32
    src/main.cpp
    src/application/application_lifecycle.cpp
    src/application/application_service_graph.cpp
    src/application/command_parser.cpp
    src/application/command_completion.cpp
    src/application/command_registry.cpp
    src/application/route_catalog.cpp
    src/application/capability_registry.cpp
    src/application/operation_coordinator.cpp
    src/controllers/application_controller.cpp
    src/controllers/calendar_controller.cpp
    src/controllers/currency_controller.cpp
    src/controllers/cursor_trail_controller.cpp
    src/controllers/dashboard_controller.cpp
    src/controllers/desktop_applications_controller.cpp
    src/controllers/file_explorer_controller.cpp
    src/controllers/legacy_data_import_controller.cpp
    src/controllers/setups_controller.cpp
    src/controllers/steam_games_controller.cpp
    src/controllers/favorites_controller.cpp
    src/controllers/running_applications_controller.cpp
    src/controllers/settings_controller.cpp
    src/controllers/tasks_controller.cpp
    src/controllers/terminal_controller.cpp
    src/controllers/wallpaper_controller.cpp
    src/controllers/weather_controller.cpp
    src/domain/app_error.cpp
    src/domain/desktop_application_rules.cpp
    src/domain/favorite_rules.cpp
    src/domain/task_date_rules.cpp
    src/domain/task_rules.cpp
    src/domain/steam_game.cpp
    src/domain/weather_currency.cpp
    src/platform/profile_instance_lock.cpp
    src/platform/file_explorer_platform.cpp
    src/platform/desktop_applications_platform.cpp
    src/platform/location_provider.cpp
    src/platform/system_clock.cpp
    src/providers/network_worker.cpp
    src/providers/provider_policy.cpp
    src/providers/provider_response_cache.cpp
    src/providers/provider_parsers.cpp
    src/providers/feature_parsers.cpp
    src/providers/provider_image_provider.cpp
    src/providers/wallpaper_image_provider.cpp
    src/providers/application_icon_image_provider.cpp
    src/providers/setup_icon_image_provider.cpp
    src/providers/steam_artwork_image_provider.cpp
    src/services/bounded_worker_pool.cpp
    src/services/dashboard_service.cpp
    src/services/desktop_app_icon_store.cpp
    src/services/desktop_applications_service.cpp
    src/services/file_explorer_service.cpp
    src/services/legacy_data_import_service.cpp
    src/services/setup_icon_store.cpp
    src/services/setups_service.cpp
    src/services/steam_artwork_store.cpp
    src/services/steam_service.cpp
    src/services/currency_service.cpp
    src/services/favorites_service.cpp
    src/services/task_service.cpp
    src/services/application_command_service.cpp
    src/services/application_mutation_workflow.cpp
    src/services/preferences_service.cpp
    src/services/weather_service.cpp
    src/services/wallpaper_asset_store.cpp
    src/services/wallpaper_service.cpp
    src/storage/repositories.cpp
    src/storage/data_import_repository.cpp
    src/storage/schema_migrator.cpp
    src/storage/storage_worker.cpp
    src/models/tasks_model.cpp
    src/models/terminal_output_model.cpp
    src/models/calendar_months_model.cpp
    src/models/currency_rates_model.cpp
    src/models/dashboard_news_model.cpp
    src/models/desktop_applications_model.cpp
    src/models/file_explorer_model.cpp
    src/models/setups_model.cpp
    src/models/provider_items_model.cpp
    src/models/running_windows_model.cpp
    src/models/weather_forecast_model.cpp
    src/models/steam_games_model.cpp
)

qt_add_qml_module(SomethingQt
    URI Something.App
    VERSION 1.0
    QML_FILES
        qml/shell/Main.qml
        qml/components/AppButton.qml
        qml/components/RemoveFavoriteButton.qml
        qml/components/AppTextField.qml
        qml/components/AppCard.qml
        qml/components/CursorTrail.qml
        qml/components/DashboardNewsCard.qml
        qml/components/DashboardRightRail.qml
        qml/components/DashboardLeftRail.qml
        qml/components/AppComboBox.qml
        qml/components/StatusBadge.qml
        qml/components/FilterChip.qml
        qml/components/StatePanel.qml
        qml/components/NotificationToast.qml
        qml/components/WeatherSummaryCard.qml
        qml/components/OrbitNavigation.qml
        qml/dialogs/ConfirmationDialog.qml
        qml/dialogs/TaskEditorDialog.qml
        qml/components/TaskSummaryCard.qml
        qml/pages/CalendarPage.qml
        qml/pages/CurrenciesPage.qml
        qml/pages/DesktopApplicationsPage.qml
        qml/pages/DashboardPage.qml
        qml/pages/FavoritesPage.qml
        qml/pages/FileExplorerPage.qml
        qml/pages/RunningApplicationsPage.qml
        qml/pages/SetupsPage.qml
        qml/pages/SteamGamesPage.qml
        qml/pages/TasksPage.qml
        qml/pages/TerminalPage.qml
        qml/pages/WeatherPage.qml
        qml/pages/WallpaperSettingsPage.qml
        qml/theme/Theme.qml
)

qt_add_resources(SomethingQt something_assets
    PREFIX "/something"
    FILES
        assets/appicon.png
        assets/icons/folder.svg
        assets/icons/file.svg
        assets/icons/link.svg
        assets/icons/circle-mask.svg
        assets/icons/cursor-ring.svg
        assets/icons/trash.svg
        assets/icons/trash-active.svg
        assets/icons/cursor-glow.svg
        assets/wallpapers/sandrone.jpg
        assets/wallpapers/hu-tao.jpg
        assets/wallpapers/skirk.jpg
        assets/navigation/apps-furina.png
        assets/navigation/currency-zhongli.png
        assets/navigation/file-explorer-aino.png
        assets/navigation/games-controller.svg
        assets/navigation/settings-sandrone.png
        assets/navigation/setups-layers.svg
        assets/navigation/telegram-varka.png
        assets/navigation/todo-yelan.png
        assets/navigation/weather-lisa.png
        assets/navigation/youtube-charlotte.png
)

target_link_libraries(SomethingQt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    Qt6::Network
    Qt6::Sql
    lexbor_static
)

if(TARGET Qt6::Positioning)
    target_link_libraries(SomethingQt PRIVATE Qt6::Positioning)
    target_compile_definitions(SomethingQt PRIVATE SOMETHING_QT_HAS_POSITIONING=1)
endif()

if(WIN32)
    configure_file(src/platform/windows_app.rc.in windows_app.rc @ONLY)
    target_sources(SomethingQt PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/windows_app.rc")
    target_sources(SomethingQt PRIVATE src/platform/windows_app.manifest)
    target_link_libraries(SomethingQt PRIVATE dwmapi shell32 user32 gdi32 ole32 uuid)

    # Install-time Qt deployment uses the QML-aware Qt CMake helper. Keep the
    # executable and helper output in the standard relocatable install tree.
    # Qt 6.7 added DEPLOY_TOOL_OPTIONS; Qt 6.10 added direct plugin selection.
    # Requiring the explicit plugin list prevents dynamic SQL/image/TLS plugins
    # from being silently omitted from a Windows deployment.
    include(GNUInstallDirs)
    option(SOMETHING_QT_ENABLE_WINDOWS_DEPLOYMENT
        "Deploy Qt runtime, QML imports, required plugins, and compiler runtime during install"
        ON
    )

    if(SOMETHING_QT_ENABLE_WINDOWS_DEPLOYMENT)
        if(Qt6_VERSION VERSION_LESS 6.7)
            message(FATAL_ERROR
                "Windows deployment requires Qt 6.7 or newer so required runtime plugins can be explicitly included. "
                "The application itself retains a Qt 6.5 source compatibility floor."
            )
        endif()

        install(TARGETS SomethingQt
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        )

        if(Qt6_VERSION VERSION_GREATER_EQUAL 6.10)
            qt_generate_deploy_qml_app_script(
                TARGET SomethingQt
                OUTPUT_SCRIPT something_qt_deploy_script
                INCLUDE_PLUGINS
                    qwindows
                    qsqlite
                    qgif
                    qico
                    qjpeg
                    qsvg
                    qschannelbackend
                EXCLUDE_PLUGIN_TYPES qmltooling
                EXCLUDE_PLUGINS qsqlibase qsqlmimer qsqloci qsqlodbc qsqlpsql
            )
        else()
            # Qt 6.7-6.9 expose plugin selection through windeployqt options.
            qt_generate_deploy_qml_app_script(
                TARGET SomethingQt
                OUTPUT_SCRIPT something_qt_deploy_script
                DEPLOY_TOOL_OPTIONS
                    --include-plugins
                    qwindows,qsqlite,qgif,qico,qjpeg,qsvg,qschannelbackend
            )
        endif()
        install(SCRIPT "${something_qt_deploy_script}")

        # Lexbor is linked statically; only its upstream license notices are
        # runtime-distribution files. Qt deployment tools do not collect these.
        install(FILES
            "${lexbor_SOURCE_DIR}/LICENSE"
            "${lexbor_SOURCE_DIR}/NOTICE"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/SomethingQt/Lexbor"
        )
    endif()
endif()

option(SOMETHING_QT_BUILD_TESTS "Configure test-only targets and dependencies" OFF)
if(SOMETHING_QT_BUILD_TESTS)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    enable_testing()
    add_subdirectory(tests)
endif()

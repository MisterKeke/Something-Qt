add_executable(SomethingQtApplicationCommandTests
    application_command_test.cpp
    ../src/controllers/tasks_controller.cpp
    ../src/controllers/terminal_controller.cpp
    ../src/models/tasks_model.cpp
    ../src/models/terminal_output_model.cpp
    ../src/application/application_lifecycle.cpp
    ../src/application/application_service_graph.cpp
    ../src/application/capability_registry.cpp
    ../src/application/command_parser.cpp
    ../src/application/command_completion.cpp
    ../src/application/command_registry.cpp
    ../src/application/operation_coordinator.cpp
    ../src/domain/app_error.cpp
    ../src/domain/task_date_rules.cpp
    ../src/domain/task_rules.cpp
    ../src/platform/profile_instance_lock.cpp
    ../src/providers/network_worker.cpp
    ../src/providers/provider_policy.cpp
    ../src/providers/provider_response_cache.cpp
    ../src/services/application_command_service.cpp
    ../src/services/application_mutation_workflow.cpp
    ../src/services/preferences_service.cpp
    ../src/services/bounded_worker_pool.cpp
    ../src/services/task_service.cpp
    ../src/services/wallpaper_service.cpp
    ../src/services/wallpaper_asset_store.cpp
    ../src/services/favorites_service.cpp
    ../src/services/desktop_applications_service.cpp
    ../src/services/setups_service.cpp
    ../src/services/setup_icon_store.cpp
    ../src/domain/favorite_rules.cpp
    ../src/domain/desktop_application_rules.cpp
    ../src/providers/provider_parsers.cpp
    ../src/application/route_catalog.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
    ../src/storage/storage_worker.cpp
)
target_include_directories(SomethingQtApplicationCommandTests PRIVATE ../src)
target_link_libraries(SomethingQtApplicationCommandTests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql Qt6::Test lexbor_static)
add_test(NAME SomethingQtApplicationCommandTests COMMAND SomethingQtApplicationCommandTests
    -o "${CMAKE_CURRENT_BINARY_DIR}/application-command-results.txt,txt")

add_executable(SomethingQtTerminalOutputTests
    terminal_output_model_test.cpp
    ../src/models/terminal_output_model.cpp
)
target_include_directories(SomethingQtTerminalOutputTests PRIVATE ../src)
target_link_libraries(SomethingQtTerminalOutputTests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME SomethingQtTerminalOutputTests COMMAND SomethingQtTerminalOutputTests
    -o "${CMAKE_CURRENT_BINARY_DIR}/terminal-output-results.txt,txt")

add_executable(SomethingQtConsolePhaseThreeTests
    console_phase_three_test.cpp
    ../src/application/application_lifecycle.cpp
    ../src/application/application_service_graph.cpp
    ../src/application/capability_registry.cpp
    ../src/application/command_parser.cpp
    ../src/application/command_completion.cpp
    ../src/application/command_registry.cpp
    ../src/application/operation_coordinator.cpp
    ../src/application/route_catalog.cpp
    ../src/domain/app_error.cpp
    ../src/domain/desktop_application_rules.cpp
    ../src/domain/favorite_rules.cpp
    ../src/domain/task_date_rules.cpp
    ../src/domain/task_rules.cpp
    ../src/platform/profile_instance_lock.cpp
    ../src/providers/network_worker.cpp
    ../src/providers/provider_policy.cpp
    ../src/providers/provider_response_cache.cpp
    ../src/providers/provider_parsers.cpp
    ../src/services/application_command_service.cpp
    ../src/services/application_mutation_workflow.cpp
    ../src/services/preferences_service.cpp
    ../src/services/bounded_worker_pool.cpp
    ../src/services/task_service.cpp
    ../src/services/wallpaper_service.cpp
    ../src/services/wallpaper_asset_store.cpp
    ../src/services/favorites_service.cpp
    ../src/services/desktop_applications_service.cpp
    ../src/services/setups_service.cpp
    ../src/services/setup_icon_store.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
    ../src/storage/storage_worker.cpp
)
target_include_directories(SomethingQtConsolePhaseThreeTests PRIVATE ../src)
target_link_libraries(SomethingQtConsolePhaseThreeTests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql Qt6::Test lexbor_static)
add_test(NAME SomethingQtConsolePhaseThreeTests COMMAND SomethingQtConsolePhaseThreeTests
    -o "${CMAKE_CURRENT_BINARY_DIR}/console-phase-three-results.txt,txt")
set_tests_properties(SomethingQtApplicationCommandTests SomethingQtTerminalOutputTests
    SomethingQtConsolePhaseThreeTests PROPERTIES TIMEOUT 60)

add_executable(SomethingQtLifecycleTests
    lifecycle_error_test.cpp
    ../src/application/application_lifecycle.cpp
    ../src/application/application_service_graph.cpp
    ../src/application/capability_registry.cpp
    ../src/application/operation_coordinator.cpp
    ../src/domain/app_error.cpp
    ../src/domain/task_rules.cpp
    ../src/platform/profile_instance_lock.cpp
    ../src/providers/network_worker.cpp
    ../src/providers/provider_policy.cpp
    ../src/providers/provider_response_cache.cpp
    ../src/services/bounded_worker_pool.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
    ../src/storage/storage_worker.cpp
)

target_include_directories(SomethingQtLifecycleTests PRIVATE
    ../src
)

target_link_libraries(SomethingQtLifecycleTests PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Test
)

add_test(NAME SomethingQtLifecycleTests COMMAND SomethingQtLifecycleTests)

add_executable(SomethingQtStorageTests
    storage_repository_test.cpp
    ../src/domain/app_error.cpp
    ../src/domain/task_rules.cpp
    ../src/services/preferences_service.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
)

target_include_directories(SomethingQtStorageTests PRIVATE
    ../src
)

target_link_libraries(SomethingQtStorageTests PRIVATE
    Qt6::Core
    Qt6::Sql
    Qt6::Test
)

add_test(NAME SomethingQtStorageTests COMMAND SomethingQtStorageTests)

add_executable(SomethingQtTaskHistoryTests
    task_history_test.cpp
    ../src/domain/app_error.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_rules.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
)
target_include_directories(SomethingQtTaskHistoryTests PRIVATE ../src)
target_link_libraries(SomethingQtTaskHistoryTests PRIVATE
    Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME SomethingQtTaskHistoryTests COMMAND SomethingQtTaskHistoryTests)
set_tests_properties(SomethingQtTaskHistoryTests PROPERTIES TIMEOUT 60)

add_executable(SomethingQtTaskDomainTests
    task_domain_test.cpp
    ../src/domain/app_error.cpp
    ../src/domain/task_rules.cpp
)
target_include_directories(SomethingQtTaskDomainTests PRIVATE ../src)
target_link_libraries(SomethingQtTaskDomainTests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME SomethingQtTaskDomainTests COMMAND SomethingQtTaskDomainTests)

add_executable(SomethingQtSchedulingTests
    scheduling_test.cpp
    ../src/domain/app_error.cpp
    ../src/domain/scheduling_engine.cpp
    ../src/domain/task_scheduling.cpp
)
target_include_directories(SomethingQtSchedulingTests PRIVATE ../src)
target_link_libraries(SomethingQtSchedulingTests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME SomethingQtSchedulingTests COMMAND SomethingQtSchedulingTests)

add_executable(SomethingQtTaskModelTests
    task_model_test.cpp
    ../src/models/tasks_model.cpp
)
target_include_directories(SomethingQtTaskModelTests PRIVATE ../src)
target_link_libraries(SomethingQtTaskModelTests PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
add_test(NAME SomethingQtTaskModelTests COMMAND SomethingQtTaskModelTests)

add_executable(SomethingQtCalendarDashboardTests
    calendar_dashboard_test.cpp
    ../src/domain/app_error.cpp
    ../src/domain/task_date_rules.cpp
    ../src/domain/task_rules.cpp
    ../src/models/calendar_months_model.cpp
    ../src/models/tasks_model.cpp
    ../src/services/task_service.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
)
target_include_directories(SomethingQtCalendarDashboardTests PRIVATE ../src)
target_link_libraries(SomethingQtCalendarDashboardTests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    Qt6::Test
)
add_test(NAME SomethingQtCalendarDashboardTests COMMAND SomethingQtCalendarDashboardTests)

add_executable(SomethingQtProviderInfrastructureTests
    provider_infrastructure_test.cpp
    provider_fixtures.qrc
    ../src/application/operation_coordinator.cpp
    ../src/domain/app_error.cpp
    ../src/providers/network_worker.cpp
    ../src/providers/provider_policy.cpp
    ../src/providers/provider_response_cache.cpp
    ../src/providers/provider_parsers.cpp
)
target_include_directories(SomethingQtProviderInfrastructureTests PRIVATE ../src)
target_link_libraries(SomethingQtProviderInfrastructureTests PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
    lexbor_static
)
add_test(NAME SomethingQtProviderInfrastructureTests COMMAND SomethingQtProviderInfrastructureTests)

add_executable(SomethingQtFavoritesFeatureTests
    favorites_feature_test.cpp
    provider_fixtures.qrc
    ../src/domain/app_error.cpp
    ../src/domain/favorite_rules.cpp
    ../src/domain/task_rules.cpp
    ../src/providers/provider_parsers.cpp
    ../src/providers/provider_policy.cpp
    ../src/services/favorites_service.cpp
    ../src/models/provider_items_model.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
)
target_include_directories(SomethingQtFavoritesFeatureTests PRIVATE ../src)
target_link_libraries(SomethingQtFavoritesFeatureTests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    Qt6::Test
    lexbor_static
)
add_test(NAME SomethingQtFavoritesFeatureTests COMMAND SomethingQtFavoritesFeatureTests)

add_executable(SomethingQtWeatherCurrencyTests
    weather_currency_feature_test.cpp
    provider_fixtures.qrc
    ../src/application/application_lifecycle.cpp
    ../src/application/application_service_graph.cpp
    ../src/application/capability_registry.cpp
    ../src/application/operation_coordinator.cpp
    ../src/controllers/weather_controller.cpp
    ../src/domain/app_error.cpp
    ../src/domain/task_rules.cpp
    ../src/domain/weather_currency.cpp
    ../src/models/currency_rates_model.cpp
    ../src/models/weather_comparison_model.cpp
    ../src/models/weather_forecast_model.cpp
    ../src/platform/location_provider.cpp
    ../src/platform/profile_instance_lock.cpp
    ../src/providers/feature_parsers.cpp
    ../src/providers/network_worker.cpp
    ../src/providers/provider_policy.cpp
    ../src/providers/provider_response_cache.cpp
    ../src/services/bounded_worker_pool.cpp
    ../src/services/currency_service.cpp
    ../src/services/weather_service.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
    ../src/storage/storage_worker.cpp
)
target_include_directories(SomethingQtWeatherCurrencyTests PRIVATE ../src)
target_link_libraries(SomethingQtWeatherCurrencyTests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Sql
    Qt6::Test
)
add_test(NAME SomethingQtWeatherCurrencyTests COMMAND SomethingQtWeatherCurrencyTests)

add_executable(SomethingQtDesktopApplicationsTests
    desktop_applications_test.cpp
    ../src/domain/desktop_application_rules.cpp
    ../src/models/desktop_applications_model.cpp
    ../src/models/running_windows_model.cpp
    ../src/services/desktop_app_icon_store.cpp
)
target_include_directories(SomethingQtDesktopApplicationsTests PRIVATE ../src)
target_link_libraries(SomethingQtDesktopApplicationsTests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)
add_test(NAME SomethingQtDesktopApplicationsTests COMMAND SomethingQtDesktopApplicationsTests)

add_executable(SomethingQtDesktopApplicationsControllerTests
    desktop_applications_controller_test.cpp
    ../src/application/application_lifecycle.cpp
    ../src/application/application_service_graph.cpp
    ../src/application/capability_registry.cpp
    ../src/application/operation_coordinator.cpp
    ../src/controllers/desktop_applications_controller.cpp
    ../src/domain/app_error.cpp
    ../src/domain/desktop_application_rules.cpp
    ../src/domain/task_rules.cpp
    ../src/models/desktop_applications_model.cpp
    ../src/platform/profile_instance_lock.cpp
    ../src/providers/network_worker.cpp
    ../src/providers/provider_policy.cpp
    ../src/providers/provider_response_cache.cpp
    ../src/providers/application_icon_image_provider.cpp
    ../src/services/bounded_worker_pool.cpp
    ../src/services/desktop_app_icon_store.cpp
    ../src/services/desktop_applications_service.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
    ../src/storage/storage_worker.cpp
)
target_include_directories(SomethingQtDesktopApplicationsControllerTests PRIVATE ../src)
target_link_libraries(SomethingQtDesktopApplicationsControllerTests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Quick Qt6::Sql Qt6::Test)
add_test(NAME SomethingQtDesktopApplicationsControllerTests COMMAND SomethingQtDesktopApplicationsControllerTests)

add_executable(SomethingQtRunningApplicationsControllerTests
    running_applications_controller_test.cpp
    ../src/application/application_lifecycle.cpp
    ../src/application/application_service_graph.cpp
    ../src/application/capability_registry.cpp
    ../src/application/operation_coordinator.cpp
    ../src/controllers/running_applications_controller.cpp
    ../src/domain/app_error.cpp
    ../src/domain/task_rules.cpp
    ../src/models/running_windows_model.cpp
    ../src/platform/profile_instance_lock.cpp
    ../src/providers/network_worker.cpp
    ../src/providers/provider_policy.cpp
    ../src/providers/provider_response_cache.cpp
    ../src/services/bounded_worker_pool.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
    ../src/storage/storage_worker.cpp
)
target_include_directories(SomethingQtRunningApplicationsControllerTests PRIVATE ../src)
target_link_libraries(SomethingQtRunningApplicationsControllerTests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Quick Qt6::Sql Qt6::Test)
add_test(NAME SomethingQtRunningApplicationsControllerTests
         COMMAND SomethingQtRunningApplicationsControllerTests)

add_executable(SomethingQtSetupsFeatureTests
    setups_feature_test.cpp
    ../src/domain/app_error.cpp
    ../src/domain/desktop_application_rules.cpp
    ../src/domain/task_rules.cpp
    ../src/models/setups_model.cpp
    ../src/services/desktop_applications_service.cpp
    ../src/services/setup_icon_store.cpp
    ../src/services/setups_service.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
)
target_include_directories(SomethingQtSetupsFeatureTests PRIVATE ../src)
target_link_libraries(SomethingQtSetupsFeatureTests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Sql Qt6::Test Qt6::Quick)
add_test(NAME SomethingQtSetupsFeatureTests COMMAND SomethingQtSetupsFeatureTests)

add_executable(SomethingQtSetupsControllerTests
    setups_controller_test.cpp
    ../src/application/application_lifecycle.cpp
    ../src/application/application_service_graph.cpp
    ../src/application/capability_registry.cpp
    ../src/application/operation_coordinator.cpp
    ../src/controllers/setups_controller.cpp
    ../src/domain/app_error.cpp
    ../src/domain/desktop_application_rules.cpp
    ../src/domain/task_rules.cpp
    ../src/models/setups_model.cpp
    ../src/platform/profile_instance_lock.cpp
    ../src/providers/network_worker.cpp
    ../src/providers/provider_policy.cpp
    ../src/providers/provider_response_cache.cpp
    ../src/providers/setup_icon_image_provider.cpp
    ../src/services/bounded_worker_pool.cpp
    ../src/services/desktop_applications_service.cpp
    ../src/services/setup_icon_store.cpp
    ../src/services/setups_service.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
    ../src/storage/storage_worker.cpp
)
target_include_directories(SomethingQtSetupsControllerTests PRIVATE ../src)
target_link_libraries(SomethingQtSetupsControllerTests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Quick Qt6::Sql Qt6::Test)
add_test(NAME SomethingQtSetupsControllerTests COMMAND SomethingQtSetupsControllerTests)

add_executable(SomethingQtFileExplorerTests
    file_explorer_test.cpp
    ../src/domain/app_error.cpp
    ../src/platform/file_explorer_platform.cpp
    ../src/services/file_explorer_service.cpp
)
target_include_directories(SomethingQtFileExplorerTests PRIVATE ../src)
target_link_libraries(SomethingQtFileExplorerTests PRIVATE Qt6::Core Qt6::Test)
if(WIN32)
    target_sources(SomethingQtFileExplorerTests PRIVATE ../src/platform/windows_app.manifest)
    target_link_libraries(SomethingQtFileExplorerTests PRIVATE shell32 ole32 uuid)
endif()
add_test(NAME SomethingQtFileExplorerTests COMMAND SomethingQtFileExplorerTests)

add_executable(SomethingQtSteamFeatureTests
    steam_feature_test.cpp
    provider_fixtures.qrc
    ../src/domain/app_error.cpp
    ../src/domain/task_date_rules.cpp
    ../src/domain/task_rules.cpp
    ../src/domain/steam_game.cpp
    ../src/domain/steam_release.cpp
    ../src/domain/weather_currency.cpp
    ../src/models/steam_games_model.cpp
    ../src/providers/feature_parsers.cpp
    ../src/providers/provider_policy.cpp
    ../src/providers/steam_release_provider.cpp
    ../src/providers/steam_artwork_image_provider.cpp
    ../src/services/calendar_service.cpp
    ../src/services/steam_artwork_store.cpp
    ../src/services/steam_release_service.cpp
    ../src/services/steam_service.cpp
    ../src/services/task_service.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
)
target_include_directories(SomethingQtSteamFeatureTests PRIVATE ../src)
target_link_libraries(SomethingQtSteamFeatureTests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Sql Qt6::Test)
add_test(NAME SomethingQtSteamFeatureTests COMMAND SomethingQtSteamFeatureTests)

add_executable(SomethingQtGameUpdatesTests
    game_updates_feature_test.cpp
    ../src/application/application_lifecycle.cpp
    ../src/application/application_service_graph.cpp
    ../src/application/capability_registry.cpp
    ../src/application/operation_coordinator.cpp
    ../src/controllers/steam_game_updates_controller.cpp
    ../src/domain/app_error.cpp
    ../src/domain/task_date_rules.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_rules.cpp
    ../src/domain/task_scheduling.cpp
    ../src/domain/steam_game.cpp
    ../src/domain/steam_game_updates.cpp
    ../src/domain/steam_release.cpp
    ../src/domain/weather_currency.cpp
    ../src/platform/profile_instance_lock.cpp
    ../src/providers/feature_parsers.cpp
    ../src/providers/network_worker.cpp
    ../src/providers/provider_policy.cpp
    ../src/providers/provider_response_cache.cpp
    ../src/providers/steam_news_parser.cpp
    ../src/providers/steam_news_provider.cpp
    ../src/providers/steam_release_provider.cpp
    ../src/services/bounded_worker_pool.cpp
    ../src/services/steam_artwork_store.cpp
    ../src/services/steam_game_updates_service.cpp
    ../src/storage/repositories.cpp
    ../src/storage/schema_migrator.cpp
    ../src/storage/storage_worker.cpp
)
target_include_directories(SomethingQtGameUpdatesTests PRIVATE ../src)
target_link_libraries(SomethingQtGameUpdatesTests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql Qt6::Test lexbor_static)
add_test(NAME SomethingQtGameUpdatesTests COMMAND SomethingQtGameUpdatesTests)

add_executable(SomethingQtDataImportTests
    data_import_test.cpp
    ../src/application/application_lifecycle.cpp
    ../src/application/application_service_graph.cpp
    ../src/application/capability_registry.cpp
    ../src/application/operation_coordinator.cpp
    ../src/controllers/legacy_data_import_controller.cpp
    ../src/platform/profile_instance_lock.cpp
    ../src/providers/network_worker.cpp
    ../src/providers/provider_response_cache.cpp
    ../src/services/bounded_worker_pool.cpp
    ../src/domain/task_history.cpp
    ../src/domain/task_scheduling.cpp
    ../src/storage/repositories.cpp
    ../src/storage/storage_worker.cpp
    ../src/domain/app_error.cpp
    ../src/domain/favorite_rules.cpp
    ../src/domain/steam_game.cpp
    ../src/domain/task_rules.cpp
    ../src/domain/desktop_application_rules.cpp
    ../src/domain/weather_currency.cpp
    ../src/providers/feature_parsers.cpp
    ../src/providers/provider_policy.cpp
    ../src/services/legacy_data_import_service.cpp
    ../src/providers/wallpaper_image_provider.cpp
    ../src/providers/application_icon_image_provider.cpp
    ../src/providers/setup_icon_image_provider.cpp
    ../src/providers/steam_artwork_image_provider.cpp
    ../src/storage/data_import_repository.cpp
    ../src/storage/schema_migrator.cpp
)
target_include_directories(SomethingQtDataImportTests PRIVATE ../src)
target_link_libraries(SomethingQtDataImportTests PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Network Qt6::Sql Qt6::Test)
add_test(NAME SomethingQtDataImportTests COMMAND SomethingQtDataImportTests
    -o "${CMAKE_CURRENT_BINARY_DIR}/data-import-results.txt,txt")

#include "schema_migrator.h"

#include <QSqlError>
#include <QSqlDriver>
#include <QSqlQuery>
#include <QSet>
#include <QThread>
#include <QVariant>

#include <array>
#include <iterator>

namespace something::storage {
namespace {

struct Migration {
    int version;
    const char *name;
    const char *const *statements;
    qsizetype statementCount;
};

constexpr const char *core[] = {
    R"SQL(CREATE TABLE tasks (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        title TEXT NOT NULL CHECK(length(trim(title)) > 0),
        description TEXT NOT NULL DEFAULT '',
        is_completed INTEGER NOT NULL DEFAULT 0 CHECK(is_completed IN (0, 1)),
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        due_date TEXT,
        priority TEXT NOT NULL DEFAULT 'medium' CHECK(priority IN ('low', 'medium', 'high')),
        difficulty TEXT CHECK(difficulty IS NULL OR difficulty IN ('easy', 'medium', 'hard')),
        revision INTEGER NOT NULL DEFAULT 1 CHECK(revision > 0),
        CHECK(due_date IS NULL OR (length(due_date) = 10 AND date(due_date) = due_date))
    ))SQL",
    "CREATE INDEX idx_tasks_due_completion_priority_created ON tasks(due_date, is_completed, priority, created_at, id)",
    "CREATE INDEX idx_tasks_updated_id ON tasks(updated_at DESC, id DESC)",
    R"SQL(CREATE TABLE tags (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL CHECK(length(trim(name)) BETWEEN 1 AND 64),
        name_normalized TEXT NOT NULL UNIQUE
    ))SQL",
    R"SQL(CREATE TABLE task_tags (
        task_id INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
        tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
        PRIMARY KEY(task_id, tag_id)
    ))SQL",
    "CREATE INDEX idx_task_tags_tag ON task_tags(tag_id, task_id)",
    R"SQL(CREATE TABLE task_subtasks (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        task_id INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
        title TEXT NOT NULL CHECK(length(trim(title)) > 0),
        is_completed INTEGER NOT NULL DEFAULT 0 CHECK(is_completed IN (0, 1)),
        position INTEGER NOT NULL DEFAULT 0 CHECK(position >= 0),
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    ))SQL",
    "CREATE INDEX idx_task_subtasks_order ON task_subtasks(task_id, position, id)",
    R"SQL(CREATE TABLE currency_favorites (
        base_code TEXT NOT NULL,
        quote_code TEXT NOT NULL,
        PRIMARY KEY(base_code, quote_code),
        CHECK(length(base_code) = 3 AND base_code GLOB '[A-Z][A-Z][A-Z]'),
        CHECK(length(quote_code) = 3 AND quote_code GLOB '[A-Z][A-Z][A-Z]')
    ))SQL",
    "CREATE TABLE preferences (key TEXT PRIMARY KEY, value TEXT NOT NULL)",
};

constexpr const char *favorites[] = {
    R"SQL(CREATE TABLE favorite_categories (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL CHECK(length(trim(name)) BETWEEN 1 AND 80),
        name_normalized TEXT NOT NULL,
        source TEXT NOT NULL CHECK(source IN ('telegram', 'youtube')),
        color TEXT,
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        display_order INTEGER NOT NULL DEFAULT 0,
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        UNIQUE(source, name_normalized)
    ))SQL",
    "CREATE INDEX idx_favorite_categories_order ON favorite_categories(source, display_order, id)",
    R"SQL(CREATE TABLE telegram_favorites (
        username TEXT PRIMARY KEY COLLATE NOCASE,
        added_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        category_id INTEGER REFERENCES favorite_categories(id) ON DELETE SET NULL
    ))SQL",
    "CREATE INDEX idx_telegram_favorites_category ON telegram_favorites(category_id, username)",
    "CREATE INDEX idx_telegram_favorites_order ON telegram_favorites(added_at DESC, username COLLATE NOCASE)",
    R"SQL(CREATE TABLE youtube_favorites (
        channel_id TEXT PRIMARY KEY,
        added_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        category_id INTEGER REFERENCES favorite_categories(id) ON DELETE SET NULL,
        username TEXT
    ))SQL",
    "CREATE INDEX idx_youtube_favorites_category ON youtube_favorites(category_id, channel_id)",
    "CREATE INDEX idx_youtube_favorites_order ON youtube_favorites(added_at DESC, channel_id)",
    R"SQL(CREATE TABLE favorite_update_checkpoints (
        source TEXT NOT NULL CHECK(source IN ('telegram', 'youtube')),
        source_id TEXT NOT NULL,
        checked_through TEXT NOT NULL,
        last_success_at TEXT,
        last_attempted_at TEXT,
        last_error TEXT,
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        PRIMARY KEY(source, source_id)
    ))SQL",
    R"SQL(CREATE TABLE favorite_update_seen_items (
        source TEXT NOT NULL CHECK(source IN ('telegram', 'youtube')),
        source_id TEXT NOT NULL,
        item_id TEXT NOT NULL,
        published_at TEXT NOT NULL,
        first_seen_at TEXT NOT NULL,
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        PRIMARY KEY(source, source_id, item_id)
    ))SQL",
    "CREATE INDEX idx_favorite_seen_source_published ON favorite_update_seen_items(source, source_id, published_at, item_id)",
    R"SQL(CREATE TABLE favorite_news_items (
        source TEXT NOT NULL CHECK(source IN ('telegram', 'youtube')),
        source_id TEXT NOT NULL,
        item_id TEXT NOT NULL,
        published_at TEXT NOT NULL,
        discovered_at TEXT NOT NULL,
        payload_json TEXT NOT NULL,
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        PRIMARY KEY(source, source_id, item_id)
    ))SQL",
    "CREATE INDEX idx_favorite_news_discovered ON favorite_news_items(discovered_at, published_at, source, source_id, item_id)",
    R"SQL(CREATE TABLE favorite_news_state (
        id INTEGER PRIMARY KEY CHECK(id = 1),
        scan_started_at TEXT NOT NULL,
        errors_json TEXT NOT NULL DEFAULT '[]',
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    ))SQL",
};

constexpr const char *weather[] = {
    R"SQL(CREATE TABLE saved_weather (
        id INTEGER PRIMARY KEY CHECK(id = 1),
        latitude REAL NOT NULL CHECK(latitude BETWEEN -90 AND 90),
        longitude REAL NOT NULL CHECK(longitude BETWEEN -180 AND 180),
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        forecast_json TEXT,
        forecast_updated_at TEXT
    ))SQL",
};

constexpr const char *desktop[] = {
    R"SQL(CREATE TABLE wallpapers (
        id TEXT PRIMARY KEY,
        display_name TEXT NOT NULL,
        filename TEXT NOT NULL UNIQUE,
        mime_type TEXT NOT NULL CHECK(mime_type IN ('image/jpeg', 'image/png', 'image/webp')),
        byte_size INTEGER NOT NULL CHECK(byte_size > 0),
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    ))SQL",
    "CREATE INDEX idx_wallpapers_order ON wallpapers(created_at, id)",
    R"SQL(CREATE TABLE desktop_apps (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        display_name TEXT NOT NULL CHECK(length(trim(display_name)) BETWEEN 1 AND 120),
        executable_path TEXT NOT NULL COLLATE NOCASE UNIQUE,
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    ))SQL",
    "CREATE INDEX idx_desktop_apps_display_name ON desktop_apps(display_name COLLATE NOCASE, id)",
    R"SQL(CREATE TABLE desktop_app_icons (
        app_id INTEGER PRIMARY KEY REFERENCES desktop_apps(id) ON DELETE CASCADE,
        filename TEXT NOT NULL UNIQUE,
        mime_type TEXT NOT NULL CHECK(mime_type IN ('image/jpeg', 'image/png', 'image/webp', 'image/x-icon')),
        byte_size INTEGER NOT NULL CHECK(byte_size > 0),
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    ))SQL",
    R"SQL(CREATE TABLE setups (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL COLLATE NOCASE UNIQUE CHECK(length(trim(name)) BETWEEN 1 AND 120),
        description TEXT NOT NULL DEFAULT '' CHECK(length(description) <= 300),
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    ))SQL",
    R"SQL(CREATE TABLE setup_apps (
        setup_id INTEGER NOT NULL REFERENCES setups(id) ON DELETE CASCADE,
        app_id INTEGER NOT NULL REFERENCES desktop_apps(id) ON DELETE CASCADE,
        position INTEGER NOT NULL CHECK(position >= 0),
        PRIMARY KEY(setup_id, app_id),
        UNIQUE(setup_id, position)
    ))SQL",
    "CREATE INDEX idx_setup_apps_app ON setup_apps(app_id, setup_id)",
    R"SQL(CREATE TABLE setup_icons (
        setup_id INTEGER PRIMARY KEY REFERENCES setups(id) ON DELETE CASCADE,
        filename TEXT NOT NULL UNIQUE,
        mime_type TEXT NOT NULL CHECK(mime_type IN ('image/jpeg', 'image/png', 'image/webp', 'image/x-icon')),
        byte_size INTEGER NOT NULL CHECK(byte_size > 0),
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    ))SQL",
};

constexpr const char *steam[] = {
    R"SQL(CREATE TABLE steam_game_settings (
        id INTEGER PRIMARY KEY CHECK(id = 1),
        country_code TEXT NOT NULL CHECK(length(country_code) = 2 AND country_code GLOB '[A-Z][A-Z]'),
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    ))SQL",
    "INSERT INTO steam_game_settings(id, country_code) VALUES(1, 'TR')",
    R"SQL(CREATE TABLE steam_games (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        steam_app_id INTEGER NOT NULL UNIQUE CHECK(steam_app_id > 0),
        store_url TEXT NOT NULL,
        name TEXT NOT NULL CHECK(length(trim(name)) BETWEEN 1 AND 300),
        image_source_url TEXT NOT NULL DEFAULT '',
        price_status TEXT NOT NULL CHECK(price_status IN ('priced', 'free', 'unavailable')),
        currency TEXT CHECK(currency IS NULL OR (length(currency) = 3 AND currency GLOB '[A-Z][A-Z][A-Z]')),
        regular_price_minor INTEGER CHECK(regular_price_minor IS NULL OR regular_price_minor >= 0),
        current_price_minor INTEGER CHECK(current_price_minor IS NULL OR current_price_minor >= 0),
        discount_percent INTEGER NOT NULL DEFAULT 0 CHECK(discount_percent BETWEEN 0 AND 100),
        price_country_code TEXT NOT NULL CHECK(length(price_country_code) = 2 AND price_country_code GLOB '[A-Z][A-Z]'),
        last_checked_at TEXT,
        last_attempted_at TEXT,
        last_refresh_error TEXT NOT NULL DEFAULT '',
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    ))SQL",
    "CREATE INDEX idx_steam_games_name ON steam_games(name COLLATE NOCASE, id)",
    R"SQL(CREATE TABLE steam_game_images (
        game_id INTEGER PRIMARY KEY REFERENCES steam_games(id) ON DELETE CASCADE,
        filename TEXT NOT NULL UNIQUE,
        mime_type TEXT NOT NULL CHECK(mime_type IN ('image/jpeg', 'image/png', 'image/webp')),
        byte_size INTEGER NOT NULL CHECK(byte_size > 0),
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    ))SQL",
};

constexpr const char *steamPriceObservations[] = {
    // Observations are owned by a tracked game and are removed with that game.
    R"SQL(CREATE TABLE steam_price_observations (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        steam_game_id INTEGER NOT NULL REFERENCES steam_games(id) ON DELETE CASCADE,
        steam_app_id INTEGER NOT NULL CHECK(steam_app_id > 0),
        country_code TEXT NOT NULL CHECK(length(country_code) = 2 AND country_code GLOB '[A-Z][A-Z]'),
        currency_code TEXT CHECK(currency_code IS NULL OR (length(currency_code) = 3 AND currency_code GLOB '[A-Z][A-Z][A-Z]')),
        price_state TEXT NOT NULL CHECK(price_state IN ('priced', 'free', 'unavailable', 'unsupported')),
        current_price_minor INTEGER CHECK(current_price_minor IS NULL OR current_price_minor >= 0),
        regular_price_minor INTEGER CHECK(regular_price_minor IS NULL OR regular_price_minor >= 0),
        discount_percent INTEGER NOT NULL DEFAULT 0 CHECK(discount_percent BETWEEN 0 AND 100),
        observed_at_utc TEXT NOT NULL CHECK(
            length(trim(observed_at_utc)) >= 20
            AND substr(observed_at_utc, -1, 1) = 'Z'
            AND julianday(observed_at_utc) IS NOT NULL
        ),
        CHECK(
            (price_state = 'priced' AND currency_code IS NOT NULL
                AND current_price_minor IS NOT NULL)
            OR (price_state = 'free' AND current_price_minor IS 0
                AND regular_price_minor IS 0 AND discount_percent = 0)
            OR (price_state IN ('unavailable', 'unsupported')
                AND current_price_minor IS NULL AND regular_price_minor IS NULL
                AND discount_percent = 0)
        )
    ))SQL",
    "CREATE INDEX idx_steam_price_observations_game_market_time ON steam_price_observations(steam_game_id, country_code, currency_code, observed_at_utc, id)",
    "CREATE INDEX idx_steam_price_observations_game_time ON steam_price_observations(steam_game_id, observed_at_utc, id)",
};

constexpr const char *favoriteOrdering[] = {
    "DROP INDEX idx_favorite_categories_order",
    "CREATE INDEX idx_favorite_categories_order ON favorite_categories(source, display_order, name_normalized, id)",
    "DROP INDEX idx_telegram_favorites_order",
    "CREATE INDEX idx_telegram_favorites_order ON telegram_favorites(added_at ASC, username COLLATE NOCASE ASC)",
    "DROP INDEX idx_youtube_favorites_order",
    "CREATE INDEX idx_youtube_favorites_order ON youtube_favorites(added_at ASC, channel_id ASC)",
};

constexpr const char *dataImport[] = {
    R"SQL(CREATE TABLE data_imports (
        import_id TEXT PRIMARY KEY,
        source_schema_version INTEGER NOT NULL CHECK(source_schema_version IN (24, 25)),
        completed_at TEXT NOT NULL,
        report_json TEXT NOT NULL
    ))SQL",
};

constexpr const char *applicationCategories[] = {
    R"SQL(CREATE TABLE application_categories (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL CHECK(length(trim(name)) BETWEEN 1 AND 80),
        name_normalized TEXT NOT NULL UNIQUE CHECK(length(trim(name_normalized)) > 0),
        description TEXT,
        color TEXT,
        icon_identifier TEXT,
        display_order INTEGER NOT NULL DEFAULT 0 CHECK(display_order >= 0),
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    ))SQL",
    "CREATE INDEX idx_application_categories_order ON application_categories(display_order, name_normalized, id)",
    "ALTER TABLE desktop_apps ADD COLUMN category_id INTEGER REFERENCES application_categories(id) ON DELETE SET NULL",
    "CREATE INDEX idx_desktop_apps_category ON desktop_apps(category_id, display_name COLLATE NOCASE, id)",
};

constexpr const char *taskHistory[] = {
    R"SQL(CREATE TABLE task_history (
        event_id INTEGER PRIMARY KEY AUTOINCREMENT,
        -- Deliberately no task foreign key: history survives task deletion.
        task_id INTEGER NOT NULL CHECK(task_id > 0),
        event_type TEXT NOT NULL CHECK(event_type IN (
            'task_created', 'task_updated', 'task_title_changed', 'description_changed',
            'due_date_changed', 'priority_changed', 'difficulty_changed', 'tags_changed',
            'task_completed', 'task_reopened',
            'subtask_created', 'subtask_updated', 'subtask_completed',
            'subtask_reopened', 'subtask_removed', 'task_deleted'
        )),
        occurred_at_utc TEXT NOT NULL,
        task_revision INTEGER CHECK(task_revision IS NULL OR task_revision > 0),
        task_title TEXT NOT NULL,
        serialization_version INTEGER NOT NULL CHECK(serialization_version > 0),
        changed_fields_json TEXT NOT NULL,
        previous_values_json TEXT NOT NULL,
        new_values_json TEXT NOT NULL
    ))SQL",
    "CREATE INDEX idx_task_history_task_time ON task_history(task_id, occurred_at_utc DESC, event_id DESC)",
    "CREATE INDEX idx_task_history_time ON task_history(occurred_at_utc DESC, event_id DESC)",
    "CREATE INDEX idx_task_history_type_time ON task_history(event_type, occurred_at_utc DESC, event_id DESC)",
    "CREATE TRIGGER task_history_prevent_update BEFORE UPDATE ON task_history BEGIN SELECT RAISE(ABORT, 'task history is append-only'); END",
    "CREATE TRIGGER task_history_prevent_delete BEFORE DELETE ON task_history BEGIN SELECT RAISE(ABORT, 'task history is append-only'); END",
};

constexpr const char *taskScheduling[] = {
    R"SQL(CREATE TABLE task_schedules (
        task_id INTEGER PRIMARY KEY REFERENCES tasks(id) ON DELETE CASCADE,
        planned_date TEXT CHECK(planned_date IS NULL OR
            (length(planned_date) = 10 AND date(planned_date) = planned_date)),
        planned_start_time TEXT CHECK(planned_start_time IS NULL OR
            (length(planned_start_time) = 5
             AND planned_start_time GLOB '[0-2][0-9]:[0-5][0-9]'
             AND CAST(substr(planned_start_time, 1, 2) AS INTEGER) BETWEEN 0 AND 23)),
        estimated_duration_minutes INTEGER CHECK(estimated_duration_minutes IS NULL OR
            (typeof(estimated_duration_minutes) = 'integer'
             AND estimated_duration_minutes BETWEEN 1 AND 525600)),
        revision INTEGER NOT NULL DEFAULT 1
            CHECK(typeof(revision) = 'integer' AND revision > 0),
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        CHECK(planned_start_time IS NULL OR planned_date IS NOT NULL)
    ))SQL",
    "CREATE INDEX idx_task_schedules_date_time ON task_schedules(planned_date, planned_start_time, task_id) WHERE planned_date IS NOT NULL",
    R"SQL(CREATE TABLE scheduling_preferences (
        id INTEGER PRIMARY KEY CHECK(id = 1),
        preferred_working_days_mask INTEGER NOT NULL DEFAULT 31
            CHECK(typeof(preferred_working_days_mask) = 'integer'
                  AND preferred_working_days_mask BETWEEN 0 AND 127),
        workday_start_time TEXT NOT NULL DEFAULT '09:00'
            CHECK(length(workday_start_time) = 5
                  AND workday_start_time GLOB '[0-2][0-9]:[0-5][0-9]'
                  AND CAST(substr(workday_start_time, 1, 2) AS INTEGER) BETWEEN 0 AND 23),
        workday_end_time TEXT NOT NULL DEFAULT '17:00'
            CHECK(length(workday_end_time) = 5
                  AND workday_end_time GLOB '[0-2][0-9]:[0-5][0-9]'
                  AND CAST(substr(workday_end_time, 1, 2) AS INTEGER) BETWEEN 0 AND 23),
        maximum_planned_work_minutes_per_day INTEGER NOT NULL DEFAULT 360
            CHECK(typeof(maximum_planned_work_minutes_per_day) = 'integer'
                  AND maximum_planned_work_minutes_per_day BETWEEN 1 AND 1440),
        default_estimated_duration_minutes INTEGER NOT NULL DEFAULT 60
            CHECK(typeof(default_estimated_duration_minutes) = 'integer'
                  AND default_estimated_duration_minutes BETWEEN 1 AND 525600),
        break_between_work_blocks_minutes INTEGER DEFAULT 15
            CHECK(break_between_work_blocks_minutes IS NULL OR
                  (typeof(break_between_work_blocks_minutes) = 'integer'
                   AND break_between_work_blocks_minutes BETWEEN 1 AND 240)),
        revision INTEGER NOT NULL DEFAULT 1
            CHECK(typeof(revision) = 'integer' AND revision > 0),
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        CHECK(workday_end_time > workday_start_time),
        CHECK(maximum_planned_work_minutes_per_day <=
            ((CAST(substr(workday_end_time, 1, 2) AS INTEGER) * 60)
              + CAST(substr(workday_end_time, 4, 2) AS INTEGER))
            - ((CAST(substr(workday_start_time, 1, 2) AS INTEGER) * 60)
              + CAST(substr(workday_start_time, 4, 2) AS INTEGER)))
    ))SQL",
    "INSERT INTO scheduling_preferences(id) VALUES(1)",
};

constexpr const char *weatherComparison[] = {
    R"SQL(CREATE TABLE weather_comparison_cities (
        identity TEXT PRIMARY KEY CHECK(length(trim(identity)) BETWEEN 1 AND 80),
        geocoding_id INTEGER CHECK(geocoding_id IS NULL OR geocoding_id > 0),
        display_name TEXT NOT NULL CHECK(length(trim(display_name)) BETWEEN 1 AND 120),
        administrative_area TEXT NOT NULL DEFAULT '' CHECK(length(administrative_area) <= 120),
        country TEXT NOT NULL DEFAULT '' CHECK(length(country) <= 120),
        timezone TEXT NOT NULL DEFAULT '' CHECK(length(timezone) <= 100),
        latitude REAL NOT NULL CHECK(latitude BETWEEN -90 AND 90),
        longitude REAL NOT NULL CHECK(longitude BETWEEN -180 AND 180),
        display_order INTEGER NOT NULL CHECK(typeof(display_order) = 'integer' AND display_order BETWEEN 0 AND 3),
        forecast_json TEXT,
        forecast_updated_at TEXT,
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        CHECK((forecast_json IS NULL AND forecast_updated_at IS NULL)
            OR (forecast_json IS NOT NULL AND length(forecast_json) > 0
                AND forecast_updated_at IS NOT NULL AND length(trim(forecast_updated_at)) > 0))
    ))SQL",
    "CREATE INDEX idx_weather_comparison_order ON weather_comparison_cities(display_order, identity)",
};

constexpr std::array migrations{
    Migration{1, "core tasks currencies preferences", core, qsizetype(std::size(core))},
    Migration{2, "favorites and retained update news", favorites, qsizetype(std::size(favorites))},
    Migration{3, "saved weather location and cache", weather, qsizetype(std::size(weather))},
    Migration{4, "wallpapers desktop applications and setups", desktop, qsizetype(std::size(desktop))},
    Migration{5, "Steam tracking", steam, qsizetype(std::size(steam))},
    Migration{6, "favorite display ordering", favoriteOrdering, qsizetype(std::size(favoriteOrdering))},
    Migration{7, "one-time retained profile import ledger", dataImport,
              qsizetype(std::size(dataImport))},
    Migration{8, "saved application categories", applicationCategories,
              qsizetype(std::size(applicationCategories))},
    Migration{9, "persistent task history", taskHistory,
              qsizetype(std::size(taskHistory))},
    Migration{10, "task scheduling data and preferences", taskScheduling,
              qsizetype(std::size(taskScheduling))},
    Migration{11, "weather comparison cities and forecast cache", weatherComparison,
              qsizetype(std::size(weatherComparison))},
    Migration{12, "Steam price observations", steamPriceObservations,
              qsizetype(std::size(steamPriceObservations))},
};

bool fail(QString *diagnostic, const QString &message)
{
    if (diagnostic) {
        *diagnostic = message;
    }
    return false;
}

bool exec(QSqlQuery &query, const QString &sql, QString *diagnostic)
{
    if (query.exec(sql)) {
        return true;
    }
    return fail(diagnostic, query.lastError().text());
}

bool tableExists(const QSqlDatabase &database, QString *diagnostic, bool *exists)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1"));
    query.addBindValue(QStringLiteral("schema_migrations"));
    if (!query.exec()) {
        return fail(diagnostic, query.lastError().text());
    }
    *exists = query.next();
    return true;
}

bool hasUnmanagedTables(const QSqlDatabase &database, QString *diagnostic, bool *hasTables)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' LIMIT 1"))) {
        return fail(diagnostic, query.lastError().text());
    }
    *hasTables = query.next();
    return true;
}

bool hasUnexpectedTables(const QSqlDatabase &database, QString *diagnostic, bool *unexpected)
{
    static const QSet<QString> known{
        QStringLiteral("schema_migrations"), QStringLiteral("tasks"), QStringLiteral("tags"),
        QStringLiteral("task_tags"), QStringLiteral("task_subtasks"),
        QStringLiteral("currency_favorites"), QStringLiteral("preferences"),
        QStringLiteral("favorite_categories"), QStringLiteral("telegram_favorites"),
        QStringLiteral("youtube_favorites"), QStringLiteral("favorite_update_checkpoints"),
        QStringLiteral("favorite_update_seen_items"), QStringLiteral("favorite_news_items"),
        QStringLiteral("favorite_news_state"), QStringLiteral("saved_weather"),
        QStringLiteral("application_categories"),
        QStringLiteral("wallpapers"), QStringLiteral("desktop_apps"),
        QStringLiteral("desktop_app_icons"), QStringLiteral("setups"),
        QStringLiteral("setup_apps"), QStringLiteral("setup_icons"),
        QStringLiteral("steam_game_settings"), QStringLiteral("steam_games"),
        QStringLiteral("steam_game_images"),
        QStringLiteral("steam_price_observations"), QStringLiteral("data_imports"),
        QStringLiteral("task_history"), QStringLiteral("task_schedules"),
        QStringLiteral("scheduling_preferences"),
        QStringLiteral("weather_comparison_cities")
    };
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"))) {
        return fail(diagnostic, query.lastError().text());
    }
    *unexpected = false;
    while (query.next()) {
        if (!known.contains(query.value(0).toString())) {
            *unexpected = true;
            break;
        }
    }
    return true;
}

bool loadVersion(const QSqlDatabase &database, int *version, QString *diagnostic)
{
    QSqlQuery query(database);
    if (!exec(query, QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_migrations"), diagnostic)
        || !query.next()) {
        return false;
    }
    *version = query.value(0).toInt();
    return true;
}

bool validateHistory(const QSqlDatabase &database, int current, QString *diagnostic)
{
    QSqlQuery query(database);
    if (!exec(query, QStringLiteral("SELECT version, name FROM schema_migrations ORDER BY version"), diagnostic)) {
        return false;
    }
    int expected = 1;
    while (query.next()) {
        if (expected > int(migrations.size()) || query.value(0).toInt() != expected
            || query.value(1).toString() != QString::fromLatin1(migrations[expected - 1].name)) {
            return fail(diagnostic, QStringLiteral("The database schema history is not recognized."));
        }
        ++expected;
    }
    if (expected - 1 != current || current > int(migrations.size())) {
        return fail(diagnostic, QStringLiteral("The database schema history is incomplete or newer than this application."));
    }
    return true;
}

bool applyOne(QSqlDatabase &database, const Migration &migration, QString *diagnostic)
{
    if (!database.transaction()) {
        return fail(diagnostic, database.lastError().text());
    }
    auto rollback = [&database] { database.rollback(); };
    QSqlQuery query(database);
    for (qsizetype index = 0; index < migration.statementCount; ++index) {
        if (!exec(query, QString::fromUtf8(migration.statements[index]), diagnostic)) {
            rollback();
            return false;
        }
    }
    query.prepare(QStringLiteral("INSERT INTO schema_migrations(version, name) VALUES(?, ?)"));
    query.addBindValue(migration.version);
    query.addBindValue(QString::fromLatin1(migration.name));
    if (!query.exec()) {
        const QString message = query.lastError().text();
        rollback();
        return fail(diagnostic, message);
    }
    if (!database.commit()) {
        const QString message = database.lastError().text();
        rollback();
        return fail(diagnostic, message);
    }
    return true;
}

} // namespace

int SchemaMigrator::latestVersion() noexcept
{
    return int(migrations.size());
}

bool SchemaMigrator::apply(QSqlDatabase &database, QString *diagnostic)
{
    if (!database.isValid() || !database.isOpen() || database.driverName() != QStringLiteral("QSQLITE")) {
        return fail(diagnostic, QStringLiteral("A live SQLite connection is required."));
    }
    Q_ASSERT(QThread::currentThread() == database.driver()->thread());

    QSqlQuery pragma(database);
    if (!exec(pragma, QStringLiteral("PRAGMA foreign_keys = ON"), diagnostic)
        || !exec(pragma, QStringLiteral("PRAGMA busy_timeout = 5000"), diagnostic)) {
        return false;
    }
    pragma.finish();

    bool ledgerExists = false;
    if (!tableExists(database, diagnostic, &ledgerExists)) {
        return false;
    }
    if (!ledgerExists) {
        bool unmanaged = false;
        if (!hasUnmanagedTables(database, diagnostic, &unmanaged)) {
            return false;
        }
        if (unmanaged) {
            return fail(diagnostic, QStringLiteral("An untracked database cannot be adopted as a Qt application database."));
        }
        if (!database.transaction()) {
            return fail(diagnostic, database.lastError().text());
        }
        QSqlQuery createLedger(database);
        const bool created = createLedger.exec(QStringLiteral(
            "CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY CHECK(version > 0), "
            "name TEXT NOT NULL, applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"));
        if (!created || !database.commit()) {
            const QString message = created ? database.lastError().text() : createLedger.lastError().text();
            database.rollback();
            return fail(diagnostic, message);
        }
    } else {
        bool unexpectedTables = false;
        if (!hasUnexpectedTables(database, diagnostic, &unexpectedTables)) {
            return false;
        }
        if (unexpectedTables) {
            return fail(diagnostic, QStringLiteral("The database contains tables outside the Qt application's schema."));
        }
    }

    int current = 0;
    if (!loadVersion(database, &current, diagnostic) || !validateHistory(database, current, diagnostic)) {
        return false;
    }
    if (!exec(pragma, QStringLiteral("PRAGMA journal_mode = WAL"), diagnostic)) {
        return false;
    }
    pragma.finish();
    for (const auto &migration : migrations) {
        if (migration.version <= current) {
            continue;
        }
        if (!applyOne(database, migration, diagnostic)) {
            return false;
        }
    }
    QSqlQuery verifyForeignKeys(database);
    if (!verifyForeignKeys.exec(QStringLiteral("PRAGMA foreign_keys")) || !verifyForeignKeys.next()
        || verifyForeignKeys.value(0).toInt() != 1) {
        return fail(diagnostic, QStringLiteral("SQLite foreign key enforcement could not be enabled."));
    }
    return true;
}

} // namespace something::storage

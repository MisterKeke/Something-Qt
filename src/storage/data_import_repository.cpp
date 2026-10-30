#include "data_import_repository.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHash>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlDriver>
#include <QSqlQuery>
#include <QThread>

namespace something::storage {
namespace {

bool isBuiltInWallpaper(const QString &value)
{
    return value == QStringLiteral("builtin:original") || value == QStringLiteral("builtin:sandrone")
        || value == QStringLiteral("builtin:hu-tao") || value == QStringLiteral("builtin:skirk");
}

domain::AppError sqlError(const QSqlQuery &query)
{
    bool codeOk = false;
    const int code = query.lastError().nativeErrorCode().toInt(&codeOk);
    return codeOk && (code & 0xff) == 19
        ? domain::AppError(domain::ErrorCategory::Conflict)
        : domain::AppError(domain::ErrorCategory::Storage);
}

domain::AppError databaseError(const QSqlDatabase &database)
{
    bool codeOk = false;
    const int code = database.lastError().nativeErrorCode().toInt(&codeOk);
    return codeOk && (code & 0xff) == 19
        ? domain::AppError(domain::ErrorCategory::Conflict)
        : domain::AppError(domain::ErrorCategory::Storage);
}

QVariant toSqlValue(const QJsonValue &value)
{
    if (value.isNull() || value.isUndefined()) return {};
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool();
    if (value.isDouble()) return value.toDouble();
    return {};
}

bool countRows(const QSqlDatabase &database, const QString &table, qint64 *count)
{
    QSqlQuery query(database);
    // The table names passed here are compile-time constants from this file.
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table) || !query.next()) return false;
    *count = query.value(0).toLongLong();
    return true;
}

bool writeRows(QSqlDatabase &database, const QJsonArray &rows, const QString &table,
               const QStringList &columns, qint64 *inserted,
               const std::function<bool()> &cancelled, domain::AppError *error)
{
    QStringList placeholders;
    for (qsizetype i = 0; i < columns.size(); ++i) placeholders.push_back(QStringLiteral("?"));
    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT INTO ") + table
        + QStringLiteral("(") + columns.join(QLatin1Char(','))
        + QStringLiteral(") VALUES(") + placeholders.join(QLatin1Char(','))
        + QStringLiteral(")"));

    *inserted = 0;
    for (const QJsonValue &rowValue : rows) {
        if (cancelled && cancelled()) {
            *error = domain::AppError::cancelled();
            return false;
        }
        if (!rowValue.isObject()) {
            *error = domain::AppError(domain::ErrorCategory::Validation);
            return false;
        }
        const QJsonObject row = rowValue.toObject();
        for (qsizetype index = 0; index < columns.size(); ++index) {
            const QString &column = columns.at(index);
            if (!row.contains(column)) {
                *error = domain::AppError(domain::ErrorCategory::Validation);
                return false;
            }
            query.bindValue(index, toSqlValue(row.value(column)));
        }
        if (!query.exec()) {
            *error = sqlError(query);
            return false;
        }
        ++*inserted;
        query.finish();
    }
    return true;
}

bool tableRows(const QJsonObject &rows, const QString &name, QJsonArray *result)
{
    const QJsonValue value = rows.value(name);
    if (!value.isArray()) return false;
    *result = value.toArray();
    return true;
}

} // namespace

bool DataImportRepository::targetIsEmpty(bool *empty, QString *blockingReason) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (blockingReason) blockingReason->clear();
    static const QStringList tables{
        QStringLiteral("tasks"), QStringLiteral("tags"), QStringLiteral("task_tags"),
        QStringLiteral("task_subtasks"), QStringLiteral("currency_favorites"),
        QStringLiteral("favorite_categories"),
        QStringLiteral("telegram_favorites"), QStringLiteral("youtube_favorites"),
        QStringLiteral("favorite_update_checkpoints"), QStringLiteral("favorite_update_seen_items"),
        QStringLiteral("favorite_news_items"),
        QStringLiteral("saved_weather"), QStringLiteral("wallpapers"),
        QStringLiteral("application_categories"), QStringLiteral("desktop_apps"),
        QStringLiteral("desktop_app_icons"),
        QStringLiteral("setups"), QStringLiteral("setup_apps"), QStringLiteral("setup_icons"),
        QStringLiteral("steam_games"), QStringLiteral("steam_game_images"),
        QStringLiteral("steam_release_follows"),
        QStringLiteral("steam_game_update_follows"),
        QStringLiteral("steam_game_update_news_items"),
        QStringLiteral("data_imports")};
    for (const QString &table : tables) {
        qint64 count = 0;
        if (!countRows(m_database, table, &count)) return false;
        if (count != 0) {
            *empty = false;
            if (blockingReason) {
                static const QHash<QString, QString> labels{
                    {QStringLiteral("tasks"), QStringLiteral("tasks")},
                    {QStringLiteral("tags"), QStringLiteral("tags")},
                    {QStringLiteral("task_tags"), QStringLiteral("task tags")},
                    {QStringLiteral("task_subtasks"), QStringLiteral("subtasks")},
                    {QStringLiteral("currency_favorites"), QStringLiteral("currency favorites")},
                    {QStringLiteral("favorite_categories"), QStringLiteral("favorite categories")},
                    {QStringLiteral("telegram_favorites"), QStringLiteral("Telegram favorites")},
                    {QStringLiteral("youtube_favorites"), QStringLiteral("YouTube favorites")},
                    {QStringLiteral("saved_weather"), QStringLiteral("saved weather locations")},
                    {QStringLiteral("wallpapers"), QStringLiteral("imported wallpapers")},
                    {QStringLiteral("application_categories"), QStringLiteral("application categories")},
                    {QStringLiteral("desktop_apps"), QStringLiteral("saved applications")},
                    {QStringLiteral("desktop_app_icons"), QStringLiteral("application icons")},
                    {QStringLiteral("setups"), QStringLiteral("setups")},
                    {QStringLiteral("setup_apps"), QStringLiteral("setup applications")},
                    {QStringLiteral("setup_icons"), QStringLiteral("setup icons")},
                    {QStringLiteral("steam_games"), QStringLiteral("tracked games")},
                    {QStringLiteral("steam_game_images"), QStringLiteral("game artwork")},
                    {QStringLiteral("steam_release_follows"), QStringLiteral("followed releases")},
                    {QStringLiteral("steam_game_update_follows"), QStringLiteral("followed Game Updates games")},
                    {QStringLiteral("steam_game_update_news_items"), QStringLiteral("cached Game Updates articles")},
                    {QStringLiteral("data_imports"), QStringLiteral("a completed import")}};
                *blockingReason = QStringLiteral("This Qt profile already contains %1 (%2 records). Import needs a fresh Qt profile; existing records will not be overwritten.")
                    .arg(labels.value(table, QStringLiteral("saved favorite updates"))).arg(count);
            }
            return true;
        }
    }

    // Opening an otherwise empty profile creates only these news-window
    // timestamps and an empty scan state. A built-in wallpaper choice also
    // needs no imported records/assets and is preserved by the transaction.
    QSqlQuery baseline(m_database);
    if (!baseline.exec(QStringLiteral(
        "SELECT key,value FROM preferences WHERE key NOT IN ("
        "'favorite_updates.previous_opened_at','favorite_updates.current_opened_at',"
        "'favorite_updates.previous_refresh_at','favorite_updates.last_refresh_at')"))) return false;
    while (baseline.next()) {
        const QString key = baseline.value(0).toString();
        if (key == QStringLiteral("appearance.selected_wallpaper")
            && isBuiltInWallpaper(baseline.value(1).toString())) continue;
        *empty = false;
        if (blockingReason) {
            *blockingReason = key == QStringLiteral("appearance.selected_wallpaper")
                ? QStringLiteral("This Qt profile refers to a custom or unavailable wallpaper. Import needs a profile without saved records; built-in wallpaper choices are allowed and kept.")
                : key == QStringLiteral("tasks.week_start")
                ? QStringLiteral("A week-start preference is saved in this Qt profile. Import requires a fresh Qt profile; this choice will not be overwritten.")
                : QStringLiteral("This Qt profile has saved settings. Import requires a fresh Qt profile; existing settings will not be overwritten.");
        }
        return true;
    }
    if (baseline.lastError().isValid()) return false;
    baseline.finish();
    if (!baseline.exec(QStringLiteral("SELECT id,errors_json FROM favorite_news_state"))) return false;
    while (baseline.next()) {
        QJsonParseError parseError;
        const QJsonDocument errors = QJsonDocument::fromJson(baseline.value(1).toString().toUtf8(), &parseError);
        if (baseline.value(0).toInt() != 1 || parseError.error != QJsonParseError::NoError
            || !errors.isArray() || !errors.array().isEmpty()) {
            *empty = false;
            if (blockingReason) *blockingReason = QStringLiteral("Saved favorite-news state prevents import into this Qt profile. Recheck after refreshing favorite updates, or use a fresh Qt profile.");
            return true;
        }
    }
    if (baseline.lastError().isValid()) return false;

    QSqlQuery settings(m_database);
    if (!settings.exec(QStringLiteral("SELECT id,country_code FROM steam_game_settings"))) return false;
    int settingCount = 0;
    while (settings.next()) {
        ++settingCount;
        if (settings.value(0).toInt() != 1 || settings.value(1).toString() != QStringLiteral("TR")) {
            *empty = false;
            if (blockingReason) *blockingReason = QStringLiteral("Steam tracking settings were changed in this Qt profile. Import requires a fresh Qt profile.");
            return true;
        }
    }
    if (settings.lastError().isValid()) return false;
    *empty = settingCount == 1;
    if (!*empty && blockingReason) *blockingReason = QStringLiteral("The default Steam tracking settings are missing. The Qt profile needs a storage check before import.");
    return true;
}

domain::AppError DataImportRepository::status(DataImportStatus *result) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!result) return domain::AppError(domain::ErrorCategory::Validation);
    QSqlQuery latest(m_database);
    if (!latest.exec(QStringLiteral(
            "SELECT completed_at,report_json FROM data_imports ORDER BY completed_at DESC LIMIT 1"))) {
        return sqlError(latest);
    }
    result->completed = latest.next();
    if (latest.lastError().isValid()) return sqlError(latest);
    if (result->completed) {
        result->completedAt = latest.value(0).toString();
        result->reportJson = latest.value(1).toString();
    } else {
        result->completedAt.clear();
        result->reportJson.clear();
    }
    return targetIsEmpty(&result->empty, &result->blockingReason)
        ? domain::AppError::none()
        : domain::AppError(domain::ErrorCategory::Storage);
}

domain::AppError DataImportRepository::hasCompletion(const QString &importId, bool *completed) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!completed || importId.isEmpty()) return domain::AppError(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT 1 FROM data_imports WHERE import_id=? LIMIT 1"));
    query.addBindValue(importId);
    if (!query.exec()) return sqlError(query);
    *completed = query.next();
    return domain::AppError::none();
}

domain::AppError DataImportRepository::importPlan(
    const QString &planPath, const QString &importId, QVariantMap *report,
    const std::function<bool()> &cancellationRequested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    static const QRegularExpression idPattern(QStringLiteral("^[a-f0-9]{32}$"));
    if (!report || !idPattern.match(importId).hasMatch())
        return domain::AppError(domain::ErrorCategory::Validation);

    QFile file(planPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64LL * 1024LL * 1024LL)
        return domain::AppError(domain::ErrorCategory::Storage);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return domain::AppError(domain::ErrorCategory::Validation);
    const QJsonObject plan = document.object();
    const int sourceVersion = plan.value(QStringLiteral("sourceSchemaVersion")).toInt();
    if ((sourceVersion != 24 && sourceVersion != 25)
        || plan.value(QStringLiteral("importId")).toString() != importId
        || !plan.value(QStringLiteral("rows")).isObject()) {
        return domain::AppError(domain::ErrorCategory::Validation);
    }
    QJsonObject rows = plan.value(QStringLiteral("rows")).toObject();
    QJsonObject failedReport = plan.value(QStringLiteral("report")).toObject();
    failedReport.insert(QStringLiteral("committed"), false);
    failedReport.insert(QStringLiteral("failures"), QJsonArray{QStringLiteral("Target validation, cancellation, or the database transaction prevented import.")});
    *report = {{QStringLiteral("json"), QString::fromUtf8(QJsonDocument(failedReport).toJson(QJsonDocument::Compact))}};

    if (cancellationRequested && cancellationRequested()) return domain::AppError::cancelled();
    if (!m_database.transaction()) return databaseError(m_database);

    auto rollback = [this] { m_database.rollback(); };
    bool empty = false;
    if (!targetIsEmpty(&empty)) { rollback(); return domain::AppError(domain::ErrorCategory::Storage); }
    if (!empty) { rollback(); return domain::AppError(domain::ErrorCategory::Conflict); }
    QString keptWallpaper;
    {
        QSqlQuery currentWallpaper(m_database);
        if (!currentWallpaper.exec(QStringLiteral("SELECT value FROM preferences WHERE key='appearance.selected_wallpaper'"))) {
            rollback();
            return sqlError(currentWallpaper);
        }
        if (currentWallpaper.next()) keptWallpaper = currentWallpaper.value(0).toString();
        if (currentWallpaper.lastError().isValid()) { rollback(); return sqlError(currentWallpaper); }
    }
    bool skippedSourceSelection = false;
    if (!keptWallpaper.isEmpty()) {
        QJsonArray sourcePreferences;
        if (!tableRows(rows, QStringLiteral("preferences"), &sourcePreferences)) {
            rollback();
            return domain::AppError(domain::ErrorCategory::Validation);
        }
        QJsonArray importPreferences;
        for (const QJsonValue &value : sourcePreferences) {
            if (value.isObject() && value.toObject().value(QStringLiteral("key")).toString()
                == QStringLiteral("appearance.selected_wallpaper")) {
                skippedSourceSelection = true;
            } else {
                importPreferences.append(value);
            }
        }
        rows.insert(QStringLiteral("preferences"), importPreferences);
    }
    QSqlQuery clearBaseline(m_database);
    if (!clearBaseline.exec(QStringLiteral("DELETE FROM preferences"))
        || !clearBaseline.exec(QStringLiteral("DELETE FROM favorite_news_state"))) {
        rollback();
        return sqlError(clearBaseline);
    }
    QJsonObject importedCounts;
    domain::AppError writeError;
    auto importTable = [&](const QString &key, const QString &target, const QStringList &columns) {
        QJsonArray table;
        qint64 inserted = 0;
        if (!tableRows(rows, key, &table)
            || !writeRows(m_database, table, target, columns, &inserted,
                          cancellationRequested, &writeError)) {
            return false;
        }
        importedCounts.insert(key, static_cast<qint64>(inserted));
        return true;
    };

    const QList<QPair<QString, QStringList>> order{
        {QStringLiteral("tasks"), {"id","title","description","is_completed","created_at","updated_at","due_date","priority","difficulty","revision"}},
        {QStringLiteral("tags"), {"id","name","name_normalized"}},
        {QStringLiteral("task_tags"), {"task_id","tag_id"}},
        {QStringLiteral("task_subtasks"), {"id","task_id","title","is_completed","position","created_at"}},
        {QStringLiteral("currency_favorites"), {"base_code","quote_code"}},
        {QStringLiteral("favorite_categories"), {"id","name","name_normalized","source","color","created_at","display_order","updated_at"}},
        {QStringLiteral("telegram_favorites"), {"username","added_at","category_id"}},
        {QStringLiteral("youtube_favorites"), {"channel_id","added_at","category_id","username"}},
        {QStringLiteral("favorite_update_checkpoints"), {"source","source_id","checked_through","last_success_at","last_attempted_at","last_error","updated_at"}},
        {QStringLiteral("favorite_update_seen_items"), {"source","source_id","item_id","published_at","first_seen_at","updated_at"}},
        {QStringLiteral("favorite_news_items"), {"source","source_id","item_id","published_at","discovered_at","payload_json","updated_at"}},
        {QStringLiteral("favorite_news_state"), {"id","scan_started_at","errors_json","updated_at"}},
        {QStringLiteral("saved_weather"), {"id","latitude","longitude","created_at","updated_at","forecast_json","forecast_updated_at"}},
        {QStringLiteral("desktop_apps"), {"id","display_name","executable_path","created_at","updated_at"}},
        {QStringLiteral("setups"), {"id","name","description","created_at","updated_at"}},
        {QStringLiteral("setup_apps"), {"setup_id","app_id","position"}},
        {QStringLiteral("steam_games"), {"id","steam_app_id","store_url","name","image_source_url","price_status","currency","regular_price_minor","current_price_minor","discount_percent","price_country_code","last_checked_at","last_attempted_at","last_refresh_error","created_at","updated_at"}},
        {QStringLiteral("wallpapers"), {"id","display_name","filename","mime_type","byte_size","created_at"}},
        {QStringLiteral("desktop_app_icons"), {"app_id","filename","mime_type","byte_size","updated_at"}},
        {QStringLiteral("setup_icons"), {"setup_id","filename","mime_type","byte_size","updated_at"}},
        {QStringLiteral("steam_game_images"), {"game_id","filename","mime_type","byte_size","updated_at"}},
        {QStringLiteral("preferences"), {"key","value"}},
    };
    for (const auto &entry : order) {
        if (!importTable(entry.first, entry.first, entry.second)) {
            failedReport.insert(QStringLiteral("failedTable"), entry.first);
            failedReport.insert(QStringLiteral("failures"), QJsonArray{
                QStringLiteral("The retained %1 records failed target validation or insertion.").arg(entry.first)});
            *report = {{QStringLiteral("json"), QString::fromUtf8(QJsonDocument(failedReport).toJson(QJsonDocument::Compact))}};
            rollback();
            return writeError.isCancelled() ? writeError
                : (writeError.isError() ? writeError
                                        : domain::AppError(domain::ErrorCategory::Validation));
        }
    }

    if (!keptWallpaper.isEmpty()) {
        QSqlQuery restoreWallpaper(m_database);
        restoreWallpaper.prepare(QStringLiteral("INSERT INTO preferences(key,value) VALUES('appearance.selected_wallpaper',?)"));
        restoreWallpaper.addBindValue(keptWallpaper);
        if (!restoreWallpaper.exec()) { rollback(); return sqlError(restoreWallpaper); }
    }

    QJsonArray steamSettings;
    qint64 steamSettingsCount = 0;
    if (!tableRows(rows, QStringLiteral("steam_game_settings"), &steamSettings)
        || steamSettings.size() != 1 || !steamSettings.first().isObject()) {
        rollback();
        return domain::AppError(domain::ErrorCategory::Validation);
    }
    const QJsonObject steamSetting = steamSettings.first().toObject();
    QSqlQuery updateSteam(m_database);
    updateSteam.prepare(QStringLiteral(
        "UPDATE steam_game_settings SET country_code=?,updated_at=? WHERE id=1"));
    updateSteam.addBindValue(toSqlValue(steamSetting.value(QStringLiteral("country_code"))));
    updateSteam.addBindValue(toSqlValue(steamSetting.value(QStringLiteral("updated_at"))));
    if (!updateSteam.exec() || updateSteam.numRowsAffected() != 1) {
        rollback();
        return updateSteam.lastError().isValid() ? sqlError(updateSteam)
                                                  : domain::AppError(domain::ErrorCategory::Storage);
    }
    steamSettingsCount = 1;
    importedCounts.insert(QStringLiteral("steam_game_settings"), steamSettingsCount);

    for (auto it = importedCounts.constBegin(); it != importedCounts.constEnd(); ++it) {
        qint64 stored = 0;
        const qint64 kept = it.key() == QStringLiteral("preferences") && !keptWallpaper.isEmpty() ? 1 : 0;
        if (!countRows(m_database, it.key(), &stored) || stored != it.value().toInteger() + kept) {
            rollback();
            return domain::AppError(domain::ErrorCategory::Storage);
        }
    }

    const QJsonObject originalReport = plan.value(QStringLiteral("report")).toObject();
    QJsonObject finalReport = originalReport;
    finalReport.insert(QStringLiteral("committed"), true);
    finalReport.insert(QStringLiteral("importedCounts"), importedCounts);
    if (!keptWallpaper.isEmpty()) {
        finalReport.insert(QStringLiteral("preservedTargetPreferences"), QJsonArray{QStringLiteral("appearance.selected_wallpaper")});
        QJsonArray warnings = finalReport.value(QStringLiteral("warnings")).toArray();
        warnings.append(QStringLiteral("Kept your current built-in Qt wallpaper choice. Imported wallpapers remain available in the gallery."));
        finalReport.insert(QStringLiteral("warnings"), warnings);
        if (skippedSourceSelection) {
            QJsonObject skipped = finalReport.value(QStringLiteral("skippedCounts")).toObject();
            skipped.insert(QStringLiteral("source_wallpaper_selection"), skipped.value(QStringLiteral("source_wallpaper_selection")).toInteger() + 1);
            finalReport.insert(QStringLiteral("skippedCounts"), skipped);
        }
    }
    const QByteArray reportBytes = QJsonDocument(finalReport).toJson(QJsonDocument::Compact);
    QSqlQuery marker(m_database);
    marker.prepare(QStringLiteral(
        "INSERT INTO data_imports(import_id,source_schema_version,completed_at,report_json) "
        "VALUES(?,?,CURRENT_TIMESTAMP,?)"));
    marker.addBindValue(importId);
    marker.addBindValue(sourceVersion);
    marker.addBindValue(QString::fromUtf8(reportBytes));
    if (!marker.exec()) {
        rollback();
        return sqlError(marker);
    }

    QSqlQuery foreignKeys(m_database);
    if (!foreignKeys.exec(QStringLiteral("PRAGMA foreign_key_check"))) {
        rollback();
        return sqlError(foreignKeys);
    }
    if (foreignKeys.next()) {
        rollback();
        return domain::AppError(domain::ErrorCategory::Validation);
    }
    if (cancellationRequested && cancellationRequested()) {
        rollback();
        return domain::AppError::cancelled();
    }
    if (!m_database.commit()) {
        const domain::AppError error = databaseError(m_database);
        rollback();
        return error;
    }

    *report = {{QStringLiteral("json"), QString::fromUtf8(reportBytes)},
               {QStringLiteral("importedCounts"), importedCounts.toVariantMap()},
               {QStringLiteral("sourceSchemaVersion"), sourceVersion}};
    return domain::AppError::none();
}

} // namespace something::storage

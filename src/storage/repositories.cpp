#include "repositories.h"

#include "../domain/steam_game.h"
#include "../domain/steam_game_catalog.h"
#include "../domain/steam_collections.h"
#include "../domain/task_rules.h"
#include "../domain/task_history.h"
#include "../domain/universal_search.h"

#include <QDate>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QRegularExpression>
#include <QMap>
#include <QSet>
#include <QSqlError>
#include <QSqlDriver>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QThread>
#include <QVariant>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <optional>
#include <functional>
#include <initializer_list>
#include <utility>

namespace something::storage {
namespace {

// A default QString is SQL NULL, not the empty text required by optional
// NOT NULL columns. Nullable columns continue to use nullableString instead.
QString sqlText(const QString &value)
{
    return value.isNull() ? QStringLiteral("") : value;
}

template<typename T>
RepositoryResult<T> failure(domain::ErrorCategory category)
{
    RepositoryResult<T> result;
    result.error = domain::AppError(category);
    return result;
}

RepositoryResult<void> failureVoid(domain::ErrorCategory category)
{
    RepositoryResult<void> result;
    result.error = domain::AppError(category);
    return result;
}

domain::ErrorCategory queryFailure(const QSqlQuery &query)
{
    bool nativeCodeOk = false;
    const int nativeCode = query.lastError().nativeErrorCode().toInt(&nativeCodeOk);
    // SQLite's SQLITE_CONSTRAINT is 19; extended constraint result codes keep
    // the low byte equal to that base code.
    return nativeCodeOk && (nativeCode & 0xff) == 19
        ? domain::ErrorCategory::Conflict : domain::ErrorCategory::Storage;
}

bool validSource(const QString &source)
{
    return source == QStringLiteral("telegram") || source == QStringLiteral("youtube");
}

bool normalizeCategoryColor(QString value, QString *normalized)
{
    value = value.trimmed().toLower();
    if (value.isEmpty()) {
        normalized->clear();
        return true;
    }
    static const QRegularExpression sixDigit(QStringLiteral("^#[0-9a-f]{6}$"));
    static const QRegularExpression threeDigit(QStringLiteral("^#[0-9a-f]{3}$"));
    if (threeDigit.match(value).hasMatch()) {
        *normalized = QStringLiteral("#") + value[1] + value[1] + value[2] + value[2] + value[3] + value[3];
        return true;
    }
    if (!sixDigit.match(value).hasMatch()) return false;
    *normalized = std::move(value);
    return true;
}

QString normalizedName(const QString &value)
{
    return value.trimmed().toCaseFolded();
}

QString normalizedApplicationCategoryName(const QString &value)
{
    // Treat canonically equivalent Unicode spellings as the same category name
    // (for example, a precomposed character and its combining-mark form).
    return value.trimmed().normalized(QString::NormalizationForm_C)
        .toCaseFolded().normalized(QString::NormalizationForm_C);
}

QString sqlLikePattern(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    value.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return QStringLiteral("%") + value.toCaseFolded() + QStringLiteral("%");
}

QString taskHistoryTitleLikePattern(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    value.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return QStringLiteral("%") + value + QStringLiteral("%");
}

bool validAsset(const AssetMetadata &asset, const QStringList &allowedMimeTypes)
{
    if (asset.filename.isEmpty() || asset.filename == QStringLiteral(".")
        || asset.filename == QStringLiteral("..") || asset.filename.contains(QLatin1Char('/'))
        || asset.filename.contains(QLatin1Char('\\')) || asset.byteSize <= 0
        || !allowedMimeTypes.contains(asset.mimeType)) {
        return false;
    }
    return true;
}

bool validUtcInstant(const QString &value)
{
    if (value.isEmpty() || !value.endsWith(QLatin1Char('Z'))) return false;
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    return parsed.isValid() && parsed.timeSpec() == Qt::UTC;
}

bool validSteamNewsId(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9]{1,32}$"));
    return pattern.match(value).hasMatch();
}

bool validSteamNewsSourceUrl(const QString &value)
{
    if (value.isEmpty() || value.toUtf8().size() > 4096) return false;
    const QUrl url(value, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    return url.isValid()
        && (scheme == QStringLiteral("https") || scheme == QStringLiteral("http"))
        && !url.host().isEmpty() && url.userInfo().isEmpty() && url.fragment().isEmpty()
        && (url.port(-1) == -1
            || url.port(-1) == (scheme == QStringLiteral("https") ? 443 : 80));
}

bool validCachedSteamNewsArticle(const domain::SteamNewsArticle &article, quint32 appId)
{
    return appId != 0 && article.appId == appId && validSteamNewsId(article.newsId)
        && !article.title.trimmed().isEmpty() && article.title.size() <= 500
        && article.publishedAtUnixSeconds > 0
        && article.publishedAtUnixSeconds <= 253402300799LL
        && validSteamNewsSourceUrl(article.sourceUrl)
        && article.author.size() <= 500 && article.feedLabel.size() <= 200
        && article.feedName.size() <= 200
        && (!article.feedType || *article.feedType >= 0)
        && article.contentText.size()
            <= SteamGameUpdatesRepository::maximumArticleContentCharacters;
}

bool validSteamReleaseArtwork(quint32 appId,
                              const domain::SteamReleaseArtworkMetadata &artwork)
{
    if (!artwork.sourceUrl.isEmpty()
        && domain::normalizeSteamArtworkUrl(artwork.sourceUrl) != artwork.sourceUrl) {
        return false;
    }
    if (artwork.cachedFilename.isEmpty())
        return artwork.mimeType.isEmpty() && artwork.byteSize == 0;
    if (artwork.sourceUrl.isEmpty()) return false;
    static const QStringList allowedMimeTypes{
        QStringLiteral("image/jpeg"), QStringLiteral("image/png"), QStringLiteral("image/webp")};
    const AssetMetadata asset{artwork.cachedFilename, artwork.mimeType, artwork.byteSize};
    static const QRegularExpression filenamePattern(
        QStringLiteral("^[1-9][0-9]{0,9}-[a-f0-9]{64}\\.(jpg|png|webp)$"));
    const bool mimeMatchesExtension =
        (artwork.cachedFilename.endsWith(QStringLiteral(".jpg"))
         && artwork.mimeType == QStringLiteral("image/jpeg"))
        || (artwork.cachedFilename.endsWith(QStringLiteral(".png"))
            && artwork.mimeType == QStringLiteral("image/png"))
        || (artwork.cachedFilename.endsWith(QStringLiteral(".webp"))
            && artwork.mimeType == QStringLiteral("image/webp"));
    return appId != 0 && validAsset(asset, allowedMimeTypes)
        && artwork.byteSize <= 2LL * 1024 * 1024 && mimeMatchesExtension
        && filenamePattern.match(artwork.cachedFilename).hasMatch()
        && artwork.cachedFilename.startsWith(QString::number(appId) + QLatin1Char('-'));
}

bool validTrackedSteamRelease(const domain::SteamTrackedRelease &release)
{
    const domain::SteamStoreUrl normalized =
        domain::normalizeSteamStoreUrl(release.canonicalStoreUrl);
    domain::SteamReleaseDetails details;
    details.appId = release.appId;
    details.title = release.title;
    details.artworkUrl = release.artwork.sourceUrl;
    details.releaseDate = release.releaseDate;
    details.status = release.status;
    if (!normalized.isValid() || normalized.appId != release.appId
        || normalized.canonicalUrl != release.canonicalStoreUrl
        || !domain::isValidSteamReleaseDetails(details)
        || !validSteamReleaseArtwork(release.appId, release.artwork)
        || !validUtcInstant(release.followedAtUtc)) {
        return false;
    }

    switch (release.refreshState) {
    case domain::SteamReleaseRefreshState::NeverAttempted:
        return release.lastAttemptedUpdateAtUtc.isEmpty()
            && release.lastSuccessfulUpdateAtUtc.isEmpty() && release.refreshError.isEmpty();
    case domain::SteamReleaseRefreshState::Succeeded:
        return validUtcInstant(release.lastAttemptedUpdateAtUtc)
            && validUtcInstant(release.lastSuccessfulUpdateAtUtc)
            && release.refreshError.isEmpty();
    case domain::SteamReleaseRefreshState::Failed:
        return validUtcInstant(release.lastAttemptedUpdateAtUtc)
            && !release.refreshError.trimmed().isEmpty()
            && release.refreshError.size() <= 500
            && (release.lastSuccessfulUpdateAtUtc.isEmpty()
                || validUtcInstant(release.lastSuccessfulUpdateAtUtc));
    }
    return false;
}

class Transaction final {
public:
    explicit Transaction(QSqlDatabase &database) : m_database(database), m_active(database.transaction()) {}
    ~Transaction() { if (m_active) m_database.rollback(); }
    bool active() const { return m_active; }
    bool commit() { if (!m_active) return false; m_active = !m_database.commit(); return !m_active; }
private:
    QSqlDatabase &m_database;
    bool m_active{false};
};

QString placeholderList(int count)
{
    QStringList values;
    values.reserve(count);
    for (int i = 0; i < count; ++i) values.push_back(QStringLiteral("?"));
    return values.join(QLatin1Char(','));
}

QVariant nullableString(const QString &value)
{
    return value.isEmpty() ? QVariant() : QVariant(value);
}

std::optional<qint64> nullableInteger(const QVariant &value)
{
    return value.isNull() ? std::nullopt : std::optional<qint64>(value.toLongLong());
}

RepositoryResult<void> recordSteamPriceObservation(QSqlDatabase &database,
                                                   const SteamGame &game,
                                                   const QString &observedAtUtc)
{
    static const QRegularExpression countryPattern(QStringLiteral("^[A-Z]{2}$"));
    static const QRegularExpression currencyPattern(QStringLiteral("^[A-Z]{3}$"));
    const bool priced = game.priceStatus == QStringLiteral("priced");
    const bool free = game.priceStatus == QStringLiteral("free");
    const bool unavailable = game.priceStatus == QStringLiteral("unavailable");
    const bool unsupported = game.priceStatus == QStringLiteral("unsupported");
    if (game.id <= 0 || game.steamAppId <= 0
        || !countryPattern.match(game.priceCountryCode).hasMatch()
        || (!game.currency.isEmpty() && !currencyPattern.match(game.currency).hasMatch())
        || game.discountPercent < 0 || game.discountPercent > 100
        || (!priced && game.discountPercent != 0)
        || (priced && (game.currency.isEmpty() || !game.currentPriceMinor
                       || !game.regularPriceMinor))
        || (free && (game.currentPriceMinor != std::optional<qint64>(0)
                     || game.regularPriceMinor != std::optional<qint64>(0)))
        || ((unavailable || unsupported)
            && (game.currentPriceMinor || game.regularPriceMinor))) {
        return failureVoid(domain::ErrorCategory::Validation);
    }

    // Keep unavailable and unsupported checks as state-only observations; no price is inferred.
    QDateTime timestamp = QDateTime::fromString(observedAtUtc, Qt::ISODateWithMs);
    if (!timestamp.isValid()) timestamp = QDateTime::fromString(observedAtUtc, Qt::ISODate);
    if (!timestamp.isValid()) return failureVoid(domain::ErrorCategory::Validation);
    timestamp = timestamp.toUTC();
    const QString normalizedTimestamp = timestamp.toString(Qt::ISODateWithMs);
    const QString observationDay = timestamp.date().toString(Qt::ISODate);

    QSqlQuery previous(database);
    previous.prepare(QStringLiteral(
        "SELECT price_state,current_price_minor,regular_price_minor,discount_percent,"
        "date(observed_at_utc) FROM steam_price_observations "
        "WHERE steam_game_id=? AND country_code=? AND currency_code IS ? "
        "ORDER BY observed_at_utc DESC,id DESC LIMIT 1"));
    previous.addBindValue(game.id);
    previous.addBindValue(game.priceCountryCode);
    previous.addBindValue(nullableString(game.currency));
    if (!previous.exec()) return failureVoid(queryFailure(previous));

    bool unchangedToday = false;
    if (previous.next()) {
        unchangedToday = previous.value(0).toString() == game.priceStatus
            && nullableInteger(previous.value(1)) == game.currentPriceMinor
            && nullableInteger(previous.value(2)) == game.regularPriceMinor
            && previous.value(3).toInt() == game.discountPercent
            && previous.value(4).toString() == observationDay;
    }
    previous.finish();
    if (unchangedToday) return {};

    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT INTO steam_price_observations(steam_game_id,steam_app_id,country_code,"
        "currency_code,price_state,current_price_minor,regular_price_minor,"
        "discount_percent,observed_at_utc) VALUES(?,?,?,?,?,?,?,?,?)"));
    insert.addBindValue(game.id);
    insert.addBindValue(game.steamAppId);
    insert.addBindValue(game.priceCountryCode);
    insert.addBindValue(nullableString(game.currency));
    insert.addBindValue(game.priceStatus);
    insert.addBindValue(game.currentPriceMinor
        ? QVariant(*game.currentPriceMinor) : QVariant());
    insert.addBindValue(game.regularPriceMinor
        ? QVariant(*game.regularPriceMinor) : QVariant());
    insert.addBindValue(game.discountPercent);
    insert.addBindValue(normalizedTimestamp);
    if (!insert.exec()) return failureVoid(queryFailure(insert));
    return {};
}

domain::TaskHistoryValue historyValue(const QJsonValue &value)
{
    return domain::TaskHistoryValue::fromJsonValue(value);
}

domain::TaskHistoryValue historyString(const QString &value, bool nullable)
{
    if (nullable && value.isEmpty()) return domain::TaskHistoryValue::null();
    return historyValue(QJsonValue(value));
}

QJsonArray taskTagsJson(const QStringList &tags)
{
    QJsonArray result;
    for (const QString &tag : tags) result.push_back(tag);
    return result;
}

QJsonArray taskSubtasksJson(const QVector<Subtask> &subtasks)
{
    QJsonArray result;
    for (const Subtask &subtask : subtasks) {
        result.push_back(QJsonObject{
            {QStringLiteral("id"), QString::number(subtask.id)},
            {QStringLiteral("title"), subtask.title},
            {QStringLiteral("completed"), subtask.completed},
            {QStringLiteral("position"), subtask.position},
        });
    }
    return result;
}

QJsonObject subtaskHistoryJson(const Subtask &subtask)
{
    return {{QStringLiteral("id"), QString::number(subtask.id)},
            {QStringLiteral("title"), subtask.title},
            {QStringLiteral("completed"), subtask.completed},
            {QStringLiteral("position"), subtask.position}};
}

void appendHistoryChange(QVector<domain::TaskHistoryChange> *changes,
                         QString field, domain::TaskHistoryValue previous,
                         domain::TaskHistoryValue current)
{
    changes->push_back({std::move(field), std::move(previous), std::move(current)});
}

QVector<domain::TaskHistoryChange> taskChanges(const Task &before, const Task &after)
{
    QVector<domain::TaskHistoryChange> changes;
    if (before.title != after.title)
        appendHistoryChange(&changes, QStringLiteral("title"), historyString(before.title, false),
                           historyString(after.title, false));
    if (before.description != after.description)
        appendHistoryChange(&changes, QStringLiteral("description"), historyString(before.description, false),
                           historyString(after.description, false));
    if (before.dueDate != after.dueDate)
        appendHistoryChange(&changes, QStringLiteral("dueDate"), historyString(before.dueDate, true),
                           historyString(after.dueDate, true));
    if (before.priority != after.priority)
        appendHistoryChange(&changes, QStringLiteral("priority"), historyString(before.priority, false),
                           historyString(after.priority, false));
    if (before.difficulty != after.difficulty)
        appendHistoryChange(&changes, QStringLiteral("difficulty"), historyString(before.difficulty, true),
                           historyString(after.difficulty, true));
    if (before.completed != after.completed)
        appendHistoryChange(&changes, QStringLiteral("completed"),
                           historyValue(QJsonValue(before.completed)),
                           historyValue(QJsonValue(after.completed)));
    if (before.tags != after.tags)
        appendHistoryChange(&changes, QStringLiteral("tags"), historyValue(taskTagsJson(before.tags)),
                           historyValue(taskTagsJson(after.tags)));
    if (taskSubtasksJson(before.subtasks) != taskSubtasksJson(after.subtasks))
        appendHistoryChange(&changes, QStringLiteral("subtasks"),
                           historyValue(taskSubtasksJson(before.subtasks)),
                           historyValue(taskSubtasksJson(after.subtasks)));
    return changes;
}

QVector<domain::TaskHistoryChange> taskCreatedChanges(const Task &task)
{
    QVector<domain::TaskHistoryChange> changes;
    changes.push_back({QStringLiteral("title"), domain::TaskHistoryValue::unset(),
                       historyString(task.title, false)});
    changes.push_back({QStringLiteral("description"), domain::TaskHistoryValue::unset(),
                       historyString(task.description, false)});
    changes.push_back({QStringLiteral("dueDate"), domain::TaskHistoryValue::unset(),
                       historyString(task.dueDate, true)});
    changes.push_back({QStringLiteral("priority"), domain::TaskHistoryValue::unset(),
                       historyString(task.priority, false)});
    changes.push_back({QStringLiteral("difficulty"), domain::TaskHistoryValue::unset(),
                       historyString(task.difficulty, true)});
    changes.push_back({QStringLiteral("completed"), domain::TaskHistoryValue::unset(),
                       historyValue(QJsonValue(task.completed))});
    changes.push_back({QStringLiteral("tags"), domain::TaskHistoryValue::unset(),
                       historyValue(taskTagsJson(task.tags))});
    changes.push_back({QStringLiteral("subtasks"), domain::TaskHistoryValue::unset(),
                       historyValue(taskSubtasksJson(task.subtasks))});
    return changes;
}

QVector<domain::TaskHistoryChange> taskDeletedChanges(const Task &task)
{
    QVector<domain::TaskHistoryChange> changes = taskCreatedChanges(task);
    for (auto &change : changes) {
        change.previous = std::move(change.current);
        change.current = domain::TaskHistoryValue::unset();
    }
    return changes;
}

bool insertTaskHistory(QSqlDatabase &database, qint64 taskId,
                       domain::TaskHistoryEventType type,
                       std::optional<qint64> taskRevision,
                       const QString &taskTitle,
                       const QVector<domain::TaskHistoryChange> &changes,
                       domain::AppError *error)
{
    if (changes.isEmpty()) return true;
    const auto diff = domain::serializeTaskHistoryDiff(changes);
    if (!diff) {
        *error = domain::AppError(domain::ErrorCategory::Internal);
        return false;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO task_history(task_id,event_type,occurred_at_utc,task_revision,task_title,"
        "serialization_version,changed_fields_json,previous_values_json,new_values_json) "
        "VALUES(?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(taskId);
    query.addBindValue(domain::taskHistoryEventTypeName(type));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(taskRevision ? QVariant(*taskRevision) : QVariant());
    query.addBindValue(taskTitle);
    query.addBindValue(diff->version);
    query.addBindValue(diff->changedFieldsJson);
    query.addBindValue(diff->previousValuesJson);
    query.addBindValue(diff->newValuesJson);
    if (!query.exec()) {
        *error = domain::AppError(queryFailure(query));
        return false;
    }
    return true;
}

bool taskHistoryTableExists(QSqlDatabase &database, bool *exists,
                            domain::AppError *error)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='task_history'"))) {
        *error = domain::AppError(queryFailure(query));
        return false;
    }
    *exists = query.next();
    return true;
}

QVector<domain::TaskHistoryChange> taskScheduleHistoryChanges(
    const TaskSchedule &before, const TaskSchedule &after)
{
    QVector<domain::TaskHistoryChange> changes;
    if (before.plannedDate != after.plannedDate) {
        const domain::TaskHistoryValue oldValue = before.plannedDate
            ? historyValue(QJsonValue(before.plannedDate->toString(Qt::ISODate)))
            : domain::TaskHistoryValue::null();
        const domain::TaskHistoryValue newValue = after.plannedDate
            ? historyValue(QJsonValue(after.plannedDate->toString(Qt::ISODate)))
            : domain::TaskHistoryValue::null();
        appendHistoryChange(&changes, QStringLiteral("plannedDate"),
                            oldValue, newValue);
    }
    if (before.plannedStartTime != after.plannedStartTime) {
        const domain::TaskHistoryValue oldValue = before.plannedStartTime
            ? historyValue(QJsonValue(before.plannedStartTime->toString(QStringLiteral("HH:mm"))))
            : domain::TaskHistoryValue::null();
        const domain::TaskHistoryValue newValue = after.plannedStartTime
            ? historyValue(QJsonValue(after.plannedStartTime->toString(QStringLiteral("HH:mm"))))
            : domain::TaskHistoryValue::null();
        appendHistoryChange(&changes, QStringLiteral("plannedStartTime"),
                            oldValue, newValue);
    }
    if (before.estimatedDurationMinutes != after.estimatedDurationMinutes) {
        const domain::TaskHistoryValue oldValue = before.estimatedDurationMinutes
            ? historyValue(QJsonValue(*before.estimatedDurationMinutes))
            : domain::TaskHistoryValue::null();
        const domain::TaskHistoryValue newValue = after.estimatedDurationMinutes
            ? historyValue(QJsonValue(*after.estimatedDurationMinutes))
            : domain::TaskHistoryValue::null();
        appendHistoryChange(&changes, QStringLiteral("estimatedDurationMinutes"),
                            oldValue, newValue);
    }
    return changes;
}

bool sameScheduleValues(const TaskSchedule &left, const TaskSchedule &right)
{
    return left.taskId == right.taskId
        && left.plannedDate == right.plannedDate
        && left.plannedStartTime == right.plannedStartTime
        && left.estimatedDurationMinutes == right.estimatedDurationMinutes;
}

domain::TaskHistoryEventType taskUpdateEventType(
    const Task &before, const Task &after,
    const QVector<domain::TaskHistoryChange> &changes)
{
    if (changes.size() == 1) {
        const QString &field = changes.front().field;
        if (field == QStringLiteral("title")) return domain::TaskHistoryEventType::TaskTitleChanged;
        if (field == QStringLiteral("description")) return domain::TaskHistoryEventType::DescriptionChanged;
        if (field == QStringLiteral("dueDate")) return domain::TaskHistoryEventType::DueDateChanged;
        if (field == QStringLiteral("priority")) return domain::TaskHistoryEventType::PriorityChanged;
        if (field == QStringLiteral("difficulty")) return domain::TaskHistoryEventType::DifficultyChanged;
        if (field == QStringLiteral("tags")) return domain::TaskHistoryEventType::TagsChanged;
        if (field == QStringLiteral("subtasks")) {
            QVector<const Subtask *> added;
            QVector<const Subtask *> removed;
            QVector<QPair<const Subtask *, const Subtask *>> modified;
            for (const Subtask &oldSubtask : before.subtasks) {
                const auto found = std::find_if(after.subtasks.cbegin(), after.subtasks.cend(),
                    [&oldSubtask](const Subtask &candidate) { return candidate.id == oldSubtask.id; });
                if (found == after.subtasks.cend()) {
                    removed.push_back(&oldSubtask);
                } else {
                    if (oldSubtask.title != found->title || oldSubtask.completed != found->completed)
                        modified.push_back({&oldSubtask, &*found});
                }
            }
            for (const Subtask &newSubtask : after.subtasks) {
                const auto found = std::find_if(before.subtasks.cbegin(), before.subtasks.cend(),
                    [&newSubtask](const Subtask &candidate) { return candidate.id == newSubtask.id; });
                if (found == before.subtasks.cend()) added.push_back(&newSubtask);
            }
            if (added.size() == 1 && removed.isEmpty() && modified.isEmpty())
                return domain::TaskHistoryEventType::SubtaskCreated;
            if (removed.size() == 1 && added.isEmpty() && modified.isEmpty())
                return domain::TaskHistoryEventType::SubtaskRemoved;
            if (added.isEmpty() && removed.isEmpty() && modified.size() == 1) {
                const Subtask *oldSubtask = modified.front().first;
                const Subtask *newSubtask = modified.front().second;
                if (oldSubtask->title == newSubtask->title
                    && oldSubtask->position == newSubtask->position
                    && oldSubtask->completed != newSubtask->completed) {
                    return newSubtask->completed
                        ? domain::TaskHistoryEventType::SubtaskCompleted
                        : domain::TaskHistoryEventType::SubtaskReopened;
                }
                return domain::TaskHistoryEventType::SubtaskUpdated;
            }
            if (added.isEmpty() && removed.isEmpty() && modified.isEmpty())
                return domain::TaskHistoryEventType::SubtaskUpdated;
        }
    }
    return domain::TaskHistoryEventType::TaskUpdated;
}

void bindAsset(QSqlQuery &query, int firstIndex, const AssetMetadata &asset)
{
    query.bindValue(firstIndex, asset.filename);
    query.bindValue(firstIndex + 1, asset.mimeType);
    query.bindValue(firstIndex + 2, asset.byteSize);
}

RepositoryResult<void> upsertSteamCatalogDetails(
    QSqlDatabase &database, domain::SteamGameCatalogDetails details)
{
    if (domain::normalizeSteamGameCatalogDetails(details).isError())
        return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "INSERT INTO steam_game_catalog(steam_app_id,store_url,name,artwork_source_url) "
        "VALUES(?,?,?,?) ON CONFLICT(steam_app_id) DO UPDATE SET "
        "store_url=excluded.store_url,name=excluded.name,"
        "artwork_filename=CASE WHEN excluded.artwork_source_url<>'' AND "
        "steam_game_catalog.artwork_source_url<>excluded.artwork_source_url "
        "THEN '' ELSE steam_game_catalog.artwork_filename END,"
        "artwork_mime_type=CASE WHEN excluded.artwork_source_url<>'' AND "
        "steam_game_catalog.artwork_source_url<>excluded.artwork_source_url "
        "THEN '' ELSE steam_game_catalog.artwork_mime_type END,"
        "artwork_byte_size=CASE WHEN excluded.artwork_source_url<>'' AND "
        "steam_game_catalog.artwork_source_url<>excluded.artwork_source_url "
        "THEN 0 ELSE steam_game_catalog.artwork_byte_size END,"
        "artwork_source_url=CASE WHEN excluded.artwork_source_url<>'' "
        "THEN excluded.artwork_source_url ELSE steam_game_catalog.artwork_source_url END,"
        "updated_at=CURRENT_TIMESTAMP"));
    query.addBindValue(details.appId);
    query.addBindValue(details.canonicalStoreUrl);
    query.addBindValue(details.name);
    query.addBindValue(sqlText(details.artworkUrl));
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<void> setSteamCatalogArtwork(
    QSqlDatabase &database, quint32 appId, const QString &expectedSourceUrl,
    const domain::SteamGameCatalogArtwork &artwork)
{
    if (appId == 0 || expectedSourceUrl.isEmpty()
        || artwork.sourceUrl != expectedSourceUrl
        || !domain::isValidSteamGameCatalogArtwork(appId, artwork)) {
        return failureVoid(domain::ErrorCategory::Validation);
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE steam_game_catalog SET artwork_filename=?,artwork_mime_type=?,"
        "artwork_byte_size=?,updated_at=CURRENT_TIMESTAMP "
        "WHERE steam_app_id=? AND artwork_source_url=?"));
    query.addBindValue(sqlText(artwork.cachedFilename));
    query.addBindValue(sqlText(artwork.mimeType));
    query.addBindValue(artwork.byteSize);
    query.addBindValue(appId);
    query.addBindValue(expectedSourceUrl);
    if (!query.exec()) return failureVoid(queryFailure(query));
    if (query.numRowsAffected() == 1) return {};
    query.finish();
    QSqlQuery exists(database);
    exists.prepare(QStringLiteral("SELECT 1 FROM steam_game_catalog WHERE steam_app_id=?"));
    exists.addBindValue(appId);
    if (!exists.exec()) return failureVoid(queryFailure(exists));
    return failureVoid(exists.next() ? domain::ErrorCategory::Conflict
                                     : domain::ErrorCategory::NotFound);
}

bool decodeSteamCollectionGame(const QSqlQuery &query,
                               domain::SteamCollectionGameEntry *entry)
{
    entry->collectionId = query.value(0).toLongLong();
    const QString status = query.value(1).toString();
    if (!status.isEmpty()) {
        entry->status = domain::steamCollectionGameStatusFromName(status);
        if (!entry->status) return false;
    }
    if (!query.value(2).isNull()) entry->personalRating = query.value(2).toInt();
    entry->personalComment = query.value(3).toString();
    entry->createdAt = query.value(4).toString();
    entry->updatedAt = query.value(5).toString();
    entry->game.appId = query.value(6).toUInt();
    entry->game.canonicalStoreUrl = query.value(7).toString();
    entry->game.name = query.value(8).toString();
    entry->game.artwork.sourceUrl = query.value(9).toString();
    entry->game.artwork.cachedFilename = query.value(10).toString();
    entry->game.artwork.mimeType = query.value(11).toString();
    entry->game.artwork.byteSize = query.value(12).toLongLong();
    entry->game.createdAt = query.value(13).toString();
    entry->game.updatedAt = query.value(14).toString();
    return domain::isValidSteamCollectionGameEntry(*entry);
}

QString steamCollectionGameSelect()
{
    return QStringLiteral(
        "SELECT cg.collection_id,cg.status,cg.personal_rating,cg.personal_comment,"
        "cg.created_at,cg.updated_at,c.steam_app_id,c.store_url,c.name,"
        "c.artwork_source_url,c.artwork_filename,c.artwork_mime_type,c.artwork_byte_size,"
        "c.created_at,c.updated_at FROM steam_collection_games cg "
        "JOIN steam_game_catalog c ON c.steam_app_id=cg.steam_app_id "
        "JOIN steam_collections col ON col.id=cg.collection_id");
}

} // namespace

TaskRepository::TaskRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<Task> TaskRepository::get(qint64 id) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    return load(id);
}

RepositoryResult<Task> TaskRepository::load(qint64 id) const
{
    if (id <= 0) return failure<Task>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT id,title,description,is_completed,created_at,updated_at,due_date,priority,difficulty,revision FROM tasks WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec()) return failure<Task>(queryFailure(query));
    if (!query.next()) return failure<Task>(domain::ErrorCategory::NotFound);

    Task task;
    task.id = query.value(0).toLongLong();
    task.title = query.value(1).toString();
    task.description = query.value(2).toString();
    task.completed = query.value(3).toBool();
    task.createdAt = query.value(4).toString();
    task.updatedAt = query.value(5).toString();
    task.dueDate = query.value(6).toString();
    task.priority = query.value(7).toString();
    task.difficulty = query.value(8).toString();
    task.revision = query.value(9).toLongLong();
    query.finish();

    QSqlQuery tags(m_database);
    tags.prepare(QStringLiteral("SELECT g.name FROM task_tags x JOIN tags g ON g.id=x.tag_id WHERE x.task_id=? ORDER BY g.name_normalized, g.id"));
    tags.addBindValue(id);
    if (!tags.exec()) return failure<Task>(queryFailure(tags));
    while (tags.next()) task.tags.push_back(tags.value(0).toString());

    QSqlQuery subtasks(m_database);
    subtasks.prepare(QStringLiteral("SELECT id,title,is_completed,position FROM task_subtasks WHERE task_id=? ORDER BY position,id"));
    subtasks.addBindValue(id);
    if (!subtasks.exec()) return failure<Task>(queryFailure(subtasks));
    while (subtasks.next()) {
        task.subtasks.push_back(Subtask{subtasks.value(0).toLongLong(), subtasks.value(1).toString(),
                                        subtasks.value(2).toBool(), subtasks.value(3).toInt()});
    }
    const auto automation = TaskAutomationRepository(m_database).getForTask(id);
    if (!automation.succeeded()) return failure<Task>(automation.error.category());
    task.automation = automation.value;
    RepositoryResult<Task> result;
    result.value = std::move(task);
    return result;
}

RepositoryResult<QVector<Task>> TaskRepository::list(const TaskFilter &requested) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    TaskFilter filter = requested;
    if (domain::normalizeTaskFilter(filter).isError()) {
        return failure<QVector<Task>>(domain::ErrorCategory::Validation);
    }
    static const QMap<QString, QString> orderExpressions{
        {QStringLiteral("created_at"), QStringLiteral("created_at %1")},
        {QStringLiteral("updated_at"), QStringLiteral("updated_at %1")},
        {QStringLiteral("due_date"), QStringLiteral("CASE WHEN due_date IS NULL THEN 1 ELSE 0 END %1, due_date %1")},
        {QStringLiteral("priority"), QStringLiteral("CASE priority WHEN 'high' THEN 0 WHEN 'medium' THEN 1 WHEN 'low' THEN 2 ELSE 3 END %1")},
        {QStringLiteral("title"), QStringLiteral("title COLLATE NOCASE %1")},
        {QStringLiteral("id"), QStringLiteral("id %1")},
    };

    QString sql = QStringLiteral("SELECT id FROM tasks WHERE 1=1");
    QVariantList binds;
    if (filter.completion == QStringLiteral("complete")) sql += QStringLiteral(" AND is_completed=1");
    if (filter.completion == QStringLiteral("incomplete")) sql += QStringLiteral(" AND is_completed=0");
    if (!filter.priority.isEmpty()) { sql += QStringLiteral(" AND priority=?"); binds << filter.priority; }
    if (filter.difficulty == QStringLiteral("unset")) sql += QStringLiteral(" AND (difficulty IS NULL OR difficulty='')");
    else if (!filter.difficulty.isEmpty()) { sql += QStringLiteral(" AND difficulty=?"); binds << filter.difficulty; }
    if (!filter.query.isEmpty()) {
        sql += QStringLiteral(" AND (lower(title) LIKE ? ESCAPE '\\' OR lower(description) LIKE ? ESCAPE '\\' "
                              "OR EXISTS (SELECT 1 FROM task_tags x JOIN tags g ON g.id=x.tag_id "
                              "WHERE x.task_id=tasks.id AND g.name_normalized LIKE ? ESCAPE '\\') "
                              "OR EXISTS (SELECT 1 FROM task_subtasks s WHERE s.task_id=tasks.id "
                              "AND lower(s.title) LIKE ? ESCAPE '\\'))");
        binds << sqlLikePattern(filter.query) << sqlLikePattern(filter.query)
              << sqlLikePattern(filter.query) << sqlLikePattern(filter.query);
    }
    for (const QString &tag : filter.tags) {
        sql += QStringLiteral(" AND EXISTS (SELECT 1 FROM task_tags tx JOIN tags tg ON tg.id=tx.tag_id "
                              "WHERE tx.task_id=tasks.id AND tg.name_normalized=?)");
        binds << tag.toCaseFolded();
    }
    if (!filter.dueDate.isEmpty()) { sql += QStringLiteral(" AND due_date=?"); binds << filter.dueDate; }
    if (!filter.dueFrom.isEmpty()) { sql += QStringLiteral(" AND due_date>=?"); binds << filter.dueFrom; }
    if (!filter.dueTo.isEmpty()) { sql += QStringLiteral(" AND due_date<=?"); binds << filter.dueTo; }
    if (filter.overdue) sql += QStringLiteral(" AND due_date<date('now','localtime') AND is_completed=0");
    if (filter.undated) sql += QStringLiteral(" AND due_date IS NULL");
    sql += QStringLiteral(" ORDER BY %1, id %2 LIMIT ? OFFSET ?")
               .arg(orderExpressions.value(filter.sort).arg(filter.direction), filter.direction);
    binds << filter.limit << filter.offset;

    QSqlQuery query(m_database);
    query.prepare(sql);
    for (const auto &bind : binds) query.addBindValue(bind);
    if (!query.exec()) return failure<QVector<Task>>(queryFailure(query));
    QVector<qint64> ids;
    while (query.next()) ids.push_back(query.value(0).toLongLong());
    query.finish();

    RepositoryResult<QVector<Task>> result;
    result.value.reserve(ids.size());
    for (qint64 id : ids) {
        const auto item = load(id);
        if (!item.succeeded()) return failure<QVector<Task>>(item.error.category());
        result.value.push_back(item.value);
    }
    return result;
}

bool TaskRepository::replaceTags(qint64 taskId, const QStringList &tags, domain::AppError *error)
{
    QSqlQuery removeLinks(m_database);
    removeLinks.prepare(QStringLiteral("DELETE FROM task_tags WHERE task_id=?"));
    removeLinks.addBindValue(taskId);
    if (!removeLinks.exec()) { *error = domain::AppError(queryFailure(removeLinks)); return false; }
    for (const QString &tag : tags) {
        const QString normalized = normalizedName(tag);
        QSqlQuery insert(m_database);
        insert.prepare(QStringLiteral("INSERT INTO tags(name,name_normalized) VALUES(?,?) ON CONFLICT(name_normalized) DO NOTHING"));
        insert.addBindValue(tag.trimmed());
        insert.addBindValue(normalized);
        if (!insert.exec()) { *error = domain::AppError(queryFailure(insert)); return false; }
        QSqlQuery link(m_database);
        link.prepare(QStringLiteral("INSERT INTO task_tags(task_id,tag_id) SELECT ?,id FROM tags WHERE name_normalized=?"));
        link.addBindValue(taskId);
        link.addBindValue(normalized);
        if (!link.exec()) { *error = domain::AppError(queryFailure(link)); return false; }
    }
    QSqlQuery prune(m_database);
    if (!prune.exec(QStringLiteral("DELETE FROM tags WHERE NOT EXISTS (SELECT 1 FROM task_tags WHERE task_tags.tag_id=tags.id)"))) {
        *error = domain::AppError(queryFailure(prune)); return false;
    }
    return true;
}

bool TaskRepository::replaceSubtasks(qint64 taskId, const QVector<SubtaskInput> &subtasks,
                                     domain::AppError *error)
{
    QSet<qint64> retained;
    for (int position = 0; position < subtasks.size(); ++position) {
        const auto &subtask = subtasks[position];
        QSqlQuery query(m_database);
        if (subtask.id == 0) {
            query.prepare(QStringLiteral("INSERT INTO task_subtasks(task_id,title,is_completed,position) VALUES(?,?,?,?)"));
            query.addBindValue(taskId);
            query.addBindValue(subtask.title.trimmed());
            query.addBindValue(subtask.completed);
            query.addBindValue(position);
        } else {
            query.prepare(QStringLiteral("UPDATE task_subtasks SET title=?,is_completed=?,position=? WHERE task_id=? AND id=?"));
            query.addBindValue(subtask.title.trimmed());
            query.addBindValue(subtask.completed);
            query.addBindValue(position);
            query.addBindValue(taskId);
            query.addBindValue(subtask.id);
        }
        if (!query.exec()) { *error = domain::AppError(queryFailure(query)); return false; }
        if (subtask.id > 0 && query.numRowsAffected() != 1) { *error = domain::AppError(domain::ErrorCategory::Conflict); return false; }
        if (subtask.id > 0) retained.insert(subtask.id);
        else retained.insert(query.lastInsertId().toLongLong());
    }
    QSqlQuery existing(m_database);
    existing.prepare(QStringLiteral("SELECT id FROM task_subtasks WHERE task_id=?"));
    existing.addBindValue(taskId);
    if (!existing.exec()) { *error = domain::AppError(queryFailure(existing)); return false; }
    QVector<qint64> removeIds;
    while (existing.next()) {
        const qint64 id = existing.value(0).toLongLong();
        if (!retained.contains(id)) removeIds.push_back(id);
    }
    existing.finish();
    for (qint64 id : removeIds) {
        QSqlQuery remove(m_database);
        remove.prepare(QStringLiteral("DELETE FROM task_subtasks WHERE task_id=? AND id=?"));
        remove.addBindValue(taskId);
        remove.addBindValue(id);
        if (!remove.exec()) { *error = domain::AppError(queryFailure(remove)); return false; }
    }
    return true;
}

RepositoryResult<Task> TaskRepository::create(const TaskInput &requested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    TaskInput input = requested;
    if (!input.difficulty) input.difficulty = QString{};
    if (domain::normalizeTaskInput(input).isError()) return failure<Task>(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<Task>(domain::ErrorCategory::Storage);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO tasks(title,description,due_date,priority,difficulty,revision) VALUES(?,?,?,?,?,1)"));
    query.addBindValue(input.title);
    query.addBindValue(input.description);
    query.addBindValue(nullableString(input.dueDate));
    query.addBindValue(input.priority);
    query.addBindValue(nullableString(input.difficulty.value_or(QString{})));
    if (!query.exec()) return failure<Task>(queryFailure(query));
    const qint64 id = query.lastInsertId().toLongLong();
    domain::AppError error;
    if (input.tags && !replaceTags(id, *input.tags, &error)) return failure<Task>(error.category());
    if (input.subtasks && !replaceSubtasks(id, *input.subtasks, &error)) return failure<Task>(error.category());
    if (input.automation && input.automation->configured) {
        const auto savedAutomation = TaskAutomationRepository(m_database).saveForTask(id, *input.automation);
        if (!savedAutomation.succeeded()) return failure<Task>(savedAutomation.error.category());
    }
    const auto created = load(id);
    if (!created.succeeded()) return failure<Task>(created.error.category());
    if (!insertTaskHistory(m_database, id, domain::TaskHistoryEventType::TaskCreated,
                           created.value.revision, created.value.title,
                           taskCreatedChanges(created.value), &error)) {
        return failure<Task>(error.category());
    }
    if (!transaction.commit()) return failure<Task>(domain::ErrorCategory::Storage);
    return created;
}

RepositoryResult<Task> TaskRepository::update(qint64 id, qint64 expectedRevision, const TaskInput &requested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (id <= 0 || expectedRevision <= 0) return failure<Task>(domain::ErrorCategory::Validation);
    TaskInput input = requested;
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<Task>(domain::ErrorCategory::Storage);
    const auto current = load(id);
    if (!current.succeeded()) return failure<Task>(current.error.category());
    if (current.value.revision != expectedRevision) return failure<Task>(domain::ErrorCategory::Conflict);
    if (!input.difficulty) input.difficulty = current.value.difficulty;
    if (domain::normalizeTaskInput(input).isError()) return failure<Task>(domain::ErrorCategory::Validation);
    if (input.difficulty.value_or(QString{}) != QStringLiteral("hard") && !current.value.subtasks.isEmpty()
        && (!input.subtasks || !input.subtasks->isEmpty())) {
        return failure<Task>(domain::ErrorCategory::Validation);
    }
    QSqlQuery updateQuery(m_database);
    updateQuery.prepare(QStringLiteral("UPDATE tasks SET title=?,description=?,due_date=?,priority=?,difficulty=?,revision=revision+1,updated_at=CURRENT_TIMESTAMP WHERE id=? AND revision=?"));
    updateQuery.addBindValue(input.title);
    updateQuery.addBindValue(input.description);
    updateQuery.addBindValue(nullableString(input.dueDate));
    updateQuery.addBindValue(input.priority);
    updateQuery.addBindValue(nullableString(input.difficulty.value_or(QString{})));
    updateQuery.addBindValue(id);
    updateQuery.addBindValue(expectedRevision);
    if (!updateQuery.exec()) return failure<Task>(queryFailure(updateQuery));
    if (updateQuery.numRowsAffected() != 1) return failure<Task>(domain::ErrorCategory::Conflict);
    domain::AppError error;
    if (input.tags && !replaceTags(id, *input.tags, &error)) return failure<Task>(error.category());
    if (input.subtasks && !replaceSubtasks(id, *input.subtasks, &error)) return failure<Task>(error.category());
    if (input.automation) {
        TaskAutomationRepository automations(m_database);
        if (input.automation->configured) {
            const auto savedAutomation = automations.saveForTask(id, *input.automation);
            if (!savedAutomation.succeeded()) return failure<Task>(savedAutomation.error.category());
        } else {
            const auto removedAutomation = automations.removeForTask(id);
            if (!removedAutomation.succeeded()) return failure<Task>(removedAutomation.error.category());
        }
    }
    const auto updated = load(id);
    if (!updated.succeeded()) return failure<Task>(updated.error.category());
    const auto changes = taskChanges(current.value, updated.value);
    if (!changes.isEmpty()
        && !insertTaskHistory(m_database, id,
                              taskUpdateEventType(current.value, updated.value, changes),
                              updated.value.revision, updated.value.title, changes, &error)) {
        return failure<Task>(error.category());
    }
    if (!transaction.commit()) return failure<Task>(domain::ErrorCategory::Storage);
    return updated;
}

RepositoryResult<Task> TaskRepository::setCompleted(qint64 id, qint64 expectedRevision, bool completed)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (id <= 0 || expectedRevision <= 0) return failure<Task>(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<Task>(domain::ErrorCategory::Storage);
    const auto current = load(id);
    if (!current.succeeded()) return failure<Task>(current.error.category());
    if (current.value.revision != expectedRevision) return failure<Task>(domain::ErrorCategory::Conflict);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE tasks SET is_completed=?,revision=revision+1,updated_at=CURRENT_TIMESTAMP WHERE id=? AND revision=?"));
    query.addBindValue(completed);
    query.addBindValue(id);
    query.addBindValue(expectedRevision);
    if (!query.exec()) return failure<Task>(queryFailure(query));
    if (query.numRowsAffected() != 1) return failure<Task>(domain::ErrorCategory::Conflict);
    const auto updated = load(id);
    if (!updated.succeeded()) return failure<Task>(updated.error.category());
    if (current.value.completed != completed) {
        const QVector<domain::TaskHistoryChange> changes{{
            QStringLiteral("completed"), historyValue(QJsonValue(current.value.completed)),
            historyValue(QJsonValue(completed))}};
        const auto type = completed ? domain::TaskHistoryEventType::TaskCompleted
                                    : domain::TaskHistoryEventType::TaskReopened;
        domain::AppError error;
        if (!insertTaskHistory(m_database, id, type, updated.value.revision,
                               updated.value.title, changes, &error)) {
            return failure<Task>(error.category());
        }
    }
    if (!transaction.commit()) return failure<Task>(domain::ErrorCategory::Storage);
    RepositoryResult<Task> result;
    result.value = updated.value;
    result.value.previousCompleted = current.value.completed;
    return result;
}

RepositoryResult<Task> TaskRepository::setSubtaskCompleted(qint64 id, qint64 subtaskId,
                                                            qint64 expectedRevision, bool completed)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (id <= 0 || subtaskId <= 0 || expectedRevision <= 0) return failure<Task>(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<Task>(domain::ErrorCategory::Storage);
    const auto task = load(id);
    if (!task.succeeded()) return failure<Task>(task.error.category());
    if (task.value.revision != expectedRevision) return failure<Task>(domain::ErrorCategory::Conflict);
    if (task.value.difficulty != QStringLiteral("hard")) return failure<Task>(domain::ErrorCategory::Validation);
    const Subtask *previousSubtask = nullptr;
    for (const Subtask &candidate : task.value.subtasks) {
        if (candidate.id == subtaskId) {
            previousSubtask = &candidate;
            break;
        }
    }
    QSqlQuery subtask(m_database);
    subtask.prepare(QStringLiteral("UPDATE task_subtasks SET is_completed=? WHERE task_id=? AND id=?"));
    subtask.addBindValue(completed);
    subtask.addBindValue(id);
    subtask.addBindValue(subtaskId);
    if (!subtask.exec()) return failure<Task>(queryFailure(subtask));
    if (subtask.numRowsAffected() != 1) return failure<Task>(domain::ErrorCategory::NotFound);
    QSqlQuery parent(m_database);
    parent.prepare(QStringLiteral("UPDATE tasks SET revision=revision+1,updated_at=CURRENT_TIMESTAMP WHERE id=? AND revision=?"));
    parent.addBindValue(id);
    parent.addBindValue(expectedRevision);
    if (!parent.exec()) return failure<Task>(queryFailure(parent));
    if (parent.numRowsAffected() != 1) return failure<Task>(domain::ErrorCategory::Conflict);
    const auto updated = load(id);
    if (!updated.succeeded()) return failure<Task>(updated.error.category());
    if (previousSubtask && previousSubtask->completed != completed) {
        const QString field = QStringLiteral("subtasks.%1").arg(subtaskId);
        Subtask updatedSubtask = *previousSubtask;
        updatedSubtask.completed = completed;
        const QVector<domain::TaskHistoryChange> changes{{
            field, historyValue(subtaskHistoryJson(*previousSubtask)),
            historyValue(subtaskHistoryJson(updatedSubtask))}};
        const auto type = completed ? domain::TaskHistoryEventType::SubtaskCompleted
                                    : domain::TaskHistoryEventType::SubtaskReopened;
        domain::AppError error;
        if (!insertTaskHistory(m_database, id, type, updated.value.revision,
                               updated.value.title, changes, &error)) {
            return failure<Task>(error.category());
        }
    }
    if (!transaction.commit()) return failure<Task>(domain::ErrorCategory::Storage);
    return updated;
}

RepositoryResult<void> TaskRepository::remove(qint64 id, qint64 expectedRevision)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (id <= 0 || expectedRevision <= 0) return failureVoid(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    const auto task = load(id);
    if (!task.succeeded()) return failureVoid(task.error.category());
    if (task.value.revision != expectedRevision) return failureVoid(domain::ErrorCategory::Conflict);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM tasks WHERE id=? AND revision=?"));
    query.addBindValue(id);
    query.addBindValue(expectedRevision);
    if (!query.exec()) return failureVoid(queryFailure(query));
    if (query.numRowsAffected() != 1) return failureVoid(domain::ErrorCategory::Conflict);
    domain::AppError error;
    if (!insertTaskHistory(m_database, id, domain::TaskHistoryEventType::TaskDeleted,
                           task.value.revision, task.value.title,
                           taskDeletedChanges(task.value), &error)) {
        return failureVoid(error.category());
    }
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

TaskAutomationRepository::TaskAutomationRepository(QSqlDatabase &database)
    : m_database(database)
{
}

RepositoryResult<std::optional<TaskAutomation>> TaskAutomationRepository::getForTask(
    qint64 taskId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (taskId <= 0)
        return failure<std::optional<TaskAutomation>>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT a.id,a.task_id,a.enabled,a.trigger_type,a.action_type,a.target_id,a.url,"
        "a.notification_message,a.delay_ms,a.revision,a.created_at,a.updated_at,"
        "COALESCE((SELECT e.status FROM task_automation_executions e "
        "WHERE e.task_id=a.task_id ORDER BY e.rowid DESC LIMIT 1),''),"
        "COALESCE((SELECT e.error_message FROM task_automation_executions e "
        "WHERE e.task_id=a.task_id ORDER BY e.rowid DESC LIMIT 1),''),"
        "COALESCE((SELECT e.executed_at_utc FROM task_automation_executions e "
        "WHERE e.task_id=a.task_id ORDER BY e.rowid DESC LIMIT 1),'') "
        "FROM task_automations a WHERE a.task_id=?"));
    query.addBindValue(taskId);
    if (!query.exec())
        return failure<std::optional<TaskAutomation>>(queryFailure(query));
    RepositoryResult<std::optional<TaskAutomation>> result;
    if (query.next()) {
        TaskAutomation automation;
        automation.id = query.value(0).toLongLong();
        automation.taskId = query.value(1).toLongLong();
        automation.enabled = query.value(2).toBool();
        automation.triggerType = query.value(3).toString();
        automation.actionType = query.value(4).toString();
        automation.targetId = query.value(5).toLongLong();
        automation.url = query.value(6).toString();
        automation.notificationMessage = query.value(7).toString();
        automation.delayMs = query.value(8).toInt();
        automation.revision = query.value(9).toLongLong();
        automation.createdAt = query.value(10).toString();
        automation.updatedAt = query.value(11).toString();
        automation.latestExecutionStatus = query.value(12).toString();
        automation.latestExecutionError = query.value(13).toString();
        automation.latestExecutionAt = query.value(14).toString();
        result.value = std::move(automation);
    }
    return result;
}

RepositoryResult<TaskAutomation> TaskAutomationRepository::saveForTask(
    qint64 taskId, const TaskAutomationInput &requested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (taskId <= 0) return failure<TaskAutomation>(domain::ErrorCategory::Validation);
    TaskAutomationInput input = requested;
    if (domain::normalizeTaskAutomation(input).isError() || !input.configured)
        return failure<TaskAutomation>(domain::ErrorCategory::Validation);

    const auto previous = getForTask(taskId);
    if (!previous.succeeded()) return failure<TaskAutomation>(previous.error.category());
    if (previous.value) {
        const TaskAutomation &saved = *previous.value;
        if (saved.enabled == input.enabled && saved.triggerType == input.triggerType
            && saved.actionType == input.actionType && saved.targetId == input.targetId
            && saved.url == input.url
            && saved.notificationMessage == input.notificationMessage
            && saved.delayMs == input.delayMs) {
            RepositoryResult<TaskAutomation> unchanged;
            unchanged.value = saved;
            return unchanged;
        }
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO task_automations(task_id,enabled,trigger_type,action_type,target_id,url,"
        "notification_message,delay_ms,revision) VALUES(?,?,?,?,?,?,?,?,1) "
        "ON CONFLICT(task_id) DO UPDATE SET enabled=excluded.enabled,"
        "trigger_type=excluded.trigger_type,action_type=excluded.action_type,"
        "target_id=excluded.target_id,url=excluded.url,"
        "notification_message=excluded.notification_message,delay_ms=excluded.delay_ms,"
        "revision=task_automations.revision+1,updated_at=CURRENT_TIMESTAMP"));
    query.addBindValue(taskId);
    query.addBindValue(input.enabled);
    query.addBindValue(input.triggerType);
    query.addBindValue(input.actionType);
    query.addBindValue(input.targetId > 0 ? QVariant(input.targetId) : QVariant());
    query.addBindValue(input.url.isEmpty() ? QVariant() : QVariant(input.url));
    query.addBindValue(input.notificationMessage);
    query.addBindValue(input.delayMs);
    if (!query.exec()) return failure<TaskAutomation>(queryFailure(query));
    const auto saved = getForTask(taskId);
    if (!saved.succeeded()) return failure<TaskAutomation>(saved.error.category());
    if (!saved.value) return failure<TaskAutomation>(domain::ErrorCategory::Storage);
    RepositoryResult<TaskAutomation> result;
    result.value = *saved.value;
    return result;
}

RepositoryResult<void> TaskAutomationRepository::removeForTask(qint64 taskId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (taskId <= 0) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM task_automations WHERE task_id=?"));
    query.addBindValue(taskId);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<bool> TaskAutomationRepository::insertExecution(
    const TaskAutomationExecution &execution)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    static const QSet<QString> actions{
        QStringLiteral("launch_application"), QStringLiteral("launch_setup"),
        QStringLiteral("open_url"), QStringLiteral("show_notification")};
    if (execution.executionId.isEmpty() || execution.automationId <= 0
        || execution.taskId <= 0 || execution.completionEventId.isEmpty()
        || execution.targetReference.isEmpty() || !actions.contains(execution.actionType)
        || execution.configurationRevision <= 0)
        return failure<bool>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO task_automation_executions(execution_id,automation_id,task_id,"
        "completion_event_id,action_type,target_reference,requested_at_utc,status,error_message,"
        "configuration_revision) VALUES(?,?,?,?,?,?,?,'pending','',?) "
        "ON CONFLICT(completion_event_id) DO NOTHING"));
    query.addBindValue(execution.executionId);
    query.addBindValue(execution.automationId);
    query.addBindValue(execution.taskId);
    query.addBindValue(execution.completionEventId);
    query.addBindValue(execution.actionType);
    query.addBindValue(execution.targetReference);
    query.addBindValue(execution.requestedAtUtc);
    query.addBindValue(execution.configurationRevision);
    if (!query.exec()) return failure<bool>(queryFailure(query));
    RepositoryResult<bool> result;
    result.value = query.numRowsAffected() == 1;
    return result;
}

RepositoryResult<void> TaskAutomationRepository::updateExecution(
    const QString &executionId, QString status, QString executedAtUtc, QString errorMessage)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    static const QSet<QString> statuses{
        QStringLiteral("pending"), QStringLiteral("running"), QStringLiteral("succeeded"),
        QStringLiteral("failed"), QStringLiteral("canceled"), QStringLiteral("skipped")};
    if (executionId.isEmpty() || !statuses.contains(status))
        return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE task_automation_executions SET status=?,executed_at_utc=?,error_message=? "
        "WHERE execution_id=?"));
    query.addBindValue(status);
    query.addBindValue(executedAtUtc.isEmpty() ? QVariant() : QVariant(executedAtUtc));
    query.addBindValue(errorMessage);
    query.addBindValue(executionId);
    if (!query.exec()) return failureVoid(queryFailure(query));
    if (query.numRowsAffected() != 1) return failureVoid(domain::ErrorCategory::NotFound);
    return {};
}

RepositoryResult<int> TaskAutomationRepository::cancelUnfinished(QString executedAtUtc)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE task_automation_executions SET status='canceled',executed_at_utc=?,"
        "error_message='Application exited before the action result could be confirmed; the action was not resumed.' "
        "WHERE status IN ('pending','running')"));
    query.addBindValue(executedAtUtc);
    if (!query.exec()) return failure<int>(queryFailure(query));
    RepositoryResult<int> result;
    result.value = query.numRowsAffected();
    return result;
}

namespace {

RepositoryResult<TaskSchedule> taskScheduleFromQuery(const QSqlQuery &query,
                                                      int firstColumn,
                                                      bool scheduleRowExists)
{
    TaskSchedule schedule;
    schedule.taskId = query.value(firstColumn).toLongLong();
    if (!scheduleRowExists) return {schedule, {}};

    if (!query.value(firstColumn + 1).isNull()) {
        const QString value = query.value(firstColumn + 1).toString();
        const QDate date = QDate::fromString(value, Qt::ISODate);
        if (!date.isValid() || date.toString(Qt::ISODate) != value)
            return failure<TaskSchedule>(domain::ErrorCategory::Storage);
        schedule.plannedDate = date;
    }
    if (!query.value(firstColumn + 2).isNull()) {
        const QString value = query.value(firstColumn + 2).toString();
        const QTime time = QTime::fromString(value, QStringLiteral("HH:mm"));
        if (!time.isValid() || time.toString(QStringLiteral("HH:mm")) != value)
            return failure<TaskSchedule>(domain::ErrorCategory::Storage);
        schedule.plannedStartTime = time;
    }
    if (!query.value(firstColumn + 3).isNull()) {
        bool durationOk = false;
        const int duration = query.value(firstColumn + 3).toInt(&durationOk);
        if (!durationOk) return failure<TaskSchedule>(domain::ErrorCategory::Storage);
        schedule.estimatedDurationMinutes = duration;
    }
    bool revisionOk = false;
    schedule.revision = query.value(firstColumn + 4).toLongLong(&revisionOk);
    if (!revisionOk) return failure<TaskSchedule>(domain::ErrorCategory::Storage);
    schedule.updatedAt = query.value(firstColumn + 5).toString();
    if (domain::normalizeTaskSchedule(schedule).isError())
        return failure<TaskSchedule>(domain::ErrorCategory::Storage);
    return {std::move(schedule), {}};
}

int workingDaysMask(const QVector<int> &workingDays)
{
    int mask = 0;
    for (const int day : workingDays) mask |= 1 << (day - 1);
    return mask;
}

RepositoryResult<SchedulingPreferences> schedulingPreferencesFromQuery(const QSqlQuery &query)
{
    bool maskOk = false;
    const int mask = query.value(0).toInt(&maskOk);
    if (!maskOk || mask < 0 || mask > 127)
        return failure<SchedulingPreferences>(domain::ErrorCategory::Storage);

    SchedulingPreferences preferences;
    preferences.preferredWorkingDays.clear();
    for (int day = 1; day <= 7; ++day) {
        if ((mask & (1 << (day - 1))) != 0) preferences.preferredWorkingDays.push_back(day);
    }

    const QString start = query.value(1).toString();
    const QString end = query.value(2).toString();
    preferences.workdayStart = QTime::fromString(start, QStringLiteral("HH:mm"));
    preferences.workdayEnd = QTime::fromString(end, QStringLiteral("HH:mm"));
    if (!preferences.workdayStart.isValid() || !preferences.workdayEnd.isValid()
        || preferences.workdayStart.toString(QStringLiteral("HH:mm")) != start
        || preferences.workdayEnd.toString(QStringLiteral("HH:mm")) != end) {
        return failure<SchedulingPreferences>(domain::ErrorCategory::Storage);
    }
    bool maximumOk = false;
    bool defaultDurationOk = false;
    bool revisionOk = false;
    preferences.maximumPlannedWorkMinutesPerDay = query.value(3).toInt(&maximumOk);
    preferences.defaultEstimatedDurationMinutes = query.value(4).toInt(&defaultDurationOk);
    if (!maximumOk || !defaultDurationOk)
        return failure<SchedulingPreferences>(domain::ErrorCategory::Storage);
    if (!query.value(5).isNull()) {
        bool breakOk = false;
        const int breakMinutes = query.value(5).toInt(&breakOk);
        if (!breakOk) return failure<SchedulingPreferences>(domain::ErrorCategory::Storage);
        preferences.breakBetweenWorkBlocksMinutes = breakMinutes;
    } else {
        preferences.breakBetweenWorkBlocksMinutes.reset();
    }
    preferences.revision = query.value(6).toLongLong(&revisionOk);
    if (!revisionOk) return failure<SchedulingPreferences>(domain::ErrorCategory::Storage);
    preferences.updatedAt = query.value(7).toString();
    if (domain::normalizeSchedulingPreferences(preferences).isError())
        return failure<SchedulingPreferences>(domain::ErrorCategory::Storage);
    return {std::move(preferences), {}};
}

} // namespace

SchedulingRepository::SchedulingRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<TaskSchedule> SchedulingRepository::taskSchedule(qint64 taskId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (taskId <= 0) return failure<TaskSchedule>(domain::ErrorCategory::Validation);
    return loadTaskSchedule(taskId);
}

RepositoryResult<TaskSchedule> SchedulingRepository::loadTaskSchedule(qint64 taskId) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT t.id,s.planned_date,s.planned_start_time,s.estimated_duration_minutes,"
        "s.revision,s.updated_at FROM tasks t LEFT JOIN task_schedules s ON s.task_id=t.id "
        "WHERE t.id=?"));
    query.addBindValue(taskId);
    if (!query.exec()) return failure<TaskSchedule>(queryFailure(query));
    if (!query.next()) return failure<TaskSchedule>(domain::ErrorCategory::NotFound);
    return taskScheduleFromQuery(query, 0, !query.value(4).isNull());
}

RepositoryResult<QVector<TaskSchedule>> SchedulingRepository::taskSchedules() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT task_id,planned_date,planned_start_time,estimated_duration_minutes,"
            "revision,updated_at FROM task_schedules ORDER BY task_id"))) {
        return failure<QVector<TaskSchedule>>(queryFailure(query));
    }
    RepositoryResult<QVector<TaskSchedule>> result;
    while (query.next()) {
        auto schedule = taskScheduleFromQuery(query, 0, true);
        if (!schedule.succeeded()) return failure<QVector<TaskSchedule>>(schedule.error.category());
        result.value.push_back(std::move(schedule.value));
    }
    return result;
}

RepositoryResult<TaskSchedule> SchedulingRepository::setTaskSchedule(
    const TaskSchedule &requested, qint64 expectedRevision)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    TaskSchedule schedule = requested;
    if (expectedRevision < 0 || schedule.revision != expectedRevision
        || domain::normalizeTaskSchedule(schedule).isError())
        return failure<TaskSchedule>(domain::ErrorCategory::Validation);

    Transaction transaction(m_database);
    if (!transaction.active()) return failure<TaskSchedule>(domain::ErrorCategory::Storage);
    const auto current = loadTaskSchedule(schedule.taskId);
    if (!current.succeeded()) return failure<TaskSchedule>(current.error.category());
    if (current.value.revision != expectedRevision)
        return failure<TaskSchedule>(domain::ErrorCategory::Conflict);

    const QString plannedDate = schedule.plannedDate
        ? schedule.plannedDate->toString(Qt::ISODate) : QString{};
    const QString plannedStart = schedule.plannedStartTime
        ? schedule.plannedStartTime->toString(QStringLiteral("HH:mm")) : QString{};
    QSqlQuery query(m_database);
    if (expectedRevision == 0) {
        query.prepare(QStringLiteral(
            "INSERT INTO task_schedules(task_id,planned_date,planned_start_time,"
            "estimated_duration_minutes,revision) VALUES(?,?,?,?,1)"));
        query.addBindValue(schedule.taskId);
        query.addBindValue(nullableString(plannedDate));
        query.addBindValue(nullableString(plannedStart));
        query.addBindValue(schedule.estimatedDurationMinutes
                               ? QVariant(*schedule.estimatedDurationMinutes) : QVariant{});
    } else {
        query.prepare(QStringLiteral(
            "UPDATE task_schedules SET planned_date=?,planned_start_time=?,"
            "estimated_duration_minutes=?,revision=revision+1,updated_at=CURRENT_TIMESTAMP "
            "WHERE task_id=? AND revision=?"));
        query.addBindValue(nullableString(plannedDate));
        query.addBindValue(nullableString(plannedStart));
        query.addBindValue(schedule.estimatedDurationMinutes
                               ? QVariant(*schedule.estimatedDurationMinutes) : QVariant{});
        query.addBindValue(schedule.taskId);
        query.addBindValue(expectedRevision);
    }
    if (!query.exec()) return failure<TaskSchedule>(queryFailure(query));
    if (query.numRowsAffected() != 1) return failure<TaskSchedule>(domain::ErrorCategory::Conflict);

    auto saved = loadTaskSchedule(schedule.taskId);
    if (!saved.succeeded()) return saved;
    if (!transaction.commit()) return failure<TaskSchedule>(domain::ErrorCategory::Storage);
    return saved;
}

RepositoryResult<TaskScheduleBatchResult> SchedulingRepository::applyTaskSchedules(
    const QVector<TaskScheduleProposal> &proposals,
    const QVector<TaskScheduleSnapshot> &taskSnapshot,
    qint64 expectedPreferencesRevision,
    const std::function<bool()> &cancelled)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (proposals.isEmpty() || taskSnapshot.isEmpty() || expectedPreferencesRevision <= 0)
        return failure<TaskScheduleBatchResult>(domain::ErrorCategory::Validation);

    QHash<qint64, TaskScheduleSnapshot> expectedTasks;
    expectedTasks.reserve(taskSnapshot.size());
    for (const TaskScheduleSnapshot &snapshot : taskSnapshot) {
        if (snapshot.taskId <= 0 || snapshot.taskRevision <= 0 || snapshot.scheduleRevision < 0
            || expectedTasks.contains(snapshot.taskId)) {
            return failure<TaskScheduleBatchResult>(domain::ErrorCategory::Validation);
        }
        expectedTasks.insert(snapshot.taskId, snapshot);
    }

    QSet<qint64> seenTaskIds;
    for (const TaskScheduleProposal &proposal : proposals) {
        TaskSchedule schedule = proposal.schedule;
        const auto expectedTask = expectedTasks.constFind(schedule.taskId);
        if (proposal.expectedTaskRevision <= 0 || !schedule.plannedDate
            || schedule.revision < 0 || seenTaskIds.contains(schedule.taskId)
            || expectedTask == expectedTasks.cend()
            || expectedTask->taskRevision != proposal.expectedTaskRevision
            || expectedTask->scheduleRevision != schedule.revision
            || domain::normalizeTaskSchedule(schedule).isError()) {
            return failure<TaskScheduleBatchResult>(domain::ErrorCategory::Validation);
        }
        seenTaskIds.insert(schedule.taskId);
    }
    const auto isCancelled = [&cancelled] { return cancelled && cancelled(); };
    if (isCancelled()) {
        RepositoryResult<TaskScheduleBatchResult> result;
        result.error = domain::AppError::cancelled();
        return result;
    }

    Transaction transaction(m_database);
    if (!transaction.active())
        return failure<TaskScheduleBatchResult>(domain::ErrorCategory::Storage);

    const auto preferences = schedulingPreferences();
    if (!preferences.succeeded())
        return failure<TaskScheduleBatchResult>(preferences.error.category());
    if (preferences.value.revision != expectedPreferencesRevision)
        return failure<TaskScheduleBatchResult>(domain::ErrorCategory::Conflict);

    struct LiveTask final {
        qint64 revision{0};
        QString title;
    };
    QHash<qint64, LiveTask> liveTasks;
    liveTasks.reserve(taskSnapshot.size());
    QSqlQuery tasksQuery(m_database);
    if (!tasksQuery.exec(QStringLiteral(
            "SELECT id,revision,title FROM tasks WHERE is_completed=0 ORDER BY id"))) {
        return failure<TaskScheduleBatchResult>(queryFailure(tasksQuery));
    }
    while (tasksQuery.next()) {
        if (isCancelled()) {
            RepositoryResult<TaskScheduleBatchResult> result;
            result.error = domain::AppError::cancelled();
            return result;
        }
        bool idOk = false;
        bool revisionOk = false;
        const qint64 taskId = tasksQuery.value(0).toLongLong(&idOk);
        const qint64 revision = tasksQuery.value(1).toLongLong(&revisionOk);
        if (!idOk || !revisionOk || taskId <= 0 || revision <= 0)
            return failure<TaskScheduleBatchResult>(domain::ErrorCategory::Storage);
        liveTasks.insert(taskId, LiveTask{revision, tasksQuery.value(2).toString()});
    }
    tasksQuery.finish();
    if (liveTasks.size() != expectedTasks.size())
        return failure<TaskScheduleBatchResult>(domain::ErrorCategory::Conflict);
    for (const TaskScheduleSnapshot &snapshot : taskSnapshot) {
        const auto live = liveTasks.constFind(snapshot.taskId);
        if (live == liveTasks.cend() || live->revision != snapshot.taskRevision)
            return failure<TaskScheduleBatchResult>(domain::ErrorCategory::Conflict);
    }

    QHash<qint64, TaskSchedule> currentSchedules;
    QSqlQuery schedulesQuery(m_database);
    if (!schedulesQuery.exec(QStringLiteral(
            "SELECT task_id,planned_date,planned_start_time,estimated_duration_minutes,"
            "revision,updated_at FROM task_schedules ORDER BY task_id"))) {
        return failure<TaskScheduleBatchResult>(queryFailure(schedulesQuery));
    }
    while (schedulesQuery.next()) {
        if (isCancelled()) {
            RepositoryResult<TaskScheduleBatchResult> result;
            result.error = domain::AppError::cancelled();
            return result;
        }
        const auto schedule = taskScheduleFromQuery(schedulesQuery, 0, true);
        if (!schedule.succeeded())
            return failure<TaskScheduleBatchResult>(schedule.error.category());
        currentSchedules.insert(schedule.value.taskId, schedule.value);
    }
    schedulesQuery.finish();
    for (const TaskScheduleSnapshot &snapshot : taskSnapshot) {
        if (!currentSchedules.contains(snapshot.taskId)) {
            TaskSchedule schedule;
            schedule.taskId = snapshot.taskId;
            currentSchedules.insert(snapshot.taskId, schedule);
        }
        if (currentSchedules.value(snapshot.taskId).revision != snapshot.scheduleRevision)
            return failure<TaskScheduleBatchResult>(domain::ErrorCategory::Conflict);
    }

    struct CheckedProposal final {
        TaskScheduleProposal proposal;
        TaskSchedule currentSchedule;
        QString taskTitle;
    };
    QVector<CheckedProposal> checked;
    checked.reserve(proposals.size());
    for (const TaskScheduleProposal &proposal : proposals) {
        const LiveTask task = liveTasks.value(proposal.schedule.taskId);
        checked.push_back({proposal, currentSchedules.value(proposal.schedule.taskId), task.title});
    }

    bool historyAvailable = false;
    domain::AppError historyAvailabilityError;
    if (!taskHistoryTableExists(m_database, &historyAvailable, &historyAvailabilityError))
        return failure<TaskScheduleBatchResult>(historyAvailabilityError.category());

    TaskScheduleBatchResult batch;
    batch.schedules.reserve(checked.size());
    for (const CheckedProposal &entry : checked) {
        if (isCancelled()) {
            RepositoryResult<TaskScheduleBatchResult> result;
            result.error = domain::AppError::cancelled();
            return result;
        }

        const TaskSchedule &schedule = entry.proposal.schedule;
        if (sameScheduleValues(entry.currentSchedule, schedule)) {
            batch.schedules.push_back(entry.currentSchedule);
            continue;
        }

        const QString plannedDate = schedule.plannedDate->toString(Qt::ISODate);
        const QString plannedStart = schedule.plannedStartTime
            ? schedule.plannedStartTime->toString(QStringLiteral("HH:mm")) : QString{};
        QSqlQuery writeQuery(m_database);
        if (schedule.revision == 0) {
            writeQuery.prepare(QStringLiteral(
                "INSERT INTO task_schedules(task_id,planned_date,planned_start_time,"
                "estimated_duration_minutes,revision) "
                "SELECT ?,?,?,?,1 WHERE EXISTS(SELECT 1 FROM tasks "
                "WHERE id=? AND revision=? AND is_completed=0) "
                "AND EXISTS(SELECT 1 FROM scheduling_preferences WHERE id=1 AND revision=?)"));
            writeQuery.addBindValue(schedule.taskId);
            writeQuery.addBindValue(plannedDate);
            writeQuery.addBindValue(nullableString(plannedStart));
            writeQuery.addBindValue(schedule.estimatedDurationMinutes
                                        ? QVariant(*schedule.estimatedDurationMinutes)
                                        : QVariant{});
            writeQuery.addBindValue(schedule.taskId);
            writeQuery.addBindValue(entry.proposal.expectedTaskRevision);
            writeQuery.addBindValue(expectedPreferencesRevision);
        } else {
            writeQuery.prepare(QStringLiteral(
                "UPDATE task_schedules SET planned_date=?,planned_start_time=?,"
                "estimated_duration_minutes=?,revision=revision+1,updated_at=CURRENT_TIMESTAMP "
                "WHERE task_id=? AND revision=? AND EXISTS(SELECT 1 FROM tasks "
                "WHERE id=? AND revision=? AND is_completed=0) "
                "AND EXISTS(SELECT 1 FROM scheduling_preferences WHERE id=1 AND revision=?)"));
            writeQuery.addBindValue(plannedDate);
            writeQuery.addBindValue(nullableString(plannedStart));
            writeQuery.addBindValue(schedule.estimatedDurationMinutes
                                        ? QVariant(*schedule.estimatedDurationMinutes)
                                        : QVariant{});
            writeQuery.addBindValue(schedule.taskId);
            writeQuery.addBindValue(schedule.revision);
            writeQuery.addBindValue(schedule.taskId);
            writeQuery.addBindValue(entry.proposal.expectedTaskRevision);
            writeQuery.addBindValue(expectedPreferencesRevision);
        }
        if (!writeQuery.exec())
            return failure<TaskScheduleBatchResult>(queryFailure(writeQuery));
        if (writeQuery.numRowsAffected() != 1)
            return failure<TaskScheduleBatchResult>(domain::ErrorCategory::Conflict);

        if (isCancelled()) {
            RepositoryResult<TaskScheduleBatchResult> result;
            result.error = domain::AppError::cancelled();
            return result;
        }
        if (historyAvailable) {
            const QVector<domain::TaskHistoryChange> changes = taskScheduleHistoryChanges(
                entry.currentSchedule, schedule);
            domain::AppError historyError;
            if (!insertTaskHistory(m_database, schedule.taskId,
                                   domain::TaskHistoryEventType::TaskUpdated, std::nullopt,
                                   entry.taskTitle, changes, &historyError)) {
                return failure<TaskScheduleBatchResult>(historyError.category());
            }
        }

        const auto saved = loadTaskSchedule(schedule.taskId);
        if (!saved.succeeded())
            return failure<TaskScheduleBatchResult>(saved.error.category());
        batch.schedules.push_back(saved.value);
        ++batch.changedCount;
    }

    if (isCancelled()) {
        RepositoryResult<TaskScheduleBatchResult> result;
        result.error = domain::AppError::cancelled();
        return result;
    }
    if (!transaction.commit())
        return failure<TaskScheduleBatchResult>(domain::ErrorCategory::Storage);
    RepositoryResult<TaskScheduleBatchResult> result;
    result.value = std::move(batch);
    return result;
}

RepositoryResult<SchedulingPreferences> SchedulingRepository::schedulingPreferences() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT preferred_working_days_mask,workday_start_time,workday_end_time,"
            "maximum_planned_work_minutes_per_day,default_estimated_duration_minutes,"
            "break_between_work_blocks_minutes,revision,updated_at "
            "FROM scheduling_preferences WHERE id=1"))) {
        return failure<SchedulingPreferences>(queryFailure(query));
    }
    if (!query.next()) return failure<SchedulingPreferences>(domain::ErrorCategory::NotFound);
    return schedulingPreferencesFromQuery(query);
}

RepositoryResult<SchedulingPreferences> SchedulingRepository::setSchedulingPreferences(
    const SchedulingPreferences &requested, qint64 expectedRevision)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    SchedulingPreferences preferences = requested;
    if (expectedRevision <= 0 || preferences.revision != expectedRevision
        || domain::normalizeSchedulingPreferences(preferences).isError())
        return failure<SchedulingPreferences>(domain::ErrorCategory::Validation);

    Transaction transaction(m_database);
    if (!transaction.active()) return failure<SchedulingPreferences>(domain::ErrorCategory::Storage);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE scheduling_preferences SET preferred_working_days_mask=?,"
        "workday_start_time=?,workday_end_time=?,maximum_planned_work_minutes_per_day=?,"
        "default_estimated_duration_minutes=?,break_between_work_blocks_minutes=?,"
        "revision=revision+1,updated_at=CURRENT_TIMESTAMP WHERE id=1 AND revision=?"));
    query.addBindValue(workingDaysMask(preferences.preferredWorkingDays));
    query.addBindValue(preferences.workdayStart.toString(QStringLiteral("HH:mm")));
    query.addBindValue(preferences.workdayEnd.toString(QStringLiteral("HH:mm")));
    query.addBindValue(preferences.maximumPlannedWorkMinutesPerDay);
    query.addBindValue(preferences.defaultEstimatedDurationMinutes);
    query.addBindValue(preferences.breakBetweenWorkBlocksMinutes
                           ? QVariant(*preferences.breakBetweenWorkBlocksMinutes) : QVariant{});
    query.addBindValue(expectedRevision);
    if (!query.exec()) return failure<SchedulingPreferences>(queryFailure(query));
    if (query.numRowsAffected() != 1)
        return failure<SchedulingPreferences>(domain::ErrorCategory::Conflict);

    auto saved = schedulingPreferences();
    if (!saved.succeeded()) return saved;
    if (!transaction.commit()) return failure<SchedulingPreferences>(domain::ErrorCategory::Storage);
    return saved;
}

TaskHistoryRepository::TaskHistoryRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<TaskHistoryPage> TaskHistoryRepository::forTask(
    qint64 taskId, std::optional<domain::TaskHistoryEventType> eventType,
    int pageSize, const TaskHistoryCursor &before) const
{
    if (taskId <= 0) return failure<TaskHistoryPage>(domain::ErrorCategory::Validation);
    return list(taskId, {}, eventType, pageSize, before);
}

RepositoryResult<TaskHistoryPage> TaskHistoryRepository::recent(
    QString taskTitleQuery, std::optional<domain::TaskHistoryEventType> eventType,
    int pageSize, const TaskHistoryCursor &before) const
{
    return list(std::nullopt, std::move(taskTitleQuery), eventType, pageSize, before);
}

RepositoryResult<TaskHistoryPage> TaskHistoryRepository::list(
    std::optional<qint64> taskId, QString taskTitleQuery,
    std::optional<domain::TaskHistoryEventType> eventType,
    int pageSize, const TaskHistoryCursor &before) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (pageSize < 1 || pageSize > 100 || !before.isValid()
        || taskTitleQuery.toUcs4().size() > 256
        || (before.hasValue()
            && (!QDateTime::fromString(before.occurredAtUtc, Qt::ISODateWithMs).isValid()
                || before.occurredAtUtc.size() > 64
                || !before.occurredAtUtc.endsWith(QLatin1Char('Z'))))) {
        return failure<TaskHistoryPage>(domain::ErrorCategory::Validation);
    }

    QString sql = QStringLiteral(
        "SELECT event_id,task_id,event_type,occurred_at_utc,task_revision,task_title,"
        "serialization_version,changed_fields_json,previous_values_json,new_values_json "
        "FROM task_history WHERE 1=1");
    QVariantList binds;
    if (taskId) {
        sql += QStringLiteral(" AND task_id=?");
        binds.push_back(*taskId);
    }
    if (!taskTitleQuery.trimmed().isEmpty()) {
        sql += QStringLiteral(" AND task_title COLLATE NOCASE LIKE ? ESCAPE '\\'");
        binds.push_back(taskHistoryTitleLikePattern(taskTitleQuery.trimmed()));
    }
    if (eventType) {
        const QString typeName = domain::taskHistoryEventTypeName(*eventType);
        if (typeName.isEmpty()) return failure<TaskHistoryPage>(domain::ErrorCategory::Validation);
        sql += QStringLiteral(" AND event_type=?");
        binds.push_back(typeName);
    }
    if (before.hasValue()) {
        sql += QStringLiteral(
            " AND (occurred_at_utc<? OR (occurred_at_utc=? AND event_id<?))");
        binds << before.occurredAtUtc << before.occurredAtUtc << before.eventId;
    }
    sql += QStringLiteral(" ORDER BY occurred_at_utc DESC,event_id DESC LIMIT ?");
    binds.push_back(pageSize + 1);

    QSqlQuery query(m_database);
    query.prepare(sql);
    for (const QVariant &bind : binds) query.addBindValue(bind);
    if (!query.exec()) return failure<TaskHistoryPage>(queryFailure(query));

    RepositoryResult<TaskHistoryPage> result;
    int scanned = 0;
    while (query.next()) {
        if (scanned == pageSize) {
            result.value.hasMore = true;
            break;
        }
        ++scanned;
        const qint64 eventId = query.value(0).toLongLong();
        const qint64 taskIdValue = query.value(1).toLongLong();
        const QString occurredAtUtc = query.value(3).toString();
        if (eventId <= 0 || taskIdValue <= 0
            || !occurredAtUtc.endsWith(QLatin1Char('Z'))
            || !QDateTime::fromString(occurredAtUtc, Qt::ISODateWithMs).isValid()) {
            return failure<TaskHistoryPage>(domain::ErrorCategory::InvalidResponse);
        }
        result.value.nextCursor = {occurredAtUtc, eventId};

        const auto type = domain::taskHistoryEventTypeFromName(query.value(2).toString());
        const int version = query.value(6).toInt();
        const auto changes = domain::deserializeTaskHistoryDiff(
            version, query.value(7).toString(), query.value(8).toString(),
            query.value(9).toString());
        if (!type || !changes || changes->isEmpty()) {
            ++result.value.invalidEventsSkipped;
            continue;
        }

        domain::TaskHistoryEvent event;
        event.eventId = eventId;
        event.taskId = taskIdValue;
        event.type = *type;
        event.occurredAtUtc = occurredAtUtc;
        if (!query.isNull(4)) event.taskRevision = query.value(4).toLongLong();
        event.taskTitle = query.value(5).toString();
        event.serializationVersion = version;
        event.changes = *changes;
        result.value.events.push_back(std::move(event));
    }
    return result;
}

CurrencyFavoritesRepository::CurrencyFavoritesRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<QVector<CurrencyPair>> CurrencyFavoritesRepository::list() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT base_code,quote_code FROM currency_favorites ORDER BY base_code,quote_code")))
        return failure<QVector<CurrencyPair>>(queryFailure(query));
    RepositoryResult<QVector<CurrencyPair>> result;
    while (query.next()) result.value.push_back({query.value(0).toString(), query.value(1).toString()});
    return result;
}

RepositoryResult<void> CurrencyFavoritesRepository::add(QString baseCode, QString quoteCode)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    baseCode = baseCode.trimmed().toUpper();
    quoteCode = quoteCode.trimmed().toUpper();
    static const QRegularExpression code(QStringLiteral("^[A-Z]{3}$"));
    if (!code.match(baseCode).hasMatch() || !code.match(quoteCode).hasMatch()
        || baseCode == quoteCode) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO currency_favorites(base_code,quote_code) VALUES(?,?) ON CONFLICT(base_code,quote_code) DO NOTHING"));
    query.addBindValue(baseCode);
    query.addBindValue(quoteCode);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<void> CurrencyFavoritesRepository::remove(QString baseCode, QString quoteCode)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    baseCode = baseCode.trimmed().toUpper();
    quoteCode = quoteCode.trimmed().toUpper();
    static const QRegularExpression code(QStringLiteral("^[A-Z]{3}$"));
    if (!code.match(baseCode).hasMatch() || !code.match(quoteCode).hasMatch())
        return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM currency_favorites WHERE base_code=? AND quote_code=?"));
    query.addBindValue(baseCode);
    query.addBindValue(quoteCode);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

FavoritesRepository::FavoritesRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<QVector<FavoriteCategory>> FavoritesRepository::categories(QString source) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!validSource(source)) return failure<QVector<FavoriteCategory>>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT id,name,source,COALESCE(color,''),created_at,display_order,COALESCE(updated_at,created_at) FROM favorite_categories WHERE source=? ORDER BY display_order,name_normalized,id"));
    query.addBindValue(source);
    if (!query.exec()) return failure<QVector<FavoriteCategory>>(queryFailure(query));
    RepositoryResult<QVector<FavoriteCategory>> result;
    while (query.next()) {
        result.value.push_back({query.value(0).toLongLong(), query.value(1).toString(), query.value(2).toString(),
                                query.value(3).toString(), query.value(4).toString(), query.value(5).toInt(), query.value(6).toString()});
    }
    return result;
}

RepositoryResult<FavoriteCategory> FavoritesRepository::createCategory(QString source, QString name, QString color)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    name = name.trimmed();
    QString normalizedColor;
    if (!validSource(source) || name.isEmpty() || name.toUcs4().size() > 80
        || !normalizeCategoryColor(std::move(color), &normalizedColor))
        return failure<FavoriteCategory>(domain::ErrorCategory::Validation);
    QSqlQuery order(m_database);
    order.prepare(QStringLiteral("SELECT COALESCE(MAX(display_order),-1)+1 FROM favorite_categories WHERE source=?"));
    order.addBindValue(source);
    if (!order.exec() || !order.next()) return failure<FavoriteCategory>(queryFailure(order));
    const int nextOrder = order.value(0).toInt();
    order.finish();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO favorite_categories(name,name_normalized,source,color,display_order) VALUES(?,?,?,?,?)"));
    query.addBindValue(name);
    query.addBindValue(normalizedName(name));
    query.addBindValue(source);
    query.addBindValue(nullableString(normalizedColor));
    query.addBindValue(nextOrder);
    if (!query.exec()) {
        if (queryFailure(query) != domain::ErrorCategory::Conflict) return failure<FavoriteCategory>(queryFailure(query));
        QSqlQuery existing(m_database);
        existing.prepare(QStringLiteral("SELECT id,name,source,COALESCE(color,''),created_at,display_order,COALESCE(updated_at,created_at) FROM favorite_categories WHERE source=? AND name_normalized=?"));
        existing.addBindValue(source);
        existing.addBindValue(normalizedName(name));
        if (!existing.exec()) return failure<FavoriteCategory>(queryFailure(existing));
        if (!existing.next()) return failure<FavoriteCategory>(domain::ErrorCategory::Conflict);
        return {FavoriteCategory{existing.value(0).toLongLong(), existing.value(1).toString(),
            existing.value(2).toString(), existing.value(3).toString(), existing.value(4).toString(), existing.value(5).toInt(), existing.value(6).toString()}, {}};
    }
    const qint64 id = query.lastInsertId().toLongLong();
    QSqlQuery timestamps(m_database);
    timestamps.prepare(QStringLiteral("SELECT created_at,COALESCE(updated_at,created_at) FROM favorite_categories WHERE id=?"));
    timestamps.addBindValue(id);
    if (!timestamps.exec() || !timestamps.next()) return failure<FavoriteCategory>(queryFailure(timestamps));
    return {FavoriteCategory{id, name, source, normalizedColor, timestamps.value(0).toString(), nextOrder, timestamps.value(1).toString()}, {}};
}

RepositoryResult<FavoriteCategory> FavoritesRepository::updateCategory(qint64 id, QString source,
                                                                        QString name, QString color)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    name = name.trimmed();
    QString normalizedColor;
    if (id <= 0 || !validSource(source) || name.isEmpty() || name.toUcs4().size() > 80
        || !normalizeCategoryColor(std::move(color), &normalizedColor))
        return failure<FavoriteCategory>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE favorite_categories SET name=?,name_normalized=?,color=?,updated_at=CURRENT_TIMESTAMP WHERE id=? AND source=?"));
    query.addBindValue(name);
    query.addBindValue(normalizedName(name));
    query.addBindValue(nullableString(normalizedColor));
    query.addBindValue(id);
    query.addBindValue(source);
    if (!query.exec()) return failure<FavoriteCategory>(queryFailure(query));
    if (query.numRowsAffected() != 1) return failure<FavoriteCategory>(domain::ErrorCategory::NotFound);
    QSqlQuery resultQuery(m_database);
    resultQuery.prepare(QStringLiteral("SELECT created_at,display_order,COALESCE(updated_at,created_at) FROM favorite_categories WHERE id=?"));
    resultQuery.addBindValue(id);
    if (!resultQuery.exec() || !resultQuery.next()) return failure<FavoriteCategory>(queryFailure(resultQuery));
    return {FavoriteCategory{id, name, source, normalizedColor, resultQuery.value(0).toString(), resultQuery.value(1).toInt(), resultQuery.value(2).toString()}, {}};
}

RepositoryResult<void> FavoritesRepository::reorderCategories(QString source, const QVector<qint64> &orderedIds)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!validSource(source)) return failureVoid(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    QSqlQuery existing(m_database);
    existing.prepare(QStringLiteral("SELECT id FROM favorite_categories WHERE source=? ORDER BY display_order,id"));
    existing.addBindValue(source);
    if (!existing.exec()) return failureVoid(queryFailure(existing));
    QSet<qint64> expected;
    while (existing.next()) expected.insert(existing.value(0).toLongLong());
    existing.finish();
    QSet<qint64> supplied;
    for (qint64 id : orderedIds) {
        if (id <= 0 || supplied.contains(id) || !expected.contains(id)) return failureVoid(domain::ErrorCategory::Validation);
        supplied.insert(id);
    }
    if (supplied != expected) return failureVoid(domain::ErrorCategory::Validation);
    for (int position = 0; position < orderedIds.size(); ++position) {
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral("UPDATE favorite_categories SET display_order=?,updated_at=CURRENT_TIMESTAMP WHERE id=? AND source=?"));
        update.addBindValue(position);
        update.addBindValue(orderedIds[position]);
        update.addBindValue(source);
        if (!update.exec()) return failureVoid(queryFailure(update));
        if (update.numRowsAffected() != 1) return failureVoid(domain::ErrorCategory::Conflict);
    }
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<FavoriteCategoryDeleteResult> FavoritesRepository::removeCategory(
    qint64 id, QString mode, std::optional<qint64> targetCategoryId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    mode = mode.trimmed().toLower();
    if (id <= 0 || (mode != QStringLiteral("unassign") && mode != QStringLiteral("move")))
        return failure<FavoriteCategoryDeleteResult>(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<FavoriteCategoryDeleteResult>(domain::ErrorCategory::Storage);
    QSqlQuery current(m_database);
    current.prepare(QStringLiteral("SELECT source FROM favorite_categories WHERE id=?"));
    current.addBindValue(id);
    if (!current.exec()) return failure<FavoriteCategoryDeleteResult>(queryFailure(current));
    if (!current.next()) return failure<FavoriteCategoryDeleteResult>(domain::ErrorCategory::NotFound);
    const QString source = current.value(0).toString();
    current.finish();
    if (!validSource(source)) return failure<FavoriteCategoryDeleteResult>(domain::ErrorCategory::Storage);

    if (mode == QStringLiteral("move")) {
        if (!targetCategoryId || *targetCategoryId <= 0 || *targetCategoryId == id)
            return failure<FavoriteCategoryDeleteResult>(domain::ErrorCategory::Validation);
        QSqlQuery target(m_database);
        target.prepare(QStringLiteral("SELECT source FROM favorite_categories WHERE id=?"));
        target.addBindValue(*targetCategoryId);
        if (!target.exec()) return failure<FavoriteCategoryDeleteResult>(queryFailure(target));
        if (!target.next()) return failure<FavoriteCategoryDeleteResult>(domain::ErrorCategory::NotFound);
        const QString targetSource = target.value(0).toString();
        target.finish();
        if (targetSource != source)
            return failure<FavoriteCategoryDeleteResult>(domain::ErrorCategory::Validation);
    }

    const QString table = source == QStringLiteral("telegram")
        ? QStringLiteral("telegram_favorites") : QStringLiteral("youtube_favorites");
    QSqlQuery count(m_database);
    count.prepare(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE category_id=?").arg(table));
    count.addBindValue(id);
    if (!count.exec() || !count.next()) return failure<FavoriteCategoryDeleteResult>(queryFailure(count));
    const int affected = count.value(0).toInt();
    count.finish();
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE %1 SET category_id=? WHERE category_id=?").arg(table));
    update.addBindValue(mode == QStringLiteral("move") ? QVariant(*targetCategoryId) : QVariant());
    update.addBindValue(id);
    if (!update.exec()) return failure<FavoriteCategoryDeleteResult>(queryFailure(update));
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM favorite_categories WHERE id=?"));
    remove.addBindValue(id);
    if (!remove.exec()) return failure<FavoriteCategoryDeleteResult>(queryFailure(remove));
    if (remove.numRowsAffected() != 1) return failure<FavoriteCategoryDeleteResult>(domain::ErrorCategory::Conflict);
    if (!transaction.commit()) return failure<FavoriteCategoryDeleteResult>(domain::ErrorCategory::Storage);
    RepositoryResult<FavoriteCategoryDeleteResult> result;
    result.value = {id, source, affected};
    return result;
}

RepositoryResult<QVector<Favorite>> FavoritesRepository::favorites(QString source) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!validSource(source)) return failure<QVector<Favorite>>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    const QString sql = source == QStringLiteral("telegram")
        ? QStringLiteral("SELECT username,username,added_at,category_id FROM telegram_favorites ORDER BY added_at ASC,username COLLATE NOCASE")
        : QStringLiteral("SELECT channel_id,COALESCE(username,''),added_at,category_id FROM youtube_favorites ORDER BY added_at ASC,channel_id");
    if (!query.exec(sql)) return failure<QVector<Favorite>>(queryFailure(query));
    RepositoryResult<QVector<Favorite>> result;
    while (query.next()) {
        Favorite item;
        item.source = source;
        item.sourceId = query.value(0).toString();
        item.username = query.value(1).toString();
        item.addedAt = query.value(2).toString();
        if (!query.value(3).isNull()) item.categoryId = query.value(3).toLongLong();
        result.value.push_back(std::move(item));
    }
    return result;
}

RepositoryResult<void> FavoritesRepository::assignFavoriteCategory(
    QString source, QString sourceId, std::optional<qint64> categoryId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    sourceId = sourceId.trimmed();
    if (!validSource(source) || sourceId.isEmpty()) return failureVoid(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    if (categoryId) {
        QSqlQuery category(m_database);
        category.prepare(QStringLiteral("SELECT 1 FROM favorite_categories WHERE id=? AND source=?"));
        category.addBindValue(*categoryId);
        category.addBindValue(source);
        if (!category.exec()) return failureVoid(queryFailure(category));
        if (!category.next()) return failureVoid(domain::ErrorCategory::Validation);
        category.finish();
    }
    QSqlQuery query(m_database);
    if (source == QStringLiteral("telegram")) {
        query.prepare(QStringLiteral("UPDATE telegram_favorites SET category_id=? WHERE username=?"));
        query.addBindValue(categoryId ? QVariant(*categoryId) : QVariant());
        query.addBindValue(sourceId);
    } else {
        query.prepare(QStringLiteral("UPDATE youtube_favorites SET category_id=? WHERE channel_id=?"));
        query.addBindValue(categoryId ? QVariant(*categoryId) : QVariant());
        query.addBindValue(sourceId);
    }
    if (!query.exec()) return failureVoid(queryFailure(query));
    if (query.numRowsAffected() != 1) return failureVoid(domain::ErrorCategory::NotFound);
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<void> FavoritesRepository::addFavorite(QString source, QString sourceId, QString username)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    sourceId = sourceId.trimmed();
    username = username.trimmed();
    if (!validSource(source) || sourceId.isEmpty())
        return failureVoid(domain::ErrorCategory::Validation);

    QSqlQuery query(m_database);
    if (source == QStringLiteral("telegram")) {
        query.prepare(QStringLiteral("INSERT OR IGNORE INTO telegram_favorites(username) VALUES(?)"));
        query.addBindValue(sourceId);
    } else {
        query.prepare(QStringLiteral("INSERT OR IGNORE INTO youtube_favorites(channel_id,username) VALUES(?,?)"));
        query.addBindValue(sourceId);
        query.addBindValue(nullableString(username));
    }
    if (!query.exec()) return failureVoid(queryFailure(query));
    if (query.numRowsAffected() != 1) return failureVoid(domain::ErrorCategory::Conflict);
    return {};
}

RepositoryResult<void> FavoritesRepository::removeFavorite(QString source, QString sourceId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!validSource(source) || sourceId.trimmed().isEmpty()) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(source == QStringLiteral("telegram")
                      ? QStringLiteral("DELETE FROM telegram_favorites WHERE username=?")
                      : QStringLiteral("DELETE FROM youtube_favorites WHERE channel_id=?"));
    query.addBindValue(sourceId.trimmed());
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

FavoriteNewsRepository::FavoriteNewsRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<void> FavoriteNewsRepository::recordAppOpen(QString now, QString retentionCutoff)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (now.trimmed().isEmpty() || retentionCutoff.trimmed().isEmpty())
        return failureVoid(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);

    QString previousCurrent = now;
    QSqlQuery current(m_database);
    current.prepare(QStringLiteral("SELECT value FROM preferences WHERE key='favorite_updates.current_opened_at'"));
    if (!current.exec()) return failureVoid(queryFailure(current));
    if (current.next()) previousCurrent = current.value(0).toString();
    current.finish();

    QSqlQuery previous(m_database);
    previous.prepare(QStringLiteral("INSERT INTO preferences(key,value) VALUES('favorite_updates.previous_opened_at',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    previous.addBindValue(previousCurrent);
    if (!previous.exec()) return failureVoid(queryFailure(previous));
    QSqlQuery opened(m_database);
    opened.prepare(QStringLiteral("INSERT INTO preferences(key,value) VALUES('favorite_updates.current_opened_at',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    opened.addBindValue(now);
    if (!opened.exec()) return failureVoid(queryFailure(opened));
    for (const QString &key : {QStringLiteral("favorite_updates.previous_refresh_at"),
                               QStringLiteral("favorite_updates.last_refresh_at")}) {
        QSqlQuery initial(m_database);
        initial.prepare(QStringLiteral("INSERT INTO preferences(key,value) VALUES(?,?) ON CONFLICT(key) DO NOTHING"));
        initial.addBindValue(key);
        initial.addBindValue(now);
        if (!initial.exec()) return failureVoid(queryFailure(initial));
    }
    QSqlQuery prune(m_database);
    prune.prepare(QStringLiteral("DELETE FROM favorite_news_items WHERE discovered_at<?"));
    prune.addBindValue(retentionCutoff);
    if (!prune.exec()) return failureVoid(queryFailure(prune));
    QSqlQuery resetScan(m_database);
    if (!resetScan.exec(QStringLiteral("DELETE FROM favorite_news_state WHERE id=1")))
        return failureVoid(queryFailure(resetScan));

    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<std::optional<FavoriteCheckpoint>> FavoriteNewsRepository::checkpoint(QString source,
                                                                                        QString sourceId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!validSource(source) || sourceId.trimmed().isEmpty())
        return failure<std::optional<FavoriteCheckpoint>>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT source,source_id,checked_through,COALESCE(last_success_at,''),COALESCE(last_attempted_at,''),COALESCE(last_error,'') FROM favorite_update_checkpoints WHERE source=? AND source_id=?"));
    query.addBindValue(source);
    query.addBindValue(sourceId.trimmed());
    if (!query.exec()) return failure<std::optional<FavoriteCheckpoint>>(queryFailure(query));
    RepositoryResult<std::optional<FavoriteCheckpoint>> result;
    if (query.next()) result.value = FavoriteCheckpoint{query.value(0).toString(), query.value(1).toString(),
        query.value(2).toString(), query.value(3).toString(), query.value(4).toString(), query.value(5).toString()};
    return result;
}

RepositoryResult<void> FavoriteNewsRepository::saveCheckpoint(const FavoriteCheckpoint &checkpoint)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!validSource(checkpoint.source) || checkpoint.sourceId.trimmed().isEmpty()
        || checkpoint.checkedThrough.isEmpty()) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO favorite_update_checkpoints(source,source_id,checked_through,last_success_at,last_attempted_at,last_error,updated_at) VALUES(?,?,?,?,?,?,CURRENT_TIMESTAMP) ON CONFLICT(source,source_id) DO UPDATE SET checked_through=excluded.checked_through,last_success_at=excluded.last_success_at,last_attempted_at=excluded.last_attempted_at,last_error=excluded.last_error,updated_at=CURRENT_TIMESTAMP"));
    query.addBindValue(checkpoint.source);
    query.addBindValue(checkpoint.sourceId.trimmed());
    query.addBindValue(checkpoint.checkedThrough);
    query.addBindValue(nullableString(checkpoint.lastSuccessAt));
    query.addBindValue(nullableString(checkpoint.lastAttemptedAt));
    query.addBindValue(nullableString(checkpoint.lastError));
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<bool> FavoriteNewsRepository::hasSeen(QString source, QString sourceId, QString itemId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!validSource(source) || sourceId.trimmed().isEmpty() || itemId.trimmed().isEmpty())
        return failure<bool>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT 1 FROM favorite_update_seen_items WHERE source=? AND source_id=? AND item_id=?"));
    query.addBindValue(source);
    query.addBindValue(sourceId.trimmed());
    query.addBindValue(itemId.trimmed());
    if (!query.exec()) return failure<bool>(queryFailure(query));
    RepositoryResult<bool> result;
    result.value = query.next();
    return result;
}

RepositoryResult<bool> FavoriteNewsRepository::sourceHasSeenItems(QString source, QString sourceId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!validSource(source) || sourceId.trimmed().isEmpty())
        return failure<bool>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT 1 FROM favorite_update_seen_items WHERE source=? AND source_id=? LIMIT 1"));
    query.addBindValue(source);
    query.addBindValue(sourceId.trimmed());
    if (!query.exec()) return failure<bool>(queryFailure(query));
    RepositoryResult<bool> result;
    result.value = query.next();
    return result;
}

RepositoryResult<void> FavoriteNewsRepository::saveSeen(const QVector<FavoriteNewsItem> &items)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    for (const auto &item : items) {
        if (!validSource(item.source) || item.sourceId.isEmpty() || item.itemId.isEmpty() || item.publishedAt.isEmpty())
            return failureVoid(domain::ErrorCategory::Validation);
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO favorite_update_seen_items(source,source_id,item_id,published_at,first_seen_at) VALUES(?,?,?,?,?) ON CONFLICT(source,source_id,item_id) DO UPDATE SET published_at=excluded.published_at,updated_at=CURRENT_TIMESTAMP"));
        query.addBindValue(item.source);
        query.addBindValue(item.sourceId);
        query.addBindValue(item.itemId);
        query.addBindValue(item.publishedAt);
        query.addBindValue(item.discoveredAt.isEmpty() ? item.publishedAt : item.discoveredAt);
        if (!query.exec()) return failureVoid(queryFailure(query));
    }
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<void> FavoriteNewsRepository::saveNews(const QVector<FavoriteNewsItem> &items)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    for (const auto &item : items) {
        if (!validSource(item.source) || item.sourceId.isEmpty() || item.itemId.isEmpty()
            || item.publishedAt.isEmpty() || item.discoveredAt.isEmpty() || item.payloadJson.isEmpty())
            return failureVoid(domain::ErrorCategory::Validation);
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO favorite_news_items(source,source_id,item_id,published_at,discovered_at,payload_json) VALUES(?,?,?,?,?,?) ON CONFLICT(source,source_id,item_id) DO UPDATE SET published_at=excluded.published_at,discovered_at=excluded.discovered_at,payload_json=excluded.payload_json,updated_at=CURRENT_TIMESTAMP"));
        query.addBindValue(item.source);
        query.addBindValue(item.sourceId);
        query.addBindValue(item.itemId);
        query.addBindValue(item.publishedAt);
        query.addBindValue(item.discoveredAt);
        query.addBindValue(item.payloadJson);
        if (!query.exec()) return failureVoid(queryFailure(query));
    }
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<QVector<FavoriteNewsItem>> FavoriteNewsRepository::recentNews(int limit) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (limit < 1 || limit > 500) return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT source,source_id,item_id,published_at,discovered_at,payload_json FROM favorite_news_items ORDER BY discovered_at DESC,published_at DESC,source,source_id,item_id LIMIT ?"));
    query.addBindValue(limit);
    if (!query.exec()) return failure<QVector<FavoriteNewsItem>>(queryFailure(query));
    RepositoryResult<QVector<FavoriteNewsItem>> result;
    while (query.next()) result.value.push_back({query.value(0).toString(), query.value(1).toString(),
        query.value(2).toString(), query.value(3).toString(), query.value(4).toString(), query.value(5).toString()});
    return result;
}

RepositoryResult<QVector<FavoriteNewsItem>> FavoriteNewsRepository::newsForScan(
    QString scanStartedAt, int limit) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (scanStartedAt.trimmed().isEmpty() || limit < 1 || limit > 500)
        return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT source,source_id,item_id,published_at,discovered_at,payload_json "
        "FROM favorite_news_items WHERE discovered_at=? "
        "ORDER BY published_at DESC,discovered_at DESC,source,source_id,item_id LIMIT ?"));
    query.addBindValue(scanStartedAt.trimmed());
    query.addBindValue(limit);
    if (!query.exec()) return failure<QVector<FavoriteNewsItem>>(queryFailure(query));
    RepositoryResult<QVector<FavoriteNewsItem>> result;
    while (query.next()) result.value.push_back({query.value(0).toString(), query.value(1).toString(),
        query.value(2).toString(), query.value(3).toString(), query.value(4).toString(), query.value(5).toString()});
    return result;
}

RepositoryResult<void> FavoriteNewsRepository::saveScanState(const FavoriteNewsState &state)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (state.scanStartedAt.isEmpty() || state.errorsJson.isEmpty()) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO favorite_news_state(id,scan_started_at,errors_json,updated_at) VALUES(1,?,?,CURRENT_TIMESTAMP) ON CONFLICT(id) DO UPDATE SET scan_started_at=excluded.scan_started_at,errors_json=excluded.errors_json,updated_at=CURRENT_TIMESTAMP"));
    query.addBindValue(state.scanStartedAt);
    query.addBindValue(state.errorsJson);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<std::optional<FavoriteNewsState>> FavoriteNewsRepository::scanState() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT scan_started_at,errors_json FROM favorite_news_state WHERE id=1")))
        return failure<std::optional<FavoriteNewsState>>(queryFailure(query));
    RepositoryResult<std::optional<FavoriteNewsState>> result;
    if (query.next()) result.value = FavoriteNewsState{query.value(0).toString(), query.value(1).toString()};
    return result;
}

RepositoryResult<void> FavoriteNewsRepository::pruneBefore(QString discoveredBefore)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (discoveredBefore.isEmpty()) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM favorite_news_items WHERE discovered_at<?"));
    query.addBindValue(discoveredBefore);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<QVector<FavoriteNewsItem>> FavoriteNewsRepository::commitScan(
    const FavoriteNewsScanCommit &scan,
    const std::function<bool()> &cancellationRequested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (scan.scanStartedAt.trimmed().isEmpty() || scan.errorsJson.trimmed().isEmpty())
        return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Validation);
    auto parseTimestamp = [](const QString &value) {
        QDateTime time = QDateTime::fromString(value, Qt::ISODateWithMs);
        if (!time.isValid()) time = QDateTime::fromString(value, Qt::ISODate);
        return time;
    };
    const QDateTime scanTime = parseTimestamp(scan.scanStartedAt);
    if (!scanTime.isValid()) return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Validation);
    const QString scanAt = scanTime.toUTC().toString(Qt::ISODateWithMs);

    Transaction transaction(m_database);
    if (!transaction.active()) return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Storage);
    QVector<FavoriteNewsItem> newlyDiscovered;

    auto upsertCheckpoint = [this](const FavoriteCheckpoint &checkpoint) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "INSERT INTO favorite_update_checkpoints(source,source_id,checked_through,last_success_at,last_attempted_at,last_error,updated_at) "
            "VALUES(?,?,?,?,?,?,CURRENT_TIMESTAMP) ON CONFLICT(source,source_id) DO UPDATE SET "
            "checked_through=excluded.checked_through,last_success_at=excluded.last_success_at," 
            "last_attempted_at=excluded.last_attempted_at,last_error=excluded.last_error,updated_at=CURRENT_TIMESTAMP"));
        query.addBindValue(checkpoint.source);
        query.addBindValue(checkpoint.sourceId);
        query.addBindValue(checkpoint.checkedThrough);
        query.addBindValue(nullableString(checkpoint.lastSuccessAt));
        query.addBindValue(nullableString(checkpoint.lastAttemptedAt));
        query.addBindValue(nullableString(checkpoint.lastError));
        return query.exec() ? domain::ErrorCategory::None : queryFailure(query);
    };

    for (const FavoriteNewsSourceScan &source : scan.sources) {
        if (cancellationRequested && cancellationRequested())
            return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Cancelled);
        if (!validSource(source.source) || source.sourceId.trimmed().isEmpty() ||
            source.checkedThrough.trimmed().isEmpty())
            return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Validation);
        if (!source.succeeded) {
            QString lastSuccessAt;
            QSqlQuery previous(m_database);
            previous.prepare(QStringLiteral(
                "SELECT COALESCE(last_success_at,'') FROM favorite_update_checkpoints "
                "WHERE source=? AND source_id=?"));
            previous.addBindValue(source.source);
            previous.addBindValue(source.sourceId);
            if (!previous.exec()) return failure<QVector<FavoriteNewsItem>>(queryFailure(previous));
            if (previous.next()) lastSuccessAt = previous.value(0).toString();
            FavoriteCheckpoint checkpoint{source.source, source.sourceId, source.checkedThrough,
                                           lastSuccessAt, scanAt, source.error};
            const auto checkpointError = upsertCheckpoint(checkpoint);
            if (checkpointError != domain::ErrorCategory::None)
                return failure<QVector<FavoriteNewsItem>>(checkpointError);
            continue;
        }

        const QDateTime checkedTime = parseTimestamp(source.checkedThrough);
        const QString checkedAt = checkedTime.isValid()
            ? checkedTime.toUTC().toString(Qt::ISODateWithMs) : QString();
        if (checkedAt.isEmpty()) return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Validation);
        const QString windowStartInput = source.updateWindowStart.trimmed().isEmpty()
            ? source.checkedThrough : source.updateWindowStart;
        const QDateTime windowStartTime = parseTimestamp(windowStartInput);
        const QString windowStart = windowStartTime.isValid()
            ? windowStartTime.toUTC().toString(Qt::ISODateWithMs) : QString();
        if (windowStart.isEmpty()) return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Validation);

        for (const FavoriteNewsItem &item : source.items) {
            if (cancellationRequested && cancellationRequested())
                return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Cancelled);
            const QDateTime published = parseTimestamp(item.publishedAt);
            if (item.source != source.source || item.sourceId != source.sourceId ||
                item.itemId.trimmed().isEmpty() || item.payloadJson.trimmed().isEmpty() || !published.isValid())
                return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Validation);
            const QString publishedAt = published.toUTC().toString(Qt::ISODateWithMs);

            QString firstSeenAt;
            bool existed = false;
            QSqlQuery seen(m_database);
            seen.prepare(QStringLiteral("SELECT first_seen_at FROM favorite_update_seen_items WHERE source=? AND source_id=? AND item_id=?"));
            seen.addBindValue(source.source);
            seen.addBindValue(source.sourceId);
            seen.addBindValue(item.itemId);
            if (!seen.exec()) return failure<QVector<FavoriteNewsItem>>(queryFailure(seen));
            if (seen.next()) {
                existed = true;
                firstSeenAt = seen.value(0).toString();
            } else if (!source.sourceHasSeenItems && publishedAt <= checkedAt) {
                firstSeenAt = checkedAt;
            } else {
                firstSeenAt = scanAt;
            }

            QSqlQuery saveSeen(m_database);
            saveSeen.prepare(QStringLiteral(
                "INSERT INTO favorite_update_seen_items(source,source_id,item_id,published_at,first_seen_at) "
                "VALUES(?,?,?,?,?) ON CONFLICT(source,source_id,item_id) DO UPDATE SET "
                "published_at=excluded.published_at,updated_at=CURRENT_TIMESTAMP"));
            saveSeen.addBindValue(item.source);
            saveSeen.addBindValue(item.sourceId);
            saveSeen.addBindValue(item.itemId);
            saveSeen.addBindValue(publishedAt);
            saveSeen.addBindValue(firstSeenAt.isEmpty() ? scanAt : firstSeenAt);
            if (!saveSeen.exec()) return failure<QVector<FavoriteNewsItem>>(queryFailure(saveSeen));

            const QString firstSeenAtUtc = parseTimestamp(firstSeenAt.isEmpty() ? scanAt : firstSeenAt)
                .toUTC().toString(Qt::ISODateWithMs);
            const bool inWindow = !existed && firstSeenAtUtc > windowStart && firstSeenAtUtc <= scanAt;
            if (inWindow) {
                QSqlQuery saveNews(m_database);
                saveNews.prepare(QStringLiteral(
                    "INSERT INTO favorite_news_items(source,source_id,item_id,published_at,discovered_at,payload_json) "
                    "VALUES(?,?,?,?,?,?) ON CONFLICT(source,source_id,item_id) DO UPDATE SET "
                    "published_at=excluded.published_at,payload_json=excluded.payload_json,updated_at=CURRENT_TIMESTAMP"));
                saveNews.addBindValue(item.source);
                saveNews.addBindValue(item.sourceId);
                saveNews.addBindValue(item.itemId);
                saveNews.addBindValue(publishedAt);
                saveNews.addBindValue(scanAt);
                saveNews.addBindValue(item.payloadJson);
                if (!saveNews.exec()) return failure<QVector<FavoriteNewsItem>>(queryFailure(saveNews));
                FavoriteNewsItem discovered = item;
                discovered.publishedAt = publishedAt;
                discovered.discoveredAt = scanAt;
                newlyDiscovered.push_back(std::move(discovered));
            } else {
                QSqlQuery updateNews(m_database);
                updateNews.prepare(QStringLiteral(
                    "UPDATE favorite_news_items SET published_at=?,payload_json=?,updated_at=CURRENT_TIMESTAMP "
                    "WHERE source=? AND source_id=? AND item_id=?"));
                updateNews.addBindValue(publishedAt);
                updateNews.addBindValue(item.payloadJson);
                updateNews.addBindValue(item.source);
                updateNews.addBindValue(item.sourceId);
                updateNews.addBindValue(item.itemId);
                if (!updateNews.exec()) return failure<QVector<FavoriteNewsItem>>(queryFailure(updateNews));
            }
        }

        const auto checkpointError = upsertCheckpoint(FavoriteCheckpoint{
            source.source, source.sourceId, scanAt, scanAt, scanAt, {}});
        if (checkpointError != domain::ErrorCategory::None)
            return failure<QVector<FavoriteNewsItem>>(checkpointError);
    }

    QSqlQuery state(m_database);
    state.prepare(QStringLiteral("INSERT INTO favorite_news_state(id,scan_started_at,errors_json,updated_at) "
                                 "VALUES(1,?,?,CURRENT_TIMESTAMP) ON CONFLICT(id) DO UPDATE SET "
                                 "scan_started_at=excluded.scan_started_at,errors_json=excluded.errors_json,updated_at=CURRENT_TIMESTAMP"));
    state.addBindValue(scanAt);
    state.addBindValue(scan.errorsJson);
    if (!state.exec()) return failure<QVector<FavoriteNewsItem>>(queryFailure(state));

    QString lastRefresh;
    QSqlQuery current(m_database);
    current.prepare(QStringLiteral("SELECT value FROM preferences WHERE key='favorite_updates.last_refresh_at'"));
    if (!current.exec()) return failure<QVector<FavoriteNewsItem>>(queryFailure(current));
    if (current.next()) lastRefresh = current.value(0).toString();
    if (lastRefresh.isEmpty()) {
        QSqlQuery opened(m_database);
        opened.prepare(QStringLiteral("SELECT value FROM preferences WHERE key='favorite_updates.current_opened_at'"));
        if (!opened.exec()) return failure<QVector<FavoriteNewsItem>>(queryFailure(opened));
        if (opened.next()) lastRefresh = opened.value(0).toString();
    }
    if (lastRefresh.isEmpty()) lastRefresh = scanAt;
    QSqlQuery previousRefresh(m_database);
    previousRefresh.prepare(QStringLiteral("INSERT INTO preferences(key,value) VALUES('favorite_updates.previous_refresh_at',?) "
                                           "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    previousRefresh.addBindValue(lastRefresh);
    if (!previousRefresh.exec()) return failure<QVector<FavoriteNewsItem>>(queryFailure(previousRefresh));
    QSqlQuery saveRefresh(m_database);
    saveRefresh.prepare(QStringLiteral("INSERT INTO preferences(key,value) VALUES('favorite_updates.last_refresh_at',?) "
                                       "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    saveRefresh.addBindValue(scanAt);
    if (!saveRefresh.exec()) return failure<QVector<FavoriteNewsItem>>(queryFailure(saveRefresh));

    if (cancellationRequested && cancellationRequested())
        return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Cancelled);
    if (!transaction.commit()) return failure<QVector<FavoriteNewsItem>>(domain::ErrorCategory::Storage);
    RepositoryResult<QVector<FavoriteNewsItem>> result;
    result.value = std::move(newlyDiscovered);
    return result;
}

WeatherRepository::WeatherRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<std::optional<WeatherLocation>> WeatherRepository::load() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT latitude,longitude,created_at,updated_at,COALESCE(forecast_json,''),COALESCE(forecast_updated_at,'') FROM saved_weather WHERE id=1")))
        return failure<std::optional<WeatherLocation>>(queryFailure(query));
    RepositoryResult<std::optional<WeatherLocation>> result;
    if (query.next()) result.value = WeatherLocation{query.value(0).toDouble(), query.value(1).toDouble(),
        query.value(2).toString(), query.value(3).toString(), query.value(4).toString(), query.value(5).toString()};
    return result;
}

RepositoryResult<void> WeatherRepository::saveLocation(double latitude, double longitude)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || latitude < -90 || latitude > 90
        || longitude < -180 || longitude > 180) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO saved_weather(id,latitude,longitude) VALUES(1,?,?) ON CONFLICT(id) DO UPDATE SET latitude=excluded.latitude,longitude=excluded.longitude,updated_at=CURRENT_TIMESTAMP,forecast_json=CASE WHEN saved_weather.latitude<>excluded.latitude OR saved_weather.longitude<>excluded.longitude THEN NULL ELSE saved_weather.forecast_json END,forecast_updated_at=CASE WHEN saved_weather.latitude<>excluded.latitude OR saved_weather.longitude<>excluded.longitude THEN NULL ELSE saved_weather.forecast_updated_at END"));
    query.addBindValue(latitude);
    query.addBindValue(longitude);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<void> WeatherRepository::saveLocationAndForecast(double latitude,
                                                                  double longitude,
                                                                  QString forecastJson,
                                                                  QString updatedAt)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || latitude < -90 || latitude > 90
        || longitude < -180 || longitude > 180 || forecastJson.isEmpty() || updatedAt.isEmpty()) {
        return failureVoid(domain::ErrorCategory::Validation);
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO saved_weather(id,latitude,longitude,forecast_json,forecast_updated_at) VALUES(1,?,?,?,?) ON CONFLICT(id) DO UPDATE SET latitude=excluded.latitude,longitude=excluded.longitude,updated_at=CURRENT_TIMESTAMP,forecast_json=excluded.forecast_json,forecast_updated_at=excluded.forecast_updated_at"));
    query.addBindValue(latitude);
    query.addBindValue(longitude);
    query.addBindValue(forecastJson);
    query.addBindValue(updatedAt);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<void> WeatherRepository::saveForecast(QString forecastJson, QString updatedAt)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (forecastJson.isEmpty() || updatedAt.isEmpty()) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE saved_weather SET forecast_json=?,forecast_updated_at=? WHERE id=1"));
    query.addBindValue(forecastJson);
    query.addBindValue(updatedAt);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return query.numRowsAffected() == 1 ? RepositoryResult<void>{} : failureVoid(domain::ErrorCategory::NotFound);
}

RepositoryResult<void> WeatherRepository::clear()
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("DELETE FROM saved_weather WHERE id=1"))) return failureVoid(queryFailure(query));
    return {};
}

WeatherComparisonRepository::WeatherComparisonRepository(QSqlDatabase &database)
    : m_database(database)
{
}

RepositoryResult<QVector<WeatherComparisonLocation>> WeatherComparisonRepository::load() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT geocoding_id,display_name,administrative_area,country,timezone,latitude,longitude,"
            "display_order,COALESCE(forecast_json,''),COALESCE(forecast_updated_at,'') "
            "FROM weather_comparison_cities ORDER BY display_order,identity"))) {
        return failure<QVector<WeatherComparisonLocation>>(queryFailure(query));
    }
    RepositoryResult<QVector<WeatherComparisonLocation>> result;
    while (query.next()) {
        domain::WeatherCity city;
        city.geocodingId = query.value(0).toLongLong();
        city.name = query.value(1).toString();
        city.administrativeArea = query.value(2).toString();
        city.country = query.value(3).toString();
        city.timezone = query.value(4).toString();
        city.latitude = query.value(5).toDouble();
        city.longitude = query.value(6).toDouble();
        city.found = true;
        result.value.push_back(WeatherComparisonLocation{
            std::move(city), query.value(7).toInt(), query.value(8).toString(),
            query.value(9).toString()});
    }
    return result;
}

RepositoryResult<void> WeatherComparisonRepository::add(
    const domain::WeatherCity &city, QString forecastJson, QString forecastUpdatedAt)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    const QString identity = domain::stableWeatherCityIdentity(city);
    if (identity.isEmpty() || city.geocodingId < 0
        || city.name.trimmed().isEmpty() || city.name.size() > 120
        || city.administrativeArea.size() > 120 || city.country.size() > 120
        || city.timezone.size() > 100
        || !domain::isValidWeatherCoordinates(city.latitude, city.longitude)
        || (forecastJson.isEmpty() != forecastUpdatedAt.isEmpty())) {
        return failureVoid(domain::ErrorCategory::Validation);
    }

    const auto existing = load();
    if (!existing.succeeded()) return failureVoid(existing.error.category());
    if (existing.value.size() >= 4) return failureVoid(domain::ErrorCategory::Conflict);
    for (const WeatherComparisonLocation &entry : existing.value) {
        if (domain::isSameWeatherLocation(entry.city, city))
            return failureVoid(domain::ErrorCategory::Conflict);
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO weather_comparison_cities(identity,geocoding_id,display_name,administrative_area,"
        "country,timezone,latitude,longitude,display_order,forecast_json,forecast_updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(identity);
    if (city.geocodingId > 0) query.addBindValue(city.geocodingId);
    else query.addBindValue(QVariant());
    query.addBindValue(city.name.trimmed());
    query.addBindValue(sqlText(city.administrativeArea));
    query.addBindValue(sqlText(city.country));
    query.addBindValue(sqlText(city.timezone));
    query.addBindValue(city.latitude);
    query.addBindValue(city.longitude);
    query.addBindValue(existing.value.size());
    if (forecastJson.isEmpty()) query.addBindValue(QVariant());
    else query.addBindValue(forecastJson);
    if (forecastUpdatedAt.isEmpty()) query.addBindValue(QVariant());
    else query.addBindValue(forecastUpdatedAt);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<void> WeatherComparisonRepository::replace(
    const QString &identity, const domain::WeatherCity &city,
    QString forecastJson, QString forecastUpdatedAt)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    const QString newIdentity = domain::stableWeatherCityIdentity(city);
    if (identity.trimmed().isEmpty() || newIdentity.isEmpty() || city.geocodingId < 0
        || city.name.trimmed().isEmpty() || city.name.size() > 120
        || city.administrativeArea.size() > 120 || city.country.size() > 120
        || city.timezone.size() > 100 || !domain::isValidWeatherCoordinates(city.latitude, city.longitude)
        || forecastJson.isEmpty() || forecastUpdatedAt.isEmpty()) {
        return failureVoid(domain::ErrorCategory::Validation);
    }
    if (!m_database.transaction()) return failureVoid(domain::ErrorCategory::Storage);
    const auto rollback = [this] { m_database.rollback(); };
    const auto existing = load();
    if (!existing.succeeded()) {
        rollback();
        return failureVoid(existing.error.category());
    }
    bool targetFound = false;
    for (const WeatherComparisonLocation &entry : existing.value) {
        const QString existingIdentity = domain::stableWeatherCityIdentity(entry.city);
        if (existingIdentity == identity) {
            targetFound = true;
            continue;
        }
        if (domain::isSameWeatherLocation(entry.city, city)) {
            rollback();
            return failureVoid(domain::ErrorCategory::Conflict);
        }
    }
    if (!targetFound) {
        rollback();
        return failureVoid(domain::ErrorCategory::NotFound);
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE weather_comparison_cities SET identity=?,geocoding_id=?,display_name=?,"
        "administrative_area=?,country=?,timezone=?,latitude=?,longitude=?,forecast_json=?,"
        "forecast_updated_at=?,updated_at=CURRENT_TIMESTAMP WHERE identity=?"));
    query.addBindValue(newIdentity);
    if (city.geocodingId > 0) query.addBindValue(city.geocodingId);
    else query.addBindValue(QVariant());
    query.addBindValue(city.name.trimmed());
    query.addBindValue(sqlText(city.administrativeArea));
    query.addBindValue(sqlText(city.country));
    query.addBindValue(sqlText(city.timezone));
    query.addBindValue(city.latitude);
    query.addBindValue(city.longitude);
    query.addBindValue(forecastJson);
    query.addBindValue(forecastUpdatedAt);
    query.addBindValue(identity);
    if (!query.exec()) {
        const auto category = queryFailure(query);
        rollback();
        return failureVoid(category);
    }
    if (query.numRowsAffected() != 1) {
        rollback();
        return failureVoid(domain::ErrorCategory::NotFound);
    }
    if (!m_database.commit()) {
        rollback();
        return failureVoid(domain::ErrorCategory::Storage);
    }
    return {};
}

RepositoryResult<void> WeatherComparisonRepository::remove(const QString &identity)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (identity.trimmed().isEmpty()) return failureVoid(domain::ErrorCategory::Validation);
    if (!m_database.transaction()) return failureVoid(domain::ErrorCategory::Storage);
    QSqlQuery removeQuery(m_database);
    removeQuery.prepare(QStringLiteral("DELETE FROM weather_comparison_cities WHERE identity=?"));
    removeQuery.addBindValue(identity);
    if (!removeQuery.exec()) {
        m_database.rollback();
        return failureVoid(queryFailure(removeQuery));
    }
    if (removeQuery.numRowsAffected() != 1) {
        m_database.rollback();
        return failureVoid(domain::ErrorCategory::NotFound);
    }
    QSqlQuery listQuery(m_database);
    if (!listQuery.exec(QStringLiteral(
            "SELECT identity FROM weather_comparison_cities ORDER BY display_order,identity"))) {
        m_database.rollback();
        return failureVoid(queryFailure(listQuery));
    }
    QVector<QString> identities;
    while (listQuery.next()) identities.push_back(listQuery.value(0).toString());
    QSqlQuery updateQuery(m_database);
    updateQuery.prepare(QStringLiteral(
        "UPDATE weather_comparison_cities SET display_order=?,updated_at=CURRENT_TIMESTAMP WHERE identity=?"));
    for (qsizetype row = 0; row < identities.size(); ++row) {
        updateQuery.bindValue(0, static_cast<int>(row));
        updateQuery.bindValue(1, identities.at(row));
        if (!updateQuery.exec()) {
            const auto category = queryFailure(updateQuery);
            m_database.rollback();
            return failureVoid(category);
        }
    }
    if (!m_database.commit()) {
        m_database.rollback();
        return failureVoid(domain::ErrorCategory::Storage);
    }
    return {};
}

RepositoryResult<void> WeatherComparisonRepository::reorder(
    const QVector<QString> &orderedIdentities)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    const auto existing = load();
    if (!existing.succeeded()) return failureVoid(existing.error.category());
    if (orderedIdentities.size() != existing.value.size() || orderedIdentities.size() > 4)
        return failureVoid(domain::ErrorCategory::Validation);
    QSet<QString> requested;
    for (const QString &identity : orderedIdentities) {
        if (identity.trimmed().isEmpty() || requested.contains(identity))
            return failureVoid(domain::ErrorCategory::Validation);
        requested.insert(identity);
    }
    for (const WeatherComparisonLocation &entry : existing.value) {
        if (!requested.contains(domain::stableWeatherCityIdentity(entry.city)))
            return failureVoid(domain::ErrorCategory::Conflict);
    }

    if (!m_database.transaction()) return failureVoid(domain::ErrorCategory::Storage);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE weather_comparison_cities SET display_order=?,updated_at=CURRENT_TIMESTAMP WHERE identity=?"));
    for (qsizetype row = 0; row < orderedIdentities.size(); ++row) {
        query.bindValue(0, static_cast<int>(row));
        query.bindValue(1, orderedIdentities.at(row));
        if (!query.exec() || query.numRowsAffected() != 1) {
            const auto category = query.lastError().isValid()
                ? queryFailure(query) : domain::ErrorCategory::Conflict;
            m_database.rollback();
            return failureVoid(category);
        }
    }
    if (!m_database.commit()) {
        m_database.rollback();
        return failureVoid(domain::ErrorCategory::Storage);
    }
    return {};
}

RepositoryResult<void> WeatherComparisonRepository::saveForecast(
    const QString &identity, QString forecastJson, QString forecastUpdatedAt)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (identity.trimmed().isEmpty() || forecastJson.isEmpty() || forecastUpdatedAt.isEmpty())
        return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE weather_comparison_cities SET forecast_json=?,forecast_updated_at=?,"
        "updated_at=CURRENT_TIMESTAMP WHERE identity=?"));
    query.addBindValue(forecastJson);
    query.addBindValue(forecastUpdatedAt);
    query.addBindValue(identity);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return query.numRowsAffected() == 1 ? RepositoryResult<void>{}
                                        : failureVoid(domain::ErrorCategory::NotFound);
}

PreferencesRepository::PreferencesRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<QString> PreferencesRepository::get(QString key) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    key = key.trimmed();
    static const QSet<QString> retainedKeys{
        QStringLiteral("tasks.week_start"), QStringLiteral("appearance.selected_wallpaper"),
        QStringLiteral("calendar.game_releases_visible"),
        QStringLiteral("favorite_updates.previous_opened_at"),
        QStringLiteral("favorite_updates.current_opened_at"),
        QStringLiteral("favorite_updates.previous_refresh_at"),
        QStringLiteral("favorite_updates.last_refresh_at")
    };
    if (!retainedKeys.contains(key)) return failure<QString>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT value FROM preferences WHERE key=?"));
    query.addBindValue(key);
    if (!query.exec()) return failure<QString>(queryFailure(query));
    if (!query.next()) return failure<QString>(domain::ErrorCategory::NotFound);
    RepositoryResult<QString> result;
    result.value = query.value(0).toString();
    return result;
}

RepositoryResult<void> PreferencesRepository::set(QString key, QString value)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    key = key.trimmed();
    static const QSet<QString> retainedKeys{
        QStringLiteral("tasks.week_start"), QStringLiteral("appearance.selected_wallpaper"),
        QStringLiteral("calendar.game_releases_visible"),
        QStringLiteral("favorite_updates.previous_opened_at"),
        QStringLiteral("favorite_updates.current_opened_at"),
        QStringLiteral("favorite_updates.previous_refresh_at"),
        QStringLiteral("favorite_updates.last_refresh_at")
    };
    if (!retainedKeys.contains(key)) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO preferences(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    query.addBindValue(key);
    query.addBindValue(value);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<int> PreferencesRepository::weekStart() const
{
    const auto result = get(QStringLiteral("tasks.week_start"));
    if (result.error.category() == domain::ErrorCategory::NotFound) {
        RepositoryResult<int> fallback;
        fallback.value = 1; // observed default: Monday
        return fallback;
    }
    if (!result.succeeded()) return failure<int>(result.error.category());
    bool ok = false;
    const int value = result.value.toInt(&ok);
    if (!ok || value < 0 || value > 6) {
        RepositoryResult<int> fallback;
        fallback.value = 1;
        return fallback;
    }
    RepositoryResult<int> parsed;
    parsed.value = value;
    return parsed;
}

RepositoryResult<void> PreferencesRepository::setWeekStart(int sundayZero)
{
    if (sundayZero < 0 || sundayZero > 6) return failureVoid(domain::ErrorCategory::Validation);
    return set(QStringLiteral("tasks.week_start"), QString::number(sundayZero));
}

RepositoryResult<bool> PreferencesRepository::gameReleasesVisibleInCalendar() const
{
    const auto stored = get(QStringLiteral("calendar.game_releases_visible"));
    if (stored.error.category() == domain::ErrorCategory::NotFound) {
        RepositoryResult<bool> fallback;
        fallback.value = true;
        return fallback;
    }
    if (!stored.succeeded()) return failure<bool>(stored.error.category());
    if (stored.value == QStringLiteral("1")) {
        RepositoryResult<bool> visible;
        visible.value = true;
        return visible;
    }
    if (stored.value == QStringLiteral("0")) {
        RepositoryResult<bool> hidden;
        hidden.value = false;
        return hidden;
    }
    return failure<bool>(domain::ErrorCategory::Storage);
}

RepositoryResult<void> PreferencesRepository::setGameReleasesVisibleInCalendar(bool visible)
{
    return set(QStringLiteral("calendar.game_releases_visible"),
               visible ? QStringLiteral("1") : QStringLiteral("0"));
}

RepositoryResult<QString> PreferencesRepository::selectedWallpaper() const
{
    return get(QStringLiteral("appearance.selected_wallpaper"));
}

RepositoryResult<void> PreferencesRepository::setSelectedWallpaper(QString wallpaperId)
{
    if (wallpaperId.trimmed().isEmpty()) return failureVoid(domain::ErrorCategory::Validation);
    return set(QStringLiteral("appearance.selected_wallpaper"), wallpaperId.trimmed());
}

DesktopApplicationsRepository::DesktopApplicationsRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<QVector<DesktopApplication>> DesktopApplicationsRepository::list() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    return listFiltered(std::nullopt, false);
}

RepositoryResult<QVector<DesktopApplication>> DesktopApplicationsRepository::listByCategory(qint64 categoryId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (categoryId <= 0) return failure<QVector<DesktopApplication>>(domain::ErrorCategory::Validation);
    QSqlQuery exists(m_database);
    exists.prepare(QStringLiteral("SELECT 1 FROM application_categories WHERE id=?"));
    exists.addBindValue(categoryId);
    if (!exists.exec()) return failure<QVector<DesktopApplication>>(queryFailure(exists));
    if (!exists.next()) return failure<QVector<DesktopApplication>>(domain::ErrorCategory::NotFound);
    exists.finish();
    return listFiltered(categoryId, false);
}

RepositoryResult<QVector<DesktopApplication>> DesktopApplicationsRepository::listUncategorized() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    return listFiltered(std::nullopt, true);
}

RepositoryResult<QVector<DesktopApplication>> DesktopApplicationsRepository::listFiltered(
    std::optional<qint64> categoryId, bool uncategorized) const
{
    QSqlQuery query(m_database);
    QString sql = QStringLiteral("SELECT id FROM desktop_apps");
    if (categoryId) sql += QStringLiteral(" WHERE category_id=?");
    else if (uncategorized) sql += QStringLiteral(" WHERE category_id IS NULL");
    sql += QStringLiteral(" ORDER BY display_name COLLATE NOCASE,id");
    if (categoryId) {
        query.prepare(sql);
        query.addBindValue(*categoryId);
    } else if (!query.exec(sql)) {
        return failure<QVector<DesktopApplication>>(queryFailure(query));
    }
    if (categoryId && !query.exec()) return failure<QVector<DesktopApplication>>(queryFailure(query));
    QVector<qint64> ids;
    while (query.next()) ids.push_back(query.value(0).toLongLong());
    query.finish();
    RepositoryResult<QVector<DesktopApplication>> result;
    for (qint64 id : ids) {
        const auto item = load(id);
        if (!item.succeeded()) return failure<QVector<DesktopApplication>>(item.error.category());
        result.value.push_back(item.value);
    }
    return result;
}

RepositoryResult<QVector<DesktopApplicationCategory>> DesktopApplicationsRepository::listCategories() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
        "SELECT c.id,c.name,c.description,c.color,c.icon_identifier,c.display_order,"
        "c.created_at,c.updated_at,COUNT(a.id) "
        "FROM application_categories c LEFT JOIN desktop_apps a ON a.category_id=c.id "
        "GROUP BY c.id ORDER BY c.display_order,c.name_normalized,c.id")))
        return failure<QVector<DesktopApplicationCategory>>(queryFailure(query));
    RepositoryResult<QVector<DesktopApplicationCategory>> result;
    while (query.next()) {
        DesktopApplicationCategory category;
        category.id = query.value(0).toLongLong();
        category.name = query.value(1).toString();
        if (!query.value(2).isNull()) category.description = query.value(2).toString();
        if (!query.value(3).isNull()) category.color = query.value(3).toString();
        if (!query.value(4).isNull()) category.iconIdentifier = query.value(4).toString();
        category.displayOrder = query.value(5).toInt();
        category.createdAt = query.value(6).toString();
        category.updatedAt = query.value(7).toString();
        category.applicationCount = query.value(8).toInt();
        result.value.push_back(std::move(category));
    }
    return result;
}

RepositoryResult<int> DesktopApplicationsRepository::uncategorizedApplicationCount() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM desktop_apps WHERE category_id IS NULL"))
        || !query.next()) return failure<int>(queryFailure(query));
    return {query.value(0).toInt(), {}};
}

RepositoryResult<DesktopApplicationCategory> DesktopApplicationsRepository::loadCategory(qint64 id) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT c.id,c.name,c.description,c.color,c.icon_identifier,c.display_order,"
        "c.created_at,c.updated_at,COUNT(a.id) "
        "FROM application_categories c LEFT JOIN desktop_apps a ON a.category_id=c.id "
        "WHERE c.id=? GROUP BY c.id"));
    query.addBindValue(id);
    if (!query.exec()) return failure<DesktopApplicationCategory>(queryFailure(query));
    if (!query.next()) return failure<DesktopApplicationCategory>(domain::ErrorCategory::NotFound);
    DesktopApplicationCategory category;
    category.id = query.value(0).toLongLong();
    category.name = query.value(1).toString();
    if (!query.value(2).isNull()) category.description = query.value(2).toString();
    if (!query.value(3).isNull()) category.color = query.value(3).toString();
    if (!query.value(4).isNull()) category.iconIdentifier = query.value(4).toString();
    category.displayOrder = query.value(5).toInt();
    category.createdAt = query.value(6).toString();
    category.updatedAt = query.value(7).toString();
    category.applicationCount = query.value(8).toInt();
    return {std::move(category), {}};
}

RepositoryResult<DesktopApplicationCategory> DesktopApplicationsRepository::createCategory(
    QString name, QString description, QString color, QString iconIdentifier)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    name = name.trimmed();
    description = description.trimmed();
    iconIdentifier = iconIdentifier.trimmed();
    QString normalizedColor;
    if (name.isEmpty() || name.toUcs4().size() > 80 || description.toUcs4().size() > 300
        || iconIdentifier.toUcs4().size() > 80
        || !normalizeCategoryColor(std::move(color), &normalizedColor))
        return failure<DesktopApplicationCategory>(domain::ErrorCategory::Validation);

    Transaction transaction(m_database);
    if (!transaction.active()) return failure<DesktopApplicationCategory>(domain::ErrorCategory::Storage);
    QSqlQuery order(m_database);
    if (!order.exec(QStringLiteral("SELECT COALESCE(MAX(display_order),-1)+1 FROM application_categories"))
        || !order.next()) return failure<DesktopApplicationCategory>(queryFailure(order));
    const int nextOrder = order.value(0).toInt();
    order.finish();
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO application_categories(name,name_normalized,description,color,icon_identifier,display_order) "
        "VALUES(?,?,?,?,?,?)"));
    insert.addBindValue(name);
    insert.addBindValue(normalizedApplicationCategoryName(name));
    insert.addBindValue(nullableString(description));
    insert.addBindValue(nullableString(normalizedColor));
    insert.addBindValue(nullableString(iconIdentifier));
    insert.addBindValue(nextOrder);
    if (!insert.exec()) return failure<DesktopApplicationCategory>(queryFailure(insert));
    const qint64 id = insert.lastInsertId().toLongLong();
    if (!transaction.commit()) return failure<DesktopApplicationCategory>(domain::ErrorCategory::Storage);
    return loadCategory(id);
}

RepositoryResult<DesktopApplicationCategory> DesktopApplicationsRepository::updateCategory(
    qint64 id, QString name, QString description, QString color, QString iconIdentifier)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    name = name.trimmed();
    description = description.trimmed();
    iconIdentifier = iconIdentifier.trimmed();
    QString normalizedColor;
    if (id <= 0 || name.isEmpty() || name.toUcs4().size() > 80 || description.toUcs4().size() > 300
        || iconIdentifier.toUcs4().size() > 80
        || !normalizeCategoryColor(std::move(color), &normalizedColor))
        return failure<DesktopApplicationCategory>(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<DesktopApplicationCategory>(domain::ErrorCategory::Storage);
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE application_categories SET name=?,name_normalized=?,description=?,color=?,"
        "icon_identifier=?,updated_at=CURRENT_TIMESTAMP WHERE id=?"));
    update.addBindValue(name);
    update.addBindValue(normalizedApplicationCategoryName(name));
    update.addBindValue(nullableString(description));
    update.addBindValue(nullableString(normalizedColor));
    update.addBindValue(nullableString(iconIdentifier));
    update.addBindValue(id);
    if (!update.exec()) return failure<DesktopApplicationCategory>(queryFailure(update));
    if (update.numRowsAffected() != 1) return failure<DesktopApplicationCategory>(domain::ErrorCategory::NotFound);
    if (!transaction.commit()) return failure<DesktopApplicationCategory>(domain::ErrorCategory::Storage);
    return loadCategory(id);
}

RepositoryResult<void> DesktopApplicationsRepository::reorderCategories(const QVector<qint64> &orderedIds)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    QSqlQuery existing(m_database);
    if (!existing.exec(QStringLiteral("SELECT id FROM application_categories ORDER BY display_order,id")))
        return failureVoid(queryFailure(existing));
    QSet<qint64> expected;
    while (existing.next()) expected.insert(existing.value(0).toLongLong());
    existing.finish();
    QSet<qint64> supplied;
    for (qint64 id : orderedIds) {
        if (id <= 0 || supplied.contains(id) || !expected.contains(id))
            return failureVoid(domain::ErrorCategory::Validation);
        supplied.insert(id);
    }
    if (supplied != expected) return failureVoid(domain::ErrorCategory::Validation);
    for (int position = 0; position < orderedIds.size(); ++position) {
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral(
            "UPDATE application_categories SET display_order=?,updated_at=CURRENT_TIMESTAMP WHERE id=?"));
        update.addBindValue(position);
        update.addBindValue(orderedIds[position]);
        if (!update.exec()) return failureVoid(queryFailure(update));
        if (update.numRowsAffected() != 1) return failureVoid(domain::ErrorCategory::Conflict);
    }
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<void> DesktopApplicationsRepository::assignCategory(
    qint64 applicationId, std::optional<qint64> categoryId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (applicationId <= 0 || (categoryId && *categoryId <= 0))
        return failureVoid(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    if (categoryId) {
        QSqlQuery category(m_database);
        category.prepare(QStringLiteral("SELECT 1 FROM application_categories WHERE id=?"));
        category.addBindValue(*categoryId);
        if (!category.exec()) return failureVoid(queryFailure(category));
        if (!category.next()) return failureVoid(domain::ErrorCategory::Validation);
        category.finish();
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE desktop_apps SET category_id=?,updated_at=CURRENT_TIMESTAMP WHERE id=?"));
    update.addBindValue(categoryId ? QVariant(*categoryId) : QVariant());
    update.addBindValue(applicationId);
    if (!update.exec()) return failureVoid(queryFailure(update));
    if (update.numRowsAffected() != 1) return failureVoid(domain::ErrorCategory::NotFound);
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<DesktopApplicationCategoryDeleteResult>
DesktopApplicationsRepository::deleteCategory(qint64 id)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (id <= 0) return failure<DesktopApplicationCategoryDeleteResult>(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<DesktopApplicationCategoryDeleteResult>(domain::ErrorCategory::Storage);
    QSqlQuery count(m_database);
    count.prepare(QStringLiteral("SELECT COUNT(*) FROM desktop_apps WHERE category_id=?"));
    count.addBindValue(id);
    if (!count.exec() || !count.next()) return failure<DesktopApplicationCategoryDeleteResult>(queryFailure(count));
    const int affected = count.value(0).toInt();
    count.finish();
    QSqlQuery unassign(m_database);
    unassign.prepare(QStringLiteral(
        "UPDATE desktop_apps SET category_id=NULL,updated_at=CURRENT_TIMESTAMP WHERE category_id=?"));
    unassign.addBindValue(id);
    if (!unassign.exec()) return failure<DesktopApplicationCategoryDeleteResult>(queryFailure(unassign));
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM application_categories WHERE id=?"));
    remove.addBindValue(id);
    if (!remove.exec()) return failure<DesktopApplicationCategoryDeleteResult>(queryFailure(remove));
    if (remove.numRowsAffected() != 1)
        return failure<DesktopApplicationCategoryDeleteResult>(domain::ErrorCategory::NotFound);
    if (!transaction.commit()) return failure<DesktopApplicationCategoryDeleteResult>(domain::ErrorCategory::Storage);
    return {{id, affected}, {}};
}

RepositoryResult<DesktopApplication> DesktopApplicationsRepository::get(qint64 id) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    return load(id);
}

RepositoryResult<DesktopApplication> DesktopApplicationsRepository::load(qint64 id) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT id,display_name,executable_path,created_at,updated_at,category_id FROM desktop_apps WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec()) return failure<DesktopApplication>(queryFailure(query));
    if (!query.next()) return failure<DesktopApplication>(domain::ErrorCategory::NotFound);
    DesktopApplication app{query.value(0).toLongLong(), query.value(1).toString(), query.value(2).toString(),
                           query.value(3).toString(), query.value(4).toString(), std::nullopt};
    if (!query.value(5).isNull()) app.categoryId = query.value(5).toLongLong();
    query.finish();
    QSqlQuery icon(m_database);
    icon.prepare(QStringLiteral("SELECT filename,mime_type,byte_size FROM desktop_app_icons WHERE app_id=?"));
    icon.addBindValue(id);
    if (!icon.exec()) return failure<DesktopApplication>(queryFailure(icon));
    if (icon.next()) app.icon = AssetMetadata{icon.value(0).toString(), icon.value(1).toString(), icon.value(2).toLongLong()};
    RepositoryResult<DesktopApplication> result;
    result.value = std::move(app);
    return result;
}

RepositoryResult<DesktopApplication> DesktopApplicationsRepository::add(
    QString displayName, QString executablePath, std::optional<AssetMetadata> iconAsset)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    displayName = displayName.trimmed();
    executablePath = executablePath.trimmed();
    if (displayName.isEmpty() || displayName.toUcs4().size() > 120 || executablePath.isEmpty())
        return failure<DesktopApplication>(domain::ErrorCategory::Validation);
    if (iconAsset && !validAsset(*iconAsset, {QStringLiteral("image/jpeg"), QStringLiteral("image/png"),
                                               QStringLiteral("image/webp"), QStringLiteral("image/x-icon")}))
        return failure<DesktopApplication>(domain::ErrorCategory::Validation);
    QSqlQuery duplicate(m_database);
    duplicate.prepare(QStringLiteral("SELECT executable_path FROM desktop_apps"));
    if (!duplicate.exec()) return failure<DesktopApplication>(queryFailure(duplicate));
    bool duplicatePath = false;
    while (duplicate.next())
        duplicatePath = duplicatePath
            || QString::compare(duplicate.value(0).toString(), executablePath, Qt::CaseInsensitive) == 0;
    duplicate.finish();
    if (duplicatePath) return failure<DesktopApplication>(domain::ErrorCategory::Conflict);
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<DesktopApplication>(domain::ErrorCategory::Storage);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO desktop_apps(display_name,executable_path) VALUES(?,?)"));
    query.addBindValue(displayName);
    query.addBindValue(executablePath);
    if (!query.exec()) return failure<DesktopApplication>(queryFailure(query));
    const qint64 id = query.lastInsertId().toLongLong();
    if (iconAsset) {
        QSqlQuery icon(m_database);
        icon.prepare(QStringLiteral("INSERT INTO desktop_app_icons(app_id,filename,mime_type,byte_size) VALUES(?,?,?,?)"));
        icon.addBindValue(id);
        bindAsset(icon, 1, *iconAsset);
        if (!icon.exec()) return failure<DesktopApplication>(queryFailure(icon));
    }
    const auto loaded = load(id);
    if (!loaded.succeeded()) return loaded;
    if (!transaction.commit()) return failure<DesktopApplication>(domain::ErrorCategory::Storage);
    return loaded;
}

RepositoryResult<DesktopApplication> DesktopApplicationsRepository::update(qint64 id, QString displayName,
                                                                             QString executablePath)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    displayName = displayName.trimmed();
    executablePath = executablePath.trimmed();
    if (id <= 0 || displayName.isEmpty() || displayName.toUcs4().size() > 120 || executablePath.isEmpty())
        return failure<DesktopApplication>(domain::ErrorCategory::Validation);
    QSqlQuery duplicate(m_database);
    duplicate.prepare(QStringLiteral("SELECT executable_path FROM desktop_apps WHERE id<>?"));
    duplicate.addBindValue(id);
    if (!duplicate.exec()) return failure<DesktopApplication>(queryFailure(duplicate));
    bool duplicatePath = false;
    while (duplicate.next())
        duplicatePath = duplicatePath
            || QString::compare(duplicate.value(0).toString(), executablePath, Qt::CaseInsensitive) == 0;
    duplicate.finish();
    if (duplicatePath) return failure<DesktopApplication>(domain::ErrorCategory::Conflict);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE desktop_apps SET display_name=?,executable_path=?,updated_at=CURRENT_TIMESTAMP WHERE id=?"));
    query.addBindValue(displayName);
    query.addBindValue(executablePath);
    query.addBindValue(id);
    if (!query.exec()) return failure<DesktopApplication>(queryFailure(query));
    if (query.numRowsAffected() != 1) return failure<DesktopApplication>(domain::ErrorCategory::NotFound);
    return load(id);
}

RepositoryResult<void> DesktopApplicationsRepository::setIcon(qint64 appId, const AssetMetadata &asset)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId <= 0 || !validAsset(asset, {QStringLiteral("image/jpeg"), QStringLiteral("image/png"),
                                           QStringLiteral("image/webp"), QStringLiteral("image/x-icon")}))
        return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO desktop_app_icons(app_id,filename,mime_type,byte_size) VALUES(?,?,?,?) ON CONFLICT(app_id) DO UPDATE SET filename=excluded.filename,mime_type=excluded.mime_type,byte_size=excluded.byte_size,updated_at=CURRENT_TIMESTAMP"));
    query.addBindValue(appId);
    bindAsset(query, 1, asset);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<void> DesktopApplicationsRepository::removeIcon(qint64 appId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId <= 0) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM desktop_app_icons WHERE app_id=?"));
    query.addBindValue(appId);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<void> DesktopApplicationsRepository::remove(qint64 id)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (id <= 0) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM desktop_apps WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return query.numRowsAffected() == 1 ? RepositoryResult<void>{}
                                        : failureVoid(domain::ErrorCategory::NotFound);
}

SetupsRepository::SetupsRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<QVector<Setup>> SetupsRepository::list() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id FROM setups ORDER BY name COLLATE NOCASE,id")))
        return failure<QVector<Setup>>(queryFailure(query));
    QVector<qint64> ids;
    while (query.next()) ids.push_back(query.value(0).toLongLong());
    query.finish();
    RepositoryResult<QVector<Setup>> result;
    for (qint64 id : ids) {
        auto item = load(id);
        if (!item.succeeded()) return failure<QVector<Setup>>(item.error.category());
        result.value.push_back(item.value);
    }
    return result;
}

RepositoryResult<Setup> SetupsRepository::get(qint64 id) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    return load(id);
}

RepositoryResult<Setup> SetupsRepository::load(qint64 id) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT id,name,description,created_at,updated_at FROM setups WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec()) return failure<Setup>(queryFailure(query));
    if (!query.next()) return failure<Setup>(domain::ErrorCategory::NotFound);
    Setup setup;
    setup.id = query.value(0).toLongLong();
    setup.name = query.value(1).toString();
    setup.description = query.value(2).toString();
    setup.createdAt = query.value(3).toString();
    setup.updatedAt = query.value(4).toString();
    query.finish();
    QSqlQuery apps(m_database);
    apps.prepare(QStringLiteral("SELECT app_id FROM setup_apps WHERE setup_id=? ORDER BY position,app_id"));
    apps.addBindValue(id);
    if (!apps.exec()) return failure<Setup>(queryFailure(apps));
    while (apps.next()) setup.applicationIds.push_back(apps.value(0).toLongLong());
    QSqlQuery icon(m_database);
    icon.prepare(QStringLiteral("SELECT filename,mime_type,byte_size FROM setup_icons WHERE setup_id=?"));
    icon.addBindValue(id);
    if (!icon.exec()) return failure<Setup>(queryFailure(icon));
    if (icon.next()) setup.icon = AssetMetadata{icon.value(0).toString(), icon.value(1).toString(), icon.value(2).toLongLong()};
    RepositoryResult<Setup> result;
    result.value = std::move(setup);
    return result;
}

RepositoryResult<Setup> SetupsRepository::save(Setup setup, bool removeIcon)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    setup.name = setup.name.trimmed();
    setup.description = setup.description.trimmed();
    if (setup.id < 0 || setup.name.isEmpty() || setup.name.toUcs4().size() > 120 || setup.description.toUcs4().size() > 300
        || setup.applicationIds.isEmpty() || (setup.icon && removeIcon))
        return failure<Setup>(domain::ErrorCategory::Validation);
    if (setup.icon && !validAsset(*setup.icon, {QStringLiteral("image/jpeg"), QStringLiteral("image/png"),
                                                 QStringLiteral("image/webp"), QStringLiteral("image/x-icon")}))
        return failure<Setup>(domain::ErrorCategory::Validation);
    QSet<qint64> uniqueIds;
    for (qint64 id : setup.applicationIds) {
        if (id <= 0 || uniqueIds.contains(id)) return failure<Setup>(domain::ErrorCategory::Validation);
        uniqueIds.insert(id);
    }
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<Setup>(domain::ErrorCategory::Storage);
    QSqlQuery duplicateName(m_database);
    duplicateName.prepare(QStringLiteral("SELECT name FROM setups WHERE id<>?"));
    duplicateName.addBindValue(setup.id);
    if (!duplicateName.exec()) return failure<Setup>(queryFailure(duplicateName));
    while (duplicateName.next()) {
        if (QString::compare(duplicateName.value(0).toString(), setup.name,
                             Qt::CaseInsensitive) == 0)
            return failure<Setup>(domain::ErrorCategory::Conflict);
    }
    duplicateName.finish();
    if (setup.id <= 0) {
        QSqlQuery insert(m_database);
        insert.prepare(QStringLiteral("INSERT INTO setups(name,description) VALUES(?,?)"));
        insert.addBindValue(setup.name);
        insert.addBindValue(sqlText(setup.description));
        if (!insert.exec()) return failure<Setup>(queryFailure(insert));
        setup.id = insert.lastInsertId().toLongLong();
    } else {
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral("UPDATE setups SET name=?,description=?,updated_at=CURRENT_TIMESTAMP WHERE id=?"));
        update.addBindValue(setup.name);
        update.addBindValue(sqlText(setup.description));
        update.addBindValue(setup.id);
        if (!update.exec()) return failure<Setup>(queryFailure(update));
        if (update.numRowsAffected() != 1) return failure<Setup>(domain::ErrorCategory::NotFound);
    }
    QSqlQuery exists(m_database);
    exists.prepare(QStringLiteral("SELECT COUNT(*) FROM desktop_apps WHERE id IN (%1)").arg(placeholderList(setup.applicationIds.size())));
    for (qint64 id : setup.applicationIds) exists.addBindValue(id);
    if (!exists.exec() || !exists.next()) return failure<Setup>(queryFailure(exists));
    if (exists.value(0).toInt() != setup.applicationIds.size()) return failure<Setup>(domain::ErrorCategory::Validation);
    exists.finish();
    QSqlQuery clearApps(m_database);
    clearApps.prepare(QStringLiteral("DELETE FROM setup_apps WHERE setup_id=?"));
    clearApps.addBindValue(setup.id);
    if (!clearApps.exec()) return failure<Setup>(queryFailure(clearApps));
    for (int position = 0; position < setup.applicationIds.size(); ++position) {
        QSqlQuery relation(m_database);
        relation.prepare(QStringLiteral("INSERT INTO setup_apps(setup_id,app_id,position) VALUES(?,?,?)"));
        relation.addBindValue(setup.id);
        relation.addBindValue(setup.applicationIds[position]);
        relation.addBindValue(position);
        if (!relation.exec()) return failure<Setup>(queryFailure(relation));
    }
    if (removeIcon) {
        QSqlQuery remove(m_database);
        remove.prepare(QStringLiteral("DELETE FROM setup_icons WHERE setup_id=?"));
        remove.addBindValue(setup.id);
        if (!remove.exec()) return failure<Setup>(queryFailure(remove));
    } else if (setup.icon) {
        QSqlQuery icon(m_database);
        icon.prepare(QStringLiteral("INSERT INTO setup_icons(setup_id,filename,mime_type,byte_size) VALUES(?,?,?,?) ON CONFLICT(setup_id) DO UPDATE SET filename=excluded.filename,mime_type=excluded.mime_type,byte_size=excluded.byte_size,updated_at=CURRENT_TIMESTAMP"));
        icon.addBindValue(setup.id);
        bindAsset(icon, 1, *setup.icon);
        if (!icon.exec()) return failure<Setup>(queryFailure(icon));
    }
    if (!transaction.commit()) return failure<Setup>(domain::ErrorCategory::Storage);
    return load(setup.id);
}

RepositoryResult<void> SetupsRepository::setIcon(qint64 setupId, const AssetMetadata &asset)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (setupId <= 0 || !validAsset(asset, {QStringLiteral("image/jpeg"), QStringLiteral("image/png"),
                                             QStringLiteral("image/webp"), QStringLiteral("image/x-icon")}))
        return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO setup_icons(setup_id,filename,mime_type,byte_size) VALUES(?,?,?,?) ON CONFLICT(setup_id) DO UPDATE SET filename=excluded.filename,mime_type=excluded.mime_type,byte_size=excluded.byte_size,updated_at=CURRENT_TIMESTAMP"));
    query.addBindValue(setupId);
    bindAsset(query, 1, asset);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<void> SetupsRepository::removeIcon(qint64 setupId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM setup_icons WHERE setup_id=?"));
    query.addBindValue(setupId);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<void> SetupsRepository::remove(qint64 id)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM setups WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return query.numRowsAffected() == 1 ? RepositoryResult<void>{}
                                        : failureVoid(domain::ErrorCategory::NotFound);
}

WallpapersRepository::WallpapersRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<QVector<Wallpaper>> WallpapersRepository::list() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id,display_name,filename,mime_type,byte_size,created_at FROM wallpapers ORDER BY created_at,id")))
        return failure<QVector<Wallpaper>>(queryFailure(query));
    RepositoryResult<QVector<Wallpaper>> result;
    while (query.next()) {
        result.value.push_back({query.value(0).toString(), query.value(1).toString(),
            {query.value(2).toString(), query.value(3).toString(), query.value(4).toLongLong()}, query.value(5).toString()});
    }
    return result;
}

RepositoryResult<void> WallpapersRepository::add(const Wallpaper &wallpaper)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (wallpaper.id.trimmed().isEmpty() || wallpaper.displayName.trimmed().isEmpty()
        || !validAsset(wallpaper.asset, {QStringLiteral("image/jpeg"), QStringLiteral("image/png"), QStringLiteral("image/webp")}))
        return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO wallpapers(id,display_name,filename,mime_type,byte_size) VALUES(?,?,?,?,?)"));
    query.addBindValue(wallpaper.id.trimmed());
    query.addBindValue(wallpaper.displayName.trimmed());
    bindAsset(query, 2, wallpaper.asset);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<void> WallpapersRepository::addAndSelect(const Wallpaper &wallpaper,
                                                           QString selection,
                                                           const std::function<bool()> &cancellationRequested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    selection = selection.trimmed();
    if (wallpaper.id.trimmed().isEmpty() || wallpaper.displayName.trimmed().isEmpty()
        || !validAsset(wallpaper.asset, {QStringLiteral("image/jpeg"),
                                        QStringLiteral("image/png"),
                                        QStringLiteral("image/webp")})
        || selection != QStringLiteral("custom:") + wallpaper.id) {
        return failureVoid(domain::ErrorCategory::Validation);
    }
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO wallpapers(id,display_name,filename,mime_type,byte_size) VALUES(?,?,?,?,?)"));
    insert.addBindValue(wallpaper.id.trimmed());
    insert.addBindValue(wallpaper.displayName.trimmed());
    bindAsset(insert, 2, wallpaper.asset);
    if (!insert.exec()) return failureVoid(queryFailure(insert));
    QSqlQuery preference(m_database);
    preference.prepare(QStringLiteral(
        "INSERT INTO preferences(key,value) VALUES('appearance.selected_wallpaper',?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    preference.addBindValue(selection);
    if (!preference.exec()) return failureVoid(queryFailure(preference));
    if (cancellationRequested && cancellationRequested())
        return failureVoid(domain::ErrorCategory::Cancelled);
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<void> WallpapersRepository::remove(
    QString id, const std::function<bool()> &cancellationRequested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM wallpapers WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec()) return failureVoid(queryFailure(query));
    if (query.numRowsAffected() != 1) return failureVoid(domain::ErrorCategory::NotFound);
    QSqlQuery selection(m_database);
    selection.prepare(QStringLiteral(
        "UPDATE preferences SET value='builtin:hu-tao' "
        "WHERE key='appearance.selected_wallpaper' AND value=?"));
    selection.addBindValue(QStringLiteral("custom:") + id);
    if (!selection.exec()) return failureVoid(queryFailure(selection));
    if (cancellationRequested && cancellationRequested())
        return failureVoid(domain::ErrorCategory::Cancelled);
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

SteamGameCatalogRepository::SteamGameCatalogRepository(QSqlDatabase &database)
    : m_database(database)
{
}

RepositoryResult<domain::SteamGameCatalogEntry> SteamGameCatalogRepository::load(
    quint32 appId) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT steam_app_id,store_url,name,artwork_source_url,artwork_filename,"
        "artwork_mime_type,artwork_byte_size,created_at,updated_at "
        "FROM steam_game_catalog WHERE steam_app_id=?"));
    query.addBindValue(appId);
    if (!query.exec())
        return failure<domain::SteamGameCatalogEntry>(queryFailure(query));
    if (!query.next())
        return failure<domain::SteamGameCatalogEntry>(domain::ErrorCategory::NotFound);
    domain::SteamGameCatalogEntry entry;
    entry.appId = query.value(0).toUInt();
    entry.canonicalStoreUrl = query.value(1).toString();
    entry.name = query.value(2).toString();
    entry.artwork.sourceUrl = query.value(3).toString();
    entry.artwork.cachedFilename = query.value(4).toString();
    entry.artwork.mimeType = query.value(5).toString();
    entry.artwork.byteSize = query.value(6).toLongLong();
    entry.createdAt = query.value(7).toString();
    entry.updatedAt = query.value(8).toString();
    if (!domain::isValidSteamGameCatalogEntry(entry))
        return failure<domain::SteamGameCatalogEntry>(domain::ErrorCategory::Storage);
    RepositoryResult<domain::SteamGameCatalogEntry> result;
    result.value = std::move(entry);
    return result;
}

RepositoryResult<QVector<domain::SteamGameCatalogEntry>>
SteamGameCatalogRepository::list() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT steam_app_id,store_url,name,artwork_source_url,artwork_filename,"
            "artwork_mime_type,artwork_byte_size,created_at,updated_at "
            "FROM steam_game_catalog ORDER BY name COLLATE NOCASE,steam_app_id"))) {
        return failure<QVector<domain::SteamGameCatalogEntry>>(queryFailure(query));
    }
    QVector<domain::SteamGameCatalogEntry> entries;
    while (query.next()) {
        domain::SteamGameCatalogEntry entry;
        entry.appId = query.value(0).toUInt();
        entry.canonicalStoreUrl = query.value(1).toString();
        entry.name = query.value(2).toString();
        entry.artwork.sourceUrl = query.value(3).toString();
        entry.artwork.cachedFilename = query.value(4).toString();
        entry.artwork.mimeType = query.value(5).toString();
        entry.artwork.byteSize = query.value(6).toLongLong();
        entry.createdAt = query.value(7).toString();
        entry.updatedAt = query.value(8).toString();
        if (!domain::isValidSteamGameCatalogEntry(entry))
            return failure<QVector<domain::SteamGameCatalogEntry>>(
                domain::ErrorCategory::Storage);
        entries.push_back(std::move(entry));
    }
    RepositoryResult<QVector<domain::SteamGameCatalogEntry>> result;
    result.value = std::move(entries);
    return result;
}

RepositoryResult<domain::SteamGameCatalogEntry> SteamGameCatalogRepository::get(
    quint32 appId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0)
        return failure<domain::SteamGameCatalogEntry>(domain::ErrorCategory::Validation);
    return load(appId);
}

RepositoryResult<domain::SteamGameCatalogEntry> SteamGameCatalogRepository::upsert(
    domain::SteamGameCatalogDetails details)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    const quint32 appId = details.appId;
    if (domain::normalizeSteamGameCatalogDetails(details).isError())
        return failure<domain::SteamGameCatalogEntry>(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active())
        return failure<domain::SteamGameCatalogEntry>(domain::ErrorCategory::Storage);
    const auto saved = upsertSteamCatalogDetails(m_database, std::move(details));
    if (!saved.succeeded())
        return failure<domain::SteamGameCatalogEntry>(saved.error.category());
    const auto entry = load(appId);
    if (!entry.succeeded()) return entry;
    if (!transaction.commit())
        return failure<domain::SteamGameCatalogEntry>(domain::ErrorCategory::Storage);
    return entry;
}

RepositoryResult<void> SteamGameCatalogRepository::setArtwork(
    quint32 appId, const QString &expectedSourceUrl,
    const domain::SteamGameCatalogArtwork &artwork)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    return setSteamCatalogArtwork(m_database, appId, expectedSourceUrl, artwork);
}

RepositoryResult<bool> SteamGameCatalogRepository::hasArtworkFilename(
    const QString &filename) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (filename.isEmpty()) return failure<bool>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM steam_game_catalog WHERE artwork_filename=? LIMIT 1"));
    query.addBindValue(filename);
    if (!query.exec()) return failure<bool>(queryFailure(query));
    RepositoryResult<bool> result;
    result.value = query.next();
    return result;
}

RepositoryResult<void> SteamGameCatalogRepository::remove(quint32 appId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0) return failureVoid(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    QSqlQuery references(m_database);
    references.prepare(QStringLiteral(
        "SELECT (SELECT COUNT(*) FROM steam_collection_games WHERE steam_app_id=?)"
        "+(SELECT COUNT(*) FROM steam_games WHERE steam_app_id=?)"
        "+(SELECT COUNT(*) FROM steam_release_follows WHERE steam_app_id=?)"
        "+(SELECT COUNT(*) FROM steam_game_update_follows WHERE steam_app_id=?)"));
    for (int i = 0; i < 4; ++i) references.addBindValue(appId);
    if (!references.exec()) return failureVoid(queryFailure(references));
    if (!references.next()) return failureVoid(domain::ErrorCategory::Storage);
    if (references.value(0).toLongLong() != 0)
        return failureVoid(domain::ErrorCategory::Conflict);
    references.finish();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM steam_game_catalog WHERE steam_app_id=?"));
    query.addBindValue(appId);
    if (!query.exec()) return failureVoid(queryFailure(query));
    if (query.numRowsAffected() != 1) return failureVoid(domain::ErrorCategory::NotFound);
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

SteamCollectionsRepository::SteamCollectionsRepository(QSqlDatabase &database)
    : m_database(database)
{
}

RepositoryResult<domain::SteamCollection> SteamCollectionsRepository::load(
    qint64 collectionId) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id,name,description,color,icon,display_order,created_at,updated_at "
        "FROM steam_collections WHERE id=?"));
    query.addBindValue(collectionId);
    if (!query.exec()) return failure<domain::SteamCollection>(queryFailure(query));
    if (!query.next()) return failure<domain::SteamCollection>(domain::ErrorCategory::NotFound);
    domain::SteamCollection collection;
    collection.id = query.value(0).toLongLong();
    collection.name = query.value(1).toString();
    collection.description = query.value(2).toString();
    collection.color = query.value(3).toString();
    collection.icon = query.value(4).toString();
    collection.displayOrder = query.value(5).toInt();
    collection.createdAt = query.value(6).toString();
    collection.updatedAt = query.value(7).toString();
    if (!domain::isValidSteamCollection(collection))
        return failure<domain::SteamCollection>(domain::ErrorCategory::Storage);
    RepositoryResult<domain::SteamCollection> result;
    result.value = std::move(collection);
    return result;
}

RepositoryResult<QVector<domain::SteamCollection>> SteamCollectionsRepository::list() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT id,name,description,color,icon,display_order,created_at,updated_at "
            "FROM steam_collections ORDER BY display_order,id"))) {
        return failure<QVector<domain::SteamCollection>>(queryFailure(query));
    }
    QVector<domain::SteamCollection> collections;
    while (query.next()) {
        domain::SteamCollection collection;
        collection.id = query.value(0).toLongLong();
        collection.name = query.value(1).toString();
        collection.description = query.value(2).toString();
        collection.color = query.value(3).toString();
        collection.icon = query.value(4).toString();
        collection.displayOrder = query.value(5).toInt();
        collection.createdAt = query.value(6).toString();
        collection.updatedAt = query.value(7).toString();
        if (!domain::isValidSteamCollection(collection))
            return failure<QVector<domain::SteamCollection>>(domain::ErrorCategory::Storage);
        collections.push_back(std::move(collection));
    }
    RepositoryResult<QVector<domain::SteamCollection>> result;
    result.value = std::move(collections);
    return result;
}

RepositoryResult<domain::SteamCollection> SteamCollectionsRepository::get(
    qint64 collectionId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (collectionId <= 0) return failure<domain::SteamCollection>(domain::ErrorCategory::Validation);
    return load(collectionId);
}

RepositoryResult<domain::SteamCollection> SteamCollectionsRepository::create(
    domain::SteamCollectionInput input)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (domain::normalizeSteamCollectionInput(input).isError())
        return failure<domain::SteamCollection>(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<domain::SteamCollection>(domain::ErrorCategory::Storage);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO steam_collections(name,name_normalized,description,color,icon,display_order) "
        "VALUES(?,?,?,?,?,(SELECT COALESCE(MAX(display_order),-1)+1 FROM steam_collections))"));
    query.addBindValue(input.name);
    query.addBindValue(domain::steamCollectionNameKey(input.name));
    query.addBindValue(input.description);
    query.addBindValue(input.color);
    query.addBindValue(input.icon);
    if (!query.exec()) return failure<domain::SteamCollection>(queryFailure(query));
    const qint64 id = query.lastInsertId().toLongLong();
    query.finish();
    const auto collection = load(id);
    if (!collection.succeeded()) return collection;
    if (!transaction.commit()) return failure<domain::SteamCollection>(domain::ErrorCategory::Storage);
    return collection;
}

RepositoryResult<domain::SteamCollection> SteamCollectionsRepository::update(
    qint64 collectionId, domain::SteamCollectionInput input)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (collectionId <= 0 || domain::normalizeSteamCollectionInput(input).isError())
        return failure<domain::SteamCollection>(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<domain::SteamCollection>(domain::ErrorCategory::Storage);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE steam_collections SET name=?,name_normalized=?,description=?,color=?,icon=?,"
        "updated_at=CURRENT_TIMESTAMP WHERE id=?"));
    query.addBindValue(input.name);
    query.addBindValue(domain::steamCollectionNameKey(input.name));
    query.addBindValue(input.description);
    query.addBindValue(input.color);
    query.addBindValue(input.icon);
    query.addBindValue(collectionId);
    if (!query.exec()) return failure<domain::SteamCollection>(queryFailure(query));
    if (query.numRowsAffected() != 1)
        return failure<domain::SteamCollection>(domain::ErrorCategory::NotFound);
    query.finish();
    const auto collection = load(collectionId);
    if (!collection.succeeded()) return collection;
    if (!transaction.commit()) return failure<domain::SteamCollection>(domain::ErrorCategory::Storage);
    return collection;
}

RepositoryResult<void> SteamCollectionsRepository::reorder(
    const QVector<qint64> &orderedIds)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id,display_order FROM steam_collections")))
        return failureVoid(queryFailure(query));
    QSet<qint64> existing;
    qint64 largestOrder = -1;
    while (query.next()) {
        existing.insert(query.value(0).toLongLong());
        largestOrder = std::max(largestOrder, query.value(1).toLongLong());
    }
    query.finish();
    QSet<qint64> requested;
    for (const qint64 id : orderedIds) {
        if (id <= 0 || requested.contains(id))
            return failureVoid(domain::ErrorCategory::Validation);
        requested.insert(id);
    }
    if (requested != existing) return failureVoid(domain::ErrorCategory::Validation);
    if (orderedIds.isEmpty()) {
        if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
        return {};
    }
    const qint64 offset = largestOrder + orderedIds.size() + 1;
    QSqlQuery shift(m_database);
    shift.prepare(QStringLiteral("UPDATE steam_collections SET display_order=display_order+?"));
    shift.addBindValue(offset);
    if (!shift.exec()) return failureVoid(queryFailure(shift));
    shift.finish();
    QSqlQuery assign(m_database);
    assign.prepare(QStringLiteral(
        "UPDATE steam_collections SET display_order=?,updated_at=CURRENT_TIMESTAMP WHERE id=?"));
    for (int order = 0; order < orderedIds.size(); ++order) {
        assign.bindValue(0, order);
        assign.bindValue(1, orderedIds[order]);
        if (!assign.exec() || assign.numRowsAffected() != 1)
            return failureVoid(assign.lastError().isValid()
                ? queryFailure(assign) : domain::ErrorCategory::Storage);
        assign.finish();
    }
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<SteamCollectionDeleteResult> SteamCollectionsRepository::remove(
    qint64 collectionId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (collectionId <= 0) return failure<SteamCollectionDeleteResult>(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<SteamCollectionDeleteResult>(domain::ErrorCategory::Storage);
    QSqlQuery count(m_database);
    count.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM steam_collection_games WHERE collection_id=?"));
    count.addBindValue(collectionId);
    if (!count.exec() || !count.next())
        return failure<SteamCollectionDeleteResult>(queryFailure(count));
    const int membershipCount = count.value(0).toInt();
    count.finish();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM steam_collections WHERE id=?"));
    query.addBindValue(collectionId);
    if (!query.exec()) return failure<SteamCollectionDeleteResult>(queryFailure(query));
    if (query.numRowsAffected() != 1)
        return failure<SteamCollectionDeleteResult>(domain::ErrorCategory::NotFound);
    if (!transaction.commit())
        return failure<SteamCollectionDeleteResult>(domain::ErrorCategory::Storage);
    RepositoryResult<SteamCollectionDeleteResult> result;
    result.value = {collectionId, membershipCount};
    return result;
}

RepositoryResult<QVector<domain::SteamCollectionGameEntry>>
SteamCollectionsRepository::games(qint64 collectionId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (collectionId <= 0)
        return failure<QVector<domain::SteamCollectionGameEntry>>(
            domain::ErrorCategory::Validation);
    const auto collection = load(collectionId);
    if (!collection.succeeded())
        return failure<QVector<domain::SteamCollectionGameEntry>>(
            collection.error.category());
    QSqlQuery query(m_database);
    query.prepare(steamCollectionGameSelect()
        + QStringLiteral(" WHERE cg.collection_id=? ORDER BY c.name COLLATE NOCASE,c.steam_app_id"));
    query.addBindValue(collectionId);
    if (!query.exec())
        return failure<QVector<domain::SteamCollectionGameEntry>>(queryFailure(query));
    QVector<domain::SteamCollectionGameEntry> entries;
    while (query.next()) {
        domain::SteamCollectionGameEntry entry;
        if (!decodeSteamCollectionGame(query, &entry))
            return failure<QVector<domain::SteamCollectionGameEntry>>(
                domain::ErrorCategory::Storage);
        entries.push_back(std::move(entry));
    }
    RepositoryResult<QVector<domain::SteamCollectionGameEntry>> result;
    result.value = std::move(entries);
    return result;
}

RepositoryResult<QVector<domain::SteamCollectionGameEntry>>
SteamCollectionsRepository::allGames() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(steamCollectionGameSelect()
            + QStringLiteral(" ORDER BY c.name COLLATE NOCASE,c.steam_app_id,cg.collection_id"))) {
        return failure<QVector<domain::SteamCollectionGameEntry>>(queryFailure(query));
    }
    QVector<domain::SteamCollectionGameEntry> entries;
    while (query.next()) {
        domain::SteamCollectionGameEntry entry;
        if (!decodeSteamCollectionGame(query, &entry))
            return failure<QVector<domain::SteamCollectionGameEntry>>(
                domain::ErrorCategory::Storage);
        entries.push_back(std::move(entry));
    }
    RepositoryResult<QVector<domain::SteamCollectionGameEntry>> result;
    result.value = std::move(entries);
    return result;
}

RepositoryResult<domain::SteamCollectionGameEntry> SteamCollectionsRepository::loadGame(
    qint64 collectionId, quint32 appId) const
{
    QSqlQuery query(m_database);
    query.prepare(steamCollectionGameSelect()
        + QStringLiteral(" WHERE cg.collection_id=? AND cg.steam_app_id=?"));
    query.addBindValue(collectionId);
    query.addBindValue(appId);
    if (!query.exec())
        return failure<domain::SteamCollectionGameEntry>(queryFailure(query));
    if (!query.next())
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::NotFound);
    domain::SteamCollectionGameEntry entry;
    if (!decodeSteamCollectionGame(query, &entry))
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Storage);
    RepositoryResult<domain::SteamCollectionGameEntry> result;
    result.value = std::move(entry);
    return result;
}

RepositoryResult<domain::SteamCollectionGameEntry> SteamCollectionsRepository::addGame(
    qint64 collectionId, domain::SteamGameCatalogDetails catalogDetails,
    domain::SteamCollectionGameInput input)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (collectionId <= 0
        || domain::normalizeSteamGameCatalogDetails(catalogDetails).isError()
        || domain::normalizeSteamCollectionGameInput(input).isError()
        || catalogDetails.appId != input.steamAppId) {
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Validation);
    }
    Transaction transaction(m_database);
    if (!transaction.active())
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Storage);
    const auto collection = load(collectionId);
    if (!collection.succeeded())
        return failure<domain::SteamCollectionGameEntry>(collection.error.category());
    const auto catalog = upsertSteamCatalogDetails(m_database, std::move(catalogDetails));
    if (!catalog.succeeded())
        return failure<domain::SteamCollectionGameEntry>(catalog.error.category());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO steam_collection_games(collection_id,steam_app_id,status,personal_rating,"
        "personal_comment) VALUES(?,?,?,?,?)"));
    query.addBindValue(collectionId);
    query.addBindValue(input.steamAppId);
    query.addBindValue(input.status
        ? QVariant(domain::steamCollectionGameStatusName(*input.status)) : QVariant());
    query.addBindValue(input.personalRating ? QVariant(*input.personalRating) : QVariant());
    query.addBindValue(input.personalComment);
    if (!query.exec())
        return failure<domain::SteamCollectionGameEntry>(queryFailure(query));
    query.finish();
    const auto entry = loadGame(collectionId, input.steamAppId);
    if (!entry.succeeded()) return entry;
    if (!transaction.commit())
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Storage);
    return entry;
}

RepositoryResult<domain::SteamCollectionGameEntry> SteamCollectionsRepository::updateGame(
    qint64 collectionId, domain::SteamCollectionGameInput input)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (collectionId <= 0 || domain::normalizeSteamCollectionGameInput(input).isError())
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE steam_collection_games SET status=?,personal_rating=?,personal_comment=?,"
        "updated_at=CURRENT_TIMESTAMP WHERE collection_id=? AND steam_app_id=?"));
    query.addBindValue(input.status
        ? QVariant(domain::steamCollectionGameStatusName(*input.status)) : QVariant());
    query.addBindValue(input.personalRating ? QVariant(*input.personalRating) : QVariant());
    query.addBindValue(input.personalComment);
    query.addBindValue(collectionId);
    query.addBindValue(input.steamAppId);
    if (!query.exec())
        return failure<domain::SteamCollectionGameEntry>(queryFailure(query));
    if (query.numRowsAffected() != 1)
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::NotFound);
    query.finish();
    return loadGame(collectionId, input.steamAppId);
}

RepositoryResult<domain::SteamCollectionGameEntry> SteamCollectionsRepository::moveGame(
    qint64 sourceCollectionId, qint64 targetCollectionId, quint32 appId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (sourceCollectionId <= 0 || targetCollectionId <= 0
        || sourceCollectionId == targetCollectionId || appId == 0) {
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Validation);
    }
    Transaction transaction(m_database);
    if (!transaction.active())
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Storage);
    const auto sourceCollection = load(sourceCollectionId);
    if (!sourceCollection.succeeded())
        return failure<domain::SteamCollectionGameEntry>(sourceCollection.error.category());
    const auto targetCollection = load(targetCollectionId);
    if (!targetCollection.succeeded())
        return failure<domain::SteamCollectionGameEntry>(targetCollection.error.category());
    const auto source = loadGame(sourceCollectionId, appId);
    if (!source.succeeded())
        return failure<domain::SteamCollectionGameEntry>(source.error.category());
    const auto target = loadGame(targetCollectionId, appId);
    if (target.succeeded())
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Conflict);
    if (target.error.category() != domain::ErrorCategory::NotFound)
        return failure<domain::SteamCollectionGameEntry>(target.error.category());

    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO steam_collection_games(collection_id,steam_app_id,status,personal_rating,"
        "personal_comment) VALUES(?,?,?,?,?)"));
    insert.addBindValue(targetCollectionId);
    insert.addBindValue(appId);
    insert.addBindValue(source.value.status
        ? QVariant(domain::steamCollectionGameStatusName(*source.value.status)) : QVariant());
    insert.addBindValue(source.value.personalRating
        ? QVariant(*source.value.personalRating) : QVariant());
    insert.addBindValue(source.value.personalComment);
    if (!insert.exec())
        return failure<domain::SteamCollectionGameEntry>(queryFailure(insert));
    insert.finish();

    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral(
        "DELETE FROM steam_collection_games WHERE collection_id=? AND steam_app_id=?"));
    remove.addBindValue(sourceCollectionId);
    remove.addBindValue(appId);
    if (!remove.exec())
        return failure<domain::SteamCollectionGameEntry>(queryFailure(remove));
    if (remove.numRowsAffected() != 1)
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Conflict);
    remove.finish();

    const auto moved = loadGame(targetCollectionId, appId);
    if (!moved.succeeded()) return moved;
    if (!transaction.commit())
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Storage);
    return moved;
}

RepositoryResult<domain::SteamCollectionGameEntry> SteamCollectionsRepository::copyGame(
    qint64 sourceCollectionId, qint64 targetCollectionId, quint32 appId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (sourceCollectionId <= 0 || targetCollectionId <= 0
        || sourceCollectionId == targetCollectionId || appId == 0) {
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Validation);
    }
    Transaction transaction(m_database);
    if (!transaction.active())
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Storage);
    const auto sourceCollection = load(sourceCollectionId);
    if (!sourceCollection.succeeded())
        return failure<domain::SteamCollectionGameEntry>(sourceCollection.error.category());
    const auto targetCollection = load(targetCollectionId);
    if (!targetCollection.succeeded())
        return failure<domain::SteamCollectionGameEntry>(targetCollection.error.category());
    const auto source = loadGame(sourceCollectionId, appId);
    if (!source.succeeded())
        return failure<domain::SteamCollectionGameEntry>(source.error.category());
    const auto target = loadGame(targetCollectionId, appId);
    if (target.succeeded())
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Conflict);
    if (target.error.category() != domain::ErrorCategory::NotFound)
        return failure<domain::SteamCollectionGameEntry>(target.error.category());

    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO steam_collection_games(collection_id,steam_app_id,status,personal_rating,"
        "personal_comment) VALUES(?,?,?,?,?)"));
    insert.addBindValue(targetCollectionId);
    insert.addBindValue(appId);
    insert.addBindValue(source.value.status
        ? QVariant(domain::steamCollectionGameStatusName(*source.value.status)) : QVariant());
    insert.addBindValue(source.value.personalRating
        ? QVariant(*source.value.personalRating) : QVariant());
    insert.addBindValue(source.value.personalComment);
    if (!insert.exec())
        return failure<domain::SteamCollectionGameEntry>(queryFailure(insert));
    insert.finish();

    const auto copied = loadGame(targetCollectionId, appId);
    if (!copied.succeeded()) return copied;
    if (!transaction.commit())
        return failure<domain::SteamCollectionGameEntry>(domain::ErrorCategory::Storage);
    return copied;
}

RepositoryResult<void> SteamCollectionsRepository::removeGame(
    qint64 collectionId, quint32 appId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (collectionId <= 0 || appId == 0)
        return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "DELETE FROM steam_collection_games WHERE collection_id=? AND steam_app_id=?"));
    query.addBindValue(collectionId);
    query.addBindValue(appId);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return query.numRowsAffected() == 1 ? RepositoryResult<void>{}
        : failureVoid(domain::ErrorCategory::NotFound);
}

SteamRepository::SteamRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<SteamSettings> SteamRepository::settings() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT country_code FROM steam_game_settings WHERE id=1")) || !query.next())
        return failure<SteamSettings>(queryFailure(query));
    RepositoryResult<SteamSettings> result;
    result.value.countryCode = query.value(0).toString();
    return result;
}

RepositoryResult<void> SteamRepository::setCountry(QString countryCode)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    countryCode = countryCode.trimmed().toUpper();
    static const QRegularExpression code(QStringLiteral("^[A-Z]{2}$"));
    if (!code.match(countryCode).hasMatch()) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE steam_game_settings SET country_code=?,updated_at=CURRENT_TIMESTAMP WHERE id=1"));
    query.addBindValue(countryCode);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return query.numRowsAffected() == 1 ? RepositoryResult<void>{}
                                        : failureVoid(domain::ErrorCategory::Storage);
}

RepositoryResult<QVector<SteamGame>> SteamRepository::list() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id FROM steam_games ORDER BY name COLLATE NOCASE,id")))
        return failure<QVector<SteamGame>>(queryFailure(query));
    QVector<qint64> ids;
    while (query.next()) ids.push_back(query.value(0).toLongLong());
    query.finish();
    RepositoryResult<QVector<SteamGame>> result;
    for (qint64 id : ids) {
        auto item = load(id);
        if (!item.succeeded()) return failure<QVector<SteamGame>>(item.error.category());
        result.value.push_back(item.value);
    }
    return result;
}

RepositoryResult<SteamGame> SteamRepository::get(qint64 id) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    return load(id);
}

RepositoryResult<QVector<SteamPriceHistoryMarket>> SteamRepository::priceHistoryMarkets(
    qint64 gameId, const std::function<bool()> &cancellationRequested) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (cancellationRequested && cancellationRequested())
        return failure<QVector<SteamPriceHistoryMarket>>(domain::ErrorCategory::Cancelled);
    if (gameId <= 0)
        return failure<QVector<SteamPriceHistoryMarket>>(domain::ErrorCategory::Validation);

    QSqlQuery query(m_database);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
        "SELECT country_code,currency_code,COUNT(*),MIN(observed_at_utc),"
        "MAX(observed_at_utc) FROM steam_price_observations WHERE steam_game_id=? "
        "GROUP BY country_code,currency_code "
        "ORDER BY MAX(observed_at_utc) DESC,country_code ASC,currency_code ASC"));
    query.addBindValue(gameId);
    if (!query.exec())
        return failure<QVector<SteamPriceHistoryMarket>>(queryFailure(query));

    RepositoryResult<QVector<SteamPriceHistoryMarket>> result;
    while (query.next()) {
        if (cancellationRequested && cancellationRequested())
            return failure<QVector<SteamPriceHistoryMarket>>(domain::ErrorCategory::Cancelled);
        SteamPriceHistoryMarket market;
        market.countryCode = query.value(0).toString();
        market.currencyCode = query.value(1).toString();
        market.observationCount = query.value(2).toLongLong();
        market.firstObservationAtUtc = query.value(3).toString();
        market.mostRecentObservationAtUtc = query.value(4).toString();
        result.value.push_back(std::move(market));
    }
    if (query.lastError().isValid())
        return failure<QVector<SteamPriceHistoryMarket>>(queryFailure(query));
    return result;
}

RepositoryResult<SteamPriceHistorySnapshot> SteamRepository::priceHistory(
    qint64 gameId, QString countryCode, QString currencyCode,
    SteamPriceHistoryRange range,
    const std::function<bool()> &cancellationRequested) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (cancellationRequested && cancellationRequested())
        return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::Cancelled);
    countryCode = countryCode.trimmed().toUpper();
    currencyCode = currencyCode.trimmed().toUpper();
    static const QRegularExpression countryPattern(QStringLiteral("^[A-Z]{2}$"));
    static const QRegularExpression currencyPattern(QStringLiteral("^[A-Z]{3}$"));
    if (gameId <= 0 || !countryPattern.match(countryCode).hasMatch()
        || (!currencyCode.isEmpty() && !currencyPattern.match(currencyCode).hasMatch())) {
        return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::Validation);
    }

    QString intervalStartUtc;
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    switch (range) {
    case SteamPriceHistoryRange::Days7:
        intervalStartUtc = nowUtc.addDays(-7).toString(Qt::ISODateWithMs);
        break;
    case SteamPriceHistoryRange::Days30:
        intervalStartUtc = nowUtc.addDays(-30).toString(Qt::ISODateWithMs);
        break;
    case SteamPriceHistoryRange::Days90:
        intervalStartUtc = nowUtc.addDays(-90).toString(Qt::ISODateWithMs);
        break;
    case SteamPriceHistoryRange::Year1:
        intervalStartUtc = nowUtc.addYears(-1).toString(Qt::ISODateWithMs);
        break;
    case SteamPriceHistoryRange::All:
        break;
    default:
        return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::Validation);
    }

    QSqlQuery exists(m_database);
    exists.setForwardOnly(true);
    exists.prepare(QStringLiteral("SELECT 1 FROM steam_games WHERE id=?"));
    exists.addBindValue(gameId);
    if (!exists.exec())
        return failure<SteamPriceHistorySnapshot>(queryFailure(exists));
    if (!exists.next())
        return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::NotFound);
    exists.finish();
    if (cancellationRequested && cancellationRequested())
        return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::Cancelled);

    SteamPriceHistorySnapshot snapshot;
    SteamPriceHistoryStatistics &statistics = snapshot.statistics;
    statistics.countryCode = countryCode;
    statistics.currencyCode = currencyCode;

    QSqlQuery aggregate(m_database);
    aggregate.setForwardOnly(true);
    aggregate.prepare(QStringLiteral(
        "SELECT COUNT(*),COALESCE(SUM(CASE WHEN current_price_minor IS NOT NULL "
        "THEN 1 ELSE 0 END),0),MIN(current_price_minor),MAX(current_price_minor),"
        "MIN(observed_at_utc),MAX(observed_at_utc) FROM steam_price_observations "
        "WHERE steam_game_id=? AND country_code=? AND currency_code IS ?"));
    aggregate.addBindValue(gameId);
    aggregate.addBindValue(countryCode);
    aggregate.addBindValue(nullableString(currencyCode));
    if (!aggregate.exec())
        return failure<SteamPriceHistorySnapshot>(queryFailure(aggregate));
    if (!aggregate.next())
        return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::Storage);
    statistics.observationCount = aggregate.value(0).toLongLong();
    statistics.comparablePriceCount = aggregate.value(1).toLongLong();
    if (!aggregate.value(2).isNull())
        statistics.lowestPriceMinor = aggregate.value(2).toLongLong();
    if (!aggregate.value(3).isNull())
        statistics.highestPriceMinor = aggregate.value(3).toLongLong();
    statistics.firstObservationAtUtc = aggregate.value(4).toString();
    statistics.mostRecentObservationAtUtc = aggregate.value(5).toString();
    statistics.hasSufficientHistory = statistics.comparablePriceCount >= 2;
    aggregate.finish();
    if (cancellationRequested && cancellationRequested())
        return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::Cancelled);

    QSqlQuery latestObservation(m_database);
    latestObservation.setForwardOnly(true);
    latestObservation.prepare(QStringLiteral(
        "SELECT price_state,observed_at_utc FROM steam_price_observations "
        "WHERE steam_game_id=? AND country_code=? AND currency_code IS ? "
        "ORDER BY observed_at_utc DESC,id DESC LIMIT 1"));
    latestObservation.addBindValue(gameId);
    latestObservation.addBindValue(countryCode);
    latestObservation.addBindValue(nullableString(currencyCode));
    if (!latestObservation.exec())
        return failure<SteamPriceHistorySnapshot>(queryFailure(latestObservation));
    if (latestObservation.next()) {
        statistics.latestObservationState = latestObservation.value(0).toString();
        statistics.latestObservationAtUtc = latestObservation.value(1).toString();
    }
    latestObservation.finish();
    if (cancellationRequested && cancellationRequested())
        return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::Cancelled);

    QSqlQuery latestPrice(m_database);
    latestPrice.setForwardOnly(true);
    latestPrice.prepare(QStringLiteral(
        "SELECT current_price_minor,price_state,observed_at_utc "
        "FROM steam_price_observations WHERE steam_game_id=? AND country_code=? "
        "AND currency_code IS ? AND current_price_minor IS NOT NULL "
        "ORDER BY observed_at_utc DESC,id DESC LIMIT 1"));
    latestPrice.addBindValue(gameId);
    latestPrice.addBindValue(countryCode);
    latestPrice.addBindValue(nullableString(currencyCode));
    if (!latestPrice.exec())
        return failure<SteamPriceHistorySnapshot>(queryFailure(latestPrice));
    if (latestPrice.next()) {
        statistics.latestPriceMinor = latestPrice.value(0).toLongLong();
        statistics.latestPriceState = latestPrice.value(1).toString();
        statistics.latestPriceAtUtc = latestPrice.value(2).toString();
    }
    latestPrice.finish();
    if (cancellationRequested && cancellationRequested())
        return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::Cancelled);

    struct PricePoint final {
        qint64 id{0};
        qint64 priceMinor{0};
        QString observedAtUtc;
    };
    const auto loadIntervalPoint = [&](bool first,
                                       std::optional<PricePoint> *point,
                                       domain::ErrorCategory *errorCategory) {
        QString sql = QStringLiteral(
            "SELECT id,current_price_minor,observed_at_utc FROM steam_price_observations "
            "WHERE steam_game_id=? AND country_code=? AND currency_code IS ? "
            "AND current_price_minor IS NOT NULL");
        if (!intervalStartUtc.isEmpty())
            sql += QStringLiteral(" AND observed_at_utc>=?");
        sql += first ? QStringLiteral(" ORDER BY observed_at_utc ASC,id ASC LIMIT 1")
                     : QStringLiteral(" ORDER BY observed_at_utc DESC,id DESC LIMIT 1");

        QSqlQuery query(m_database);
        query.setForwardOnly(true);
        query.prepare(sql);
        query.addBindValue(gameId);
        query.addBindValue(countryCode);
        query.addBindValue(nullableString(currencyCode));
        if (!intervalStartUtc.isEmpty()) query.addBindValue(intervalStartUtc);
        if (!query.exec()) {
            *errorCategory = queryFailure(query);
            return false;
        }
        if (query.next()) {
            *point = PricePoint{query.value(0).toLongLong(),
                                query.value(1).toLongLong(),
                                query.value(2).toString()};
        }
        return true;
    };

    std::optional<PricePoint> intervalFirst;
    std::optional<PricePoint> intervalLast;
    domain::ErrorCategory intervalError = domain::ErrorCategory::None;
    if (!loadIntervalPoint(true, &intervalFirst, &intervalError)
        || !loadIntervalPoint(false, &intervalLast, &intervalError)) {
        return failure<SteamPriceHistorySnapshot>(intervalError);
    }
    if (intervalFirst && intervalLast && intervalFirst->id != intervalLast->id) {
        statistics.hasComparableInterval = true;
        statistics.intervalStartAtUtc = intervalFirst->observedAtUtc;
        statistics.intervalEndAtUtc = intervalLast->observedAtUtc;
        statistics.intervalStartPriceMinor = intervalFirst->priceMinor;
        statistics.intervalEndPriceMinor = intervalLast->priceMinor;
        const qint64 change = intervalLast->priceMinor - intervalFirst->priceMinor;
        statistics.intervalAbsoluteChangeMinor = change;
        if (intervalFirst->priceMinor != 0) {
            const long double percentage =
                static_cast<long double>(change) * 100.0L
                / static_cast<long double>(intervalFirst->priceMinor);
            statistics.intervalPercentageChange = static_cast<double>(percentage);
        }
    }

    if (cancellationRequested && cancellationRequested())
        return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::Cancelled);

    constexpr int pageSize = 256;
    QString cursorAtUtc;
    qint64 cursorId = 0;
    while (true) {
        if (cancellationRequested && cancellationRequested())
            return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::Cancelled);

        QString sql = QStringLiteral(
            "SELECT id,observed_at_utc,price_state,currency_code,current_price_minor "
            "FROM steam_price_observations WHERE steam_game_id=? AND country_code=? "
            "AND currency_code IS ?");
        if (!intervalStartUtc.isEmpty())
            sql += QStringLiteral(" AND observed_at_utc>=?");
        if (!cursorAtUtc.isEmpty())
            sql += QStringLiteral(" AND (observed_at_utc>? OR "
                                  "(observed_at_utc=? AND id>?))");
        sql += QStringLiteral(" ORDER BY observed_at_utc ASC,id ASC LIMIT %1").arg(pageSize);

        QSqlQuery page(m_database);
        page.setForwardOnly(true);
        page.prepare(sql);
        page.addBindValue(gameId);
        page.addBindValue(countryCode);
        page.addBindValue(nullableString(currencyCode));
        if (!intervalStartUtc.isEmpty()) page.addBindValue(intervalStartUtc);
        if (!cursorAtUtc.isEmpty()) {
            page.addBindValue(cursorAtUtc);
            page.addBindValue(cursorAtUtc);
            page.addBindValue(cursorId);
        }
        if (!page.exec())
            return failure<SteamPriceHistorySnapshot>(queryFailure(page));

        int pageCount = 0;
        while (page.next()) {
            SteamPriceHistoryPoint point;
            point.id = page.value(0).toLongLong();
            point.observedAtUtc = page.value(1).toString();
            point.priceState = page.value(2).toString();
            point.currencyCode = page.value(3).toString();
            if (!page.value(4).isNull()) point.priceMinor = page.value(4).toLongLong();
            QDateTime observedAt = QDateTime::fromString(
                point.observedAtUtc, Qt::ISODateWithMs);
            if (!observedAt.isValid())
                observedAt = QDateTime::fromString(point.observedAtUtc, Qt::ISODate);
            if (!observedAt.isValid())
                return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::Storage);
            point.observedAtUtcMs = observedAt.toUTC().toMSecsSinceEpoch();
            cursorAtUtc = point.observedAtUtc;
            cursorId = point.id;
            snapshot.points.push_back(std::move(point));
            ++pageCount;
        }
        if (page.lastError().isValid())
            return failure<SteamPriceHistorySnapshot>(queryFailure(page));
        page.finish();
        if (pageCount < pageSize) break;
    }

    if (cancellationRequested && cancellationRequested())
        return failure<SteamPriceHistorySnapshot>(domain::ErrorCategory::Cancelled);

    RepositoryResult<SteamPriceHistorySnapshot> result;
    result.value = std::move(snapshot);
    return result;
}

RepositoryResult<SteamGame> SteamRepository::load(qint64 id) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT id,steam_app_id,store_url,name,image_source_url,price_status,currency,regular_price_minor,current_price_minor,discount_percent,price_country_code,last_checked_at,last_attempted_at,last_refresh_error,created_at,updated_at FROM steam_games WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec()) return failure<SteamGame>(queryFailure(query));
    if (!query.next()) return failure<SteamGame>(domain::ErrorCategory::NotFound);
    SteamGame game;
    game.id = query.value(0).toLongLong();
    game.steamAppId = query.value(1).toLongLong();
    game.storeUrl = query.value(2).toString();
    game.name = query.value(3).toString();
    game.imageSourceUrl = query.value(4).toString();
    game.priceStatus = query.value(5).toString();
    game.currency = query.value(6).toString();
    if (!query.value(7).isNull()) game.regularPriceMinor = query.value(7).toLongLong();
    if (!query.value(8).isNull()) game.currentPriceMinor = query.value(8).toLongLong();
    game.discountPercent = query.value(9).toInt();
    game.priceCountryCode = query.value(10).toString();
    game.lastCheckedAt = query.value(11).toString();
    game.lastAttemptedAt = query.value(12).toString();
    game.lastRefreshError = query.value(13).toString();
    game.createdAt = query.value(14).toString();
    game.updatedAt = query.value(15).toString();
    query.finish();
    QSqlQuery artwork(m_database);
    artwork.prepare(QStringLiteral("SELECT filename,mime_type,byte_size FROM steam_game_images WHERE game_id=?"));
    artwork.addBindValue(id);
    if (!artwork.exec()) return failure<SteamGame>(queryFailure(artwork));
    if (artwork.next()) game.artwork = AssetMetadata{artwork.value(0).toString(), artwork.value(1).toString(), artwork.value(2).toLongLong()};
    RepositoryResult<SteamGame> result;
    result.value = std::move(game);
    return result;
}

RepositoryResult<SteamGame> SteamRepository::add(
    SteamGame game, const std::function<bool()> &cancellationRequested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    game.name = game.name.trimmed();
    game.priceStatus = game.priceStatus.trimmed().toLower();
    game.currency = game.currency.trimmed().toUpper();
    game.priceCountryCode = game.priceCountryCode.trimmed().toUpper();
    static const QRegularExpression currencyPattern(QStringLiteral("^[A-Z]{3}$"));
    static const QRegularExpression countryPattern(QStringLiteral("^[A-Z]{2}$"));
    if (game.steamAppId <= 0 || game.steamAppId > 4294967295LL
        || game.storeUrl.trimmed().isEmpty() || game.name.isEmpty()
        || game.name.toUcs4().size() > 300
        || game.priceCountryCode.isEmpty() || !countryPattern.match(game.priceCountryCode).hasMatch()
        || (game.priceStatus != QStringLiteral("priced")
            && game.priceStatus != QStringLiteral("free")
            && game.priceStatus != QStringLiteral("unavailable")
            && game.priceStatus != QStringLiteral("unsupported"))
        || (!game.currency.isEmpty() && !currencyPattern.match(game.currency).hasMatch())
        || game.discountPercent < 0 || game.discountPercent > 100
        || (game.regularPriceMinor && *game.regularPriceMinor < 0)
        || (game.currentPriceMinor && *game.currentPriceMinor < 0))
        return failure<SteamGame>(domain::ErrorCategory::Validation);
    if (cancellationRequested && cancellationRequested())
        return failure<SteamGame>(domain::ErrorCategory::Cancelled);
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<SteamGame>(domain::ErrorCategory::Storage);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO steam_games(steam_app_id,store_url,name,image_source_url,price_status,currency,regular_price_minor,current_price_minor,discount_percent,price_country_code,last_checked_at,last_attempted_at,last_refresh_error) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(game.steamAppId);
    query.addBindValue(game.storeUrl);
    query.addBindValue(game.name);
    query.addBindValue(sqlText(game.imageSourceUrl));
    query.addBindValue(game.priceStatus);
    query.addBindValue(nullableString(game.currency));
    query.addBindValue(game.regularPriceMinor ? QVariant(*game.regularPriceMinor) : QVariant());
    query.addBindValue(game.currentPriceMinor ? QVariant(*game.currentPriceMinor) : QVariant());
    query.addBindValue(game.discountPercent);
    query.addBindValue(game.priceCountryCode);
    query.addBindValue(nullableString(game.lastCheckedAt));
    query.addBindValue(nullableString(game.lastAttemptedAt));
    query.addBindValue(sqlText(game.lastRefreshError));
    if (!query.exec()) return failure<SteamGame>(queryFailure(query));
    game.id = query.lastInsertId().toLongLong();
    query.finish();
    const auto catalog = upsertSteamCatalogDetails(m_database, {
        quint32(game.steamAppId), game.storeUrl, game.name, game.imageSourceUrl});
    if (!catalog.succeeded()) return failure<SteamGame>(catalog.error.category());
    const auto observation = recordSteamPriceObservation(m_database, game, game.lastCheckedAt);
    if (!observation.succeeded())
        return failure<SteamGame>(observation.error.category());
    if (cancellationRequested && cancellationRequested())
        return failure<SteamGame>(domain::ErrorCategory::Cancelled);
    const auto inserted = load(game.id);
    if (!inserted.succeeded()) return inserted;
    if (!transaction.commit()) return failure<SteamGame>(domain::ErrorCategory::Storage);
    return inserted;
}

RepositoryResult<SteamGame> SteamRepository::updateRefresh(
    SteamGame game, const std::function<bool()> &cancellationRequested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (game.id <= 0) return failure<SteamGame>(domain::ErrorCategory::Validation);
    game.name = game.name.trimmed();
    game.priceStatus = game.priceStatus.trimmed().toLower();
    game.currency = game.currency.trimmed().toUpper();
    game.priceCountryCode = game.priceCountryCode.trimmed().toUpper();
    static const QRegularExpression currencyPattern(QStringLiteral("^[A-Z]{3}$"));
    static const QRegularExpression countryPattern(QStringLiteral("^[A-Z]{2}$"));
    if (game.steamAppId <= 0 || game.steamAppId > 4294967295LL
        || game.storeUrl.trimmed().isEmpty() || game.name.isEmpty()
        || game.name.toUcs4().size() > 300 || !countryPattern.match(game.priceCountryCode).hasMatch()
        || (game.priceStatus != QStringLiteral("priced")
            && game.priceStatus != QStringLiteral("free")
            && game.priceStatus != QStringLiteral("unavailable")
            && game.priceStatus != QStringLiteral("unsupported"))
        || (!game.currency.isEmpty() && !currencyPattern.match(game.currency).hasMatch())
        || game.discountPercent < 0 || game.discountPercent > 100
        || (game.regularPriceMinor && *game.regularPriceMinor < 0)
        || (game.currentPriceMinor && *game.currentPriceMinor < 0))
        return failure<SteamGame>(domain::ErrorCategory::Validation);
    if (cancellationRequested && cancellationRequested())
        return failure<SteamGame>(domain::ErrorCategory::Cancelled);
    Transaction transaction(m_database);
    if (!transaction.active()) return failure<SteamGame>(domain::ErrorCategory::Storage);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE steam_games SET store_url=?,name=?,image_source_url=?,price_status=?,currency=?,regular_price_minor=?,current_price_minor=?,discount_percent=?,price_country_code=?,last_checked_at=?,last_attempted_at=?,last_refresh_error=?,updated_at=CURRENT_TIMESTAMP WHERE id=?"));
    query.addBindValue(game.storeUrl);
    query.addBindValue(game.name);
    query.addBindValue(sqlText(game.imageSourceUrl));
    query.addBindValue(game.priceStatus);
    query.addBindValue(nullableString(game.currency));
    query.addBindValue(game.regularPriceMinor ? QVariant(*game.regularPriceMinor) : QVariant());
    query.addBindValue(game.currentPriceMinor ? QVariant(*game.currentPriceMinor) : QVariant());
    query.addBindValue(game.discountPercent);
    query.addBindValue(game.priceCountryCode);
    query.addBindValue(nullableString(game.lastCheckedAt));
    query.addBindValue(nullableString(game.lastAttemptedAt));
    query.addBindValue(sqlText(game.lastRefreshError));
    query.addBindValue(game.id);
    if (!query.exec()) return failure<SteamGame>(queryFailure(query));
    if (query.numRowsAffected() != 1) return failure<SteamGame>(domain::ErrorCategory::NotFound);
    query.finish();
    const auto catalog = upsertSteamCatalogDetails(m_database, {
        quint32(game.steamAppId), game.storeUrl, game.name, game.imageSourceUrl});
    if (!catalog.succeeded()) return failure<SteamGame>(catalog.error.category());
    const auto observation = recordSteamPriceObservation(m_database, game, game.lastCheckedAt);
    if (!observation.succeeded())
        return failure<SteamGame>(observation.error.category());
    if (cancellationRequested && cancellationRequested())
        return failure<SteamGame>(domain::ErrorCategory::Cancelled);
    const auto updated = load(game.id);
    if (!updated.succeeded()) return updated;
    if (!transaction.commit()) return failure<SteamGame>(domain::ErrorCategory::Storage);
    return updated;
}

RepositoryResult<void> SteamRepository::applyRefreshBatch(
    const QVector<SteamRefreshUpdate> &updates,
    const std::function<bool()> &cancellationRequested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);

    QSqlQuery failedUpdate(m_database);
    failedUpdate.prepare(QStringLiteral(
        "UPDATE steam_games SET last_attempted_at=?,last_refresh_error=?,updated_at=? WHERE id=?"));
    QSqlQuery successfulUpdate(m_database);
    successfulUpdate.prepare(QStringLiteral(
        "UPDATE steam_games SET name=?,image_source_url=?,price_status=?,currency=?,"
        "regular_price_minor=?,current_price_minor=?,discount_percent=?,price_country_code=?,"
        "last_checked_at=?,last_attempted_at=?,last_refresh_error='',updated_at=? WHERE id=?"));

    static const QRegularExpression countryPattern(QStringLiteral("^[A-Z]{2}$"));
    static const QRegularExpression currencyPattern(QStringLiteral("^[A-Z]{3}$"));
    QSet<qint64> updatedIds;
    for (const SteamRefreshUpdate &update : updates) {
        if (cancellationRequested && cancellationRequested())
            return failureVoid(domain::ErrorCategory::Cancelled);
        if (update.gameId <= 0 || update.attemptedAt.isEmpty()
            || updatedIds.contains(update.gameId))
            return failureVoid(domain::ErrorCategory::Validation);
        updatedIds.insert(update.gameId);

        if (!update.refreshedGame) {
            QString safeError = update.safeError.trimmed();
            if (safeError.isEmpty()) safeError = QStringLiteral("Price could not be refreshed.");
            const auto points = safeError.toUcs4();
            if (points.size() > 1000)
                safeError = QString::fromUcs4(points.constData(), 1000);
            failedUpdate.bindValue(0, update.attemptedAt);
            failedUpdate.bindValue(1, safeError);
            failedUpdate.bindValue(2, update.attemptedAt);
            failedUpdate.bindValue(3, update.gameId);
            if (!failedUpdate.exec()) return failureVoid(queryFailure(failedUpdate));
            if (failedUpdate.numRowsAffected() != 1)
                return failureVoid(domain::ErrorCategory::NotFound);
            failedUpdate.finish();
            continue;
        }

        const SteamGame &game = *update.refreshedGame;
        const bool knownStatus = game.priceStatus == QStringLiteral("priced")
            || game.priceStatus == QStringLiteral("free")
            || game.priceStatus == QStringLiteral("unavailable")
            || game.priceStatus == QStringLiteral("unsupported");
        if (game.id != update.gameId || game.steamAppId <= 0
            || game.steamAppId > 4294967295LL || game.name.trimmed().isEmpty()
            || game.name.toUcs4().size() > 300 || !knownStatus
            || !countryPattern.match(game.priceCountryCode).hasMatch()
            || (!game.currency.isEmpty() && !currencyPattern.match(game.currency).hasMatch())
            || game.discountPercent < 0 || game.discountPercent > 100
            || (game.regularPriceMinor && *game.regularPriceMinor < 0)
            || (game.currentPriceMinor && *game.currentPriceMinor < 0)
            || (game.priceStatus == QStringLiteral("priced")
                && (game.currency.isEmpty() || !game.currentPriceMinor
                    || !game.regularPriceMinor)))
            return failureVoid(domain::ErrorCategory::Validation);

        successfulUpdate.bindValue(0, game.name.trimmed());
        successfulUpdate.bindValue(1, game.imageSourceUrl);
        successfulUpdate.bindValue(2, game.priceStatus);
        successfulUpdate.bindValue(3, nullableString(game.currency));
        successfulUpdate.bindValue(4, game.regularPriceMinor
            ? QVariant(*game.regularPriceMinor) : QVariant());
        successfulUpdate.bindValue(5, game.currentPriceMinor
            ? QVariant(*game.currentPriceMinor) : QVariant());
        successfulUpdate.bindValue(6, game.discountPercent);
        successfulUpdate.bindValue(7, game.priceCountryCode);
        successfulUpdate.bindValue(8, update.attemptedAt);
        successfulUpdate.bindValue(9, update.attemptedAt);
        successfulUpdate.bindValue(10, update.attemptedAt);
        successfulUpdate.bindValue(11, update.gameId);
        if (!successfulUpdate.exec()) return failureVoid(queryFailure(successfulUpdate));
        if (successfulUpdate.numRowsAffected() != 1)
            return failureVoid(domain::ErrorCategory::NotFound);
        successfulUpdate.finish();
        const auto catalog = upsertSteamCatalogDetails(m_database, {
            quint32(game.steamAppId), game.storeUrl, game.name, game.imageSourceUrl});
        if (!catalog.succeeded()) return failureVoid(catalog.error.category());
        const auto observation = recordSteamPriceObservation(
            m_database, game, update.attemptedAt);
        if (!observation.succeeded())
            return failureVoid(observation.error.category());
    }

    if (cancellationRequested && cancellationRequested())
        return failureVoid(domain::ErrorCategory::Cancelled);
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<void> SteamRepository::setArtwork(qint64 gameId, const AssetMetadata &asset)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (gameId <= 0 || !validAsset(asset, {QStringLiteral("image/jpeg"), QStringLiteral("image/png"), QStringLiteral("image/webp")}))
        return failureVoid(domain::ErrorCategory::Validation);
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    QSqlQuery gameQuery(m_database);
    gameQuery.prepare(QStringLiteral(
        "SELECT steam_app_id,store_url,name,image_source_url FROM steam_games WHERE id=?"));
    gameQuery.addBindValue(gameId);
    if (!gameQuery.exec()) return failureVoid(queryFailure(gameQuery));
    if (!gameQuery.next()) return failureVoid(domain::ErrorCategory::NotFound);
    const qint64 storedAppId = gameQuery.value(0).toLongLong();
    if (storedAppId <= 0 || storedAppId > 4294967295LL)
        return failureVoid(domain::ErrorCategory::Storage);
    const quint32 appId = quint32(storedAppId);
    const QString storeUrl = gameQuery.value(1).toString();
    const QString name = gameQuery.value(2).toString();
    const QString sourceUrl = gameQuery.value(3).toString();
    gameQuery.finish();
    if (sourceUrl.isEmpty()) return failureVoid(domain::ErrorCategory::Conflict);
    const auto catalog = upsertSteamCatalogDetails(m_database, {
        appId, storeUrl, name, sourceUrl});
    if (!catalog.succeeded()) return failureVoid(catalog.error.category());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO steam_game_images(game_id,filename,mime_type,byte_size) VALUES(?,?,?,?) ON CONFLICT(game_id) DO UPDATE SET filename=excluded.filename,mime_type=excluded.mime_type,byte_size=excluded.byte_size,updated_at=CURRENT_TIMESTAMP"));
    query.addBindValue(gameId);
    bindAsset(query, 1, asset);
    if (!query.exec()) return failureVoid(queryFailure(query));
    query.finish();
    const auto sharedArtwork = setSteamCatalogArtwork(m_database, appId, sourceUrl,
        {sourceUrl, asset.filename, asset.mimeType, asset.byteSize});
    if (!sharedArtwork.succeeded()) return failureVoid(sharedArtwork.error.category());
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<void> SteamRepository::removeArtwork(qint64 gameId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM steam_game_images WHERE game_id=?"));
    query.addBindValue(gameId);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<void> SteamRepository::remove(qint64 id)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM steam_games WHERE id=?"));
    query.addBindValue(id);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return query.numRowsAffected() == 1 ? RepositoryResult<void>{}
                                        : failureVoid(domain::ErrorCategory::NotFound);
}

RepositoryResult<bool> SteamRepository::hasArtworkFilename(const QString &filename) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (filename.isEmpty()) return failure<bool>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT 1 FROM steam_game_images WHERE filename=? LIMIT 1"));
    query.addBindValue(filename);
    if (!query.exec()) return failure<bool>(queryFailure(query));
    RepositoryResult<bool> result;
    result.value = query.next();
    return result;
}

SteamReleaseRepository::SteamReleaseRepository(QSqlDatabase &database)
    : m_database(database)
{
}

RepositoryResult<QVector<domain::SteamTrackedRelease>> SteamReleaseRepository::list() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT steam_app_id FROM steam_release_follows ORDER BY title COLLATE NOCASE,steam_app_id"))) {
        return failure<QVector<domain::SteamTrackedRelease>>(queryFailure(query));
    }
    QVector<quint32> appIds;
    while (query.next()) appIds.push_back(query.value(0).toUInt());
    query.finish();

    RepositoryResult<QVector<domain::SteamTrackedRelease>> result;
    result.value.reserve(appIds.size());
    for (quint32 appId : appIds) {
        const auto release = load(appId);
        if (!release.succeeded())
            return failure<QVector<domain::SteamTrackedRelease>>(release.error.category());
        result.value.push_back(release.value);
    }
    return result;
}

RepositoryResult<QVector<domain::SteamTrackedRelease>>
SteamReleaseRepository::exactDateEvents(
    QDate startDate, QDate endDate,
    const std::function<bool()> &cancellationRequested) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (!startDate.isValid() || !endDate.isValid() || startDate > endDate)
        return failure<QVector<domain::SteamTrackedRelease>>(domain::ErrorCategory::Validation);
    if (cancellationRequested && cancellationRequested())
        return failure<QVector<domain::SteamTrackedRelease>>(domain::ErrorCategory::Cancelled);

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT steam_app_id FROM steam_release_follows "
        "WHERE release_date_precision='exact_day' "
        "AND release_date_exact BETWEEN ? AND ? "
        "ORDER BY release_date_exact,title COLLATE NOCASE,steam_app_id"));
    query.addBindValue(startDate.toString(Qt::ISODate));
    query.addBindValue(endDate.toString(Qt::ISODate));
    if (!query.exec())
        return failure<QVector<domain::SteamTrackedRelease>>(queryFailure(query));

    QVector<quint32> appIds;
    while (query.next()) {
        if (cancellationRequested && cancellationRequested())
            return failure<QVector<domain::SteamTrackedRelease>>(domain::ErrorCategory::Cancelled);
        appIds.push_back(query.value(0).toUInt());
    }
    query.finish();

    RepositoryResult<QVector<domain::SteamTrackedRelease>> result;
    result.value.reserve(appIds.size());
    for (quint32 appId : appIds) {
        if (cancellationRequested && cancellationRequested())
            return failure<QVector<domain::SteamTrackedRelease>>(domain::ErrorCategory::Cancelled);
        const auto release = load(appId);
        if (!release.succeeded())
            return failure<QVector<domain::SteamTrackedRelease>>(release.error.category());
        if (!release.value.releaseDate.hasExactDate()) {
            return failure<QVector<domain::SteamTrackedRelease>>(domain::ErrorCategory::Storage);
        }
        result.value.push_back(release.value);
    }
    return result;
}

RepositoryResult<domain::SteamTrackedRelease> SteamReleaseRepository::get(quint32 appId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0) return failure<domain::SteamTrackedRelease>(domain::ErrorCategory::Validation);
    return load(appId);
}

RepositoryResult<domain::SteamTrackedRelease> SteamReleaseRepository::load(quint32 appId) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT steam_app_id,store_url,title,artwork_source_url,artwork_filename,artwork_mime_type,"
        "artwork_byte_size,release_date_text,release_date_precision,release_date_exact,"
        "release_window_start,release_window_end,release_status,followed_at_utc,"
        "last_successful_update_at_utc,last_attempted_update_at_utc,refresh_state,refresh_error,"
        "date_change_unacknowledged "
        "FROM steam_release_follows WHERE steam_app_id=?"));
    query.addBindValue(appId);
    if (!query.exec()) return failure<domain::SteamTrackedRelease>(queryFailure(query));
    if (!query.next()) return failure<domain::SteamTrackedRelease>(domain::ErrorCategory::NotFound);

    domain::SteamTrackedRelease release;
    release.appId = query.value(0).toUInt();
    release.canonicalStoreUrl = query.value(1).toString();
    release.title = query.value(2).toString();
    release.artwork.sourceUrl = query.value(3).toString();
    release.artwork.cachedFilename = query.value(4).toString();
    release.artwork.mimeType = query.value(5).toString();
    release.artwork.byteSize = query.value(6).toLongLong();
    release.releaseDate.announcedText = query.value(7).toString();
    const auto precision = domain::steamReleaseDatePrecisionFromName(query.value(8).toString());
    const auto status = domain::steamReleaseStatusFromName(query.value(12).toString());
    const auto refreshState = domain::steamReleaseRefreshStateFromName(query.value(16).toString());
    if (!precision || !status || !refreshState)
        return failure<domain::SteamTrackedRelease>(domain::ErrorCategory::Storage);
    release.releaseDate.precision = *precision;
    if (!query.value(9).isNull()) {
        const QDate date = QDate::fromString(query.value(9).toString(), Qt::ISODate);
        if (!date.isValid()) return failure<domain::SteamTrackedRelease>(domain::ErrorCategory::Storage);
        release.releaseDate.exactDate = date;
    }
    if (!query.value(10).isNull()) {
        const QDate date = QDate::fromString(query.value(10).toString(), Qt::ISODate);
        if (!date.isValid()) return failure<domain::SteamTrackedRelease>(domain::ErrorCategory::Storage);
        release.releaseDate.windowStart = date;
    }
    if (!query.value(11).isNull()) {
        const QDate date = QDate::fromString(query.value(11).toString(), Qt::ISODate);
        if (!date.isValid()) return failure<domain::SteamTrackedRelease>(domain::ErrorCategory::Storage);
        release.releaseDate.windowEnd = date;
    }
    release.status = *status;
    release.followedAtUtc = query.value(13).toString();
    release.lastSuccessfulUpdateAtUtc = query.value(14).toString();
    release.lastAttemptedUpdateAtUtc = query.value(15).toString();
    release.refreshState = *refreshState;
    release.refreshError = query.value(17).toString();
    release.dateChangeUnacknowledged = query.value(18).toBool();
    if (!validTrackedSteamRelease(release))
        return failure<domain::SteamTrackedRelease>(domain::ErrorCategory::Storage);

    RepositoryResult<domain::SteamTrackedRelease> result;
    result.value = std::move(release);
    return result;
}

RepositoryResult<domain::SteamTrackedRelease> SteamReleaseRepository::add(
    domain::SteamTrackedRelease release,
    const std::function<bool()> &cancellationRequested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    release.title = release.title.trimmed();
    const domain::SteamStoreUrl normalized =
        domain::normalizeSteamStoreUrl(release.canonicalStoreUrl);
    if (normalized.isValid()) release.canonicalStoreUrl = normalized.canonicalUrl;
    if (!validTrackedSteamRelease(release))
        return failure<domain::SteamTrackedRelease>(domain::ErrorCategory::Validation);
    if (cancellationRequested && cancellationRequested())
        return failure<domain::SteamTrackedRelease>(domain::ErrorCategory::Cancelled);

    Transaction transaction(m_database);
    if (!transaction.active())
        return failure<domain::SteamTrackedRelease>(domain::ErrorCategory::Storage);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO steam_release_follows(steam_app_id,store_url,title,artwork_source_url,"
        "artwork_filename,artwork_mime_type,artwork_byte_size,release_date_text,"
        "release_date_precision,release_date_exact,release_window_start,release_window_end,"
        "release_status,followed_at_utc,last_successful_update_at_utc,"
        "last_attempted_update_at_utc,refresh_state,refresh_error) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(release.appId);
    query.addBindValue(release.canonicalStoreUrl);
    query.addBindValue(release.title);
    query.addBindValue(sqlText(release.artwork.sourceUrl));
    query.addBindValue(sqlText(release.artwork.cachedFilename));
    query.addBindValue(sqlText(release.artwork.mimeType));
    query.addBindValue(release.artwork.byteSize);
    query.addBindValue(sqlText(release.releaseDate.announcedText));
    query.addBindValue(domain::steamReleaseDatePrecisionName(release.releaseDate.precision));
    query.addBindValue(release.releaseDate.exactDate
        ? QVariant(release.releaseDate.exactDate->toString(Qt::ISODate)) : QVariant());
    query.addBindValue(release.releaseDate.windowStart
        ? QVariant(release.releaseDate.windowStart->toString(Qt::ISODate)) : QVariant());
    query.addBindValue(release.releaseDate.windowEnd
        ? QVariant(release.releaseDate.windowEnd->toString(Qt::ISODate)) : QVariant());
    query.addBindValue(domain::steamReleaseStatusName(release.status));
    query.addBindValue(release.followedAtUtc);
    query.addBindValue(nullableString(release.lastSuccessfulUpdateAtUtc));
    query.addBindValue(nullableString(release.lastAttemptedUpdateAtUtc));
    query.addBindValue(domain::steamReleaseRefreshStateName(release.refreshState));
    query.addBindValue(sqlText(release.refreshError));
    if (!query.exec()) return failure<domain::SteamTrackedRelease>(queryFailure(query));
    query.finish();
    const auto catalog = upsertSteamCatalogDetails(m_database, {
        release.appId, release.canonicalStoreUrl, release.title, release.artwork.sourceUrl});
    if (!catalog.succeeded())
        return failure<domain::SteamTrackedRelease>(catalog.error.category());
    if (cancellationRequested && cancellationRequested())
        return failure<domain::SteamTrackedRelease>(domain::ErrorCategory::Cancelled);
    const auto inserted = load(release.appId);
    if (!inserted.succeeded()) return inserted;
    if (!transaction.commit())
        return failure<domain::SteamTrackedRelease>(domain::ErrorCategory::Storage);
    return inserted;
}

RepositoryResult<void> SteamReleaseRepository::applyRefreshBatch(
    const QVector<SteamReleaseRefreshUpdate> &updates,
    const std::function<bool()> &cancellationRequested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSet<quint32> seen;
    for (const SteamReleaseRefreshUpdate &update : updates) {
        if (update.appId == 0 || seen.contains(update.appId)
            || !validUtcInstant(update.attemptedAtUtc)) {
            return failureVoid(domain::ErrorCategory::Validation);
        }
        seen.insert(update.appId);
        if (update.refreshedDetails) {
            if (update.refreshedDetails->appId != update.appId
                || !domain::isValidSteamReleaseDetails(*update.refreshedDetails)
                || !update.safeError.isEmpty()) {
                return failureVoid(domain::ErrorCategory::Validation);
            }
        } else if (update.safeError.trimmed().isEmpty() || update.safeError.size() > 500) {
            return failureVoid(domain::ErrorCategory::Validation);
        }
    }
    if (cancellationRequested && cancellationRequested())
        return failureVoid(domain::ErrorCategory::Cancelled);

    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    for (const SteamReleaseRefreshUpdate &update : updates) {
        const auto existing = load(update.appId);
        if (!existing.succeeded()) return failureVoid(existing.error.category());

        QSqlQuery query(m_database);
        if (update.refreshedDetails) {
            const domain::SteamReleaseDetails &details = *update.refreshedDetails;
            const bool dateChanged = existing.value.releaseDate != details.releaseDate;
            query.prepare(QStringLiteral(
                "UPDATE steam_release_follows SET title=?,artwork_filename=CASE "
                "WHEN artwork_source_url=? THEN artwork_filename ELSE '' END,"
                "artwork_mime_type=CASE WHEN artwork_source_url=? THEN artwork_mime_type ELSE '' END,"
                "artwork_byte_size=CASE WHEN artwork_source_url=? THEN artwork_byte_size ELSE 0 END,"
                "artwork_source_url=?,release_date_text=?,release_date_precision=?,"
                "release_date_exact=?,release_window_start=?,release_window_end=?,release_status=?,"
                "date_change_unacknowledged=CASE WHEN ? THEN 1 "
                "ELSE date_change_unacknowledged END,"
                "last_successful_update_at_utc=?,last_attempted_update_at_utc=?,"
                "refresh_state='succeeded',refresh_error='',updated_at=? WHERE steam_app_id=?"));
            query.addBindValue(details.title.trimmed());
            query.addBindValue(sqlText(details.artworkUrl));
            query.addBindValue(sqlText(details.artworkUrl));
            query.addBindValue(sqlText(details.artworkUrl));
            query.addBindValue(sqlText(details.artworkUrl));
            query.addBindValue(sqlText(details.releaseDate.announcedText));
            query.addBindValue(domain::steamReleaseDatePrecisionName(details.releaseDate.precision));
            query.addBindValue(details.releaseDate.exactDate
                ? QVariant(details.releaseDate.exactDate->toString(Qt::ISODate)) : QVariant());
            query.addBindValue(details.releaseDate.windowStart
                ? QVariant(details.releaseDate.windowStart->toString(Qt::ISODate)) : QVariant());
            query.addBindValue(details.releaseDate.windowEnd
                ? QVariant(details.releaseDate.windowEnd->toString(Qt::ISODate)) : QVariant());
            query.addBindValue(domain::steamReleaseStatusName(details.status));
            query.addBindValue(dateChanged);
            query.addBindValue(update.attemptedAtUtc);
            query.addBindValue(update.attemptedAtUtc);
            query.addBindValue(update.attemptedAtUtc);
            query.addBindValue(update.appId);
        } else {
            query.prepare(QStringLiteral(
                "UPDATE steam_release_follows SET last_attempted_update_at_utc=?,"
                "refresh_state='failed',refresh_error=?,updated_at=? WHERE steam_app_id=?"));
            query.addBindValue(update.attemptedAtUtc);
            query.addBindValue(update.safeError.trimmed());
            query.addBindValue(update.attemptedAtUtc);
            query.addBindValue(update.appId);
        }
        if (!query.exec()) return failureVoid(queryFailure(query));
        if (query.numRowsAffected() != 1) return failureVoid(domain::ErrorCategory::NotFound);
        query.finish();
        if (update.refreshedDetails) {
            const domain::SteamReleaseDetails &details = *update.refreshedDetails;
            const auto catalog = upsertSteamCatalogDetails(m_database, {
                update.appId, existing.value.canonicalStoreUrl, details.title,
                details.artworkUrl});
            if (!catalog.succeeded()) return failureVoid(catalog.error.category());
        }
        if (cancellationRequested && cancellationRequested())
            return failureVoid(domain::ErrorCategory::Cancelled);
    }
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<void> SteamReleaseRepository::acknowledgeDateChange(quint32 appId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0) return failureVoid(domain::ErrorCategory::Validation);

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE steam_release_follows SET date_change_unacknowledged=0,"
        "updated_at=CURRENT_TIMESTAMP WHERE steam_app_id=?"));
    query.addBindValue(appId);
    if (!query.exec()) return failureVoid(queryFailure(query));
    if (query.numRowsAffected() == 1) return {};

    query.finish();
    QSqlQuery exists(m_database);
    exists.prepare(QStringLiteral(
        "SELECT 1 FROM steam_release_follows WHERE steam_app_id=?"));
    exists.addBindValue(appId);
    if (!exists.exec()) return failureVoid(queryFailure(exists));
    if (exists.next()) return {};
    return failureVoid(domain::ErrorCategory::NotFound);
}

RepositoryResult<void> SteamReleaseRepository::setArtwork(
    quint32 appId, const QString &expectedSourceUrl,
    const domain::SteamReleaseArtworkMetadata &artwork)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0 || expectedSourceUrl.isEmpty()
        || domain::normalizeSteamArtworkUrl(expectedSourceUrl) != expectedSourceUrl
        || artwork.sourceUrl != expectedSourceUrl
        || !validSteamReleaseArtwork(appId, artwork)
        || artwork.cachedFilename.isEmpty()) {
        return failureVoid(domain::ErrorCategory::Validation);
    }
    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    const auto current = load(appId);
    if (!current.succeeded()) return failureVoid(current.error.category());
    if (current.value.artwork.sourceUrl != expectedSourceUrl)
        return failureVoid(domain::ErrorCategory::Conflict);
    const auto catalog = upsertSteamCatalogDetails(m_database, {
        appId, current.value.canonicalStoreUrl, current.value.title, expectedSourceUrl});
    if (!catalog.succeeded()) return failureVoid(catalog.error.category());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE steam_release_follows SET artwork_filename=?,artwork_mime_type=?,"
        "artwork_byte_size=?,updated_at=CURRENT_TIMESTAMP "
        "WHERE steam_app_id=? AND artwork_source_url=?"));
    query.addBindValue(sqlText(artwork.cachedFilename));
    query.addBindValue(sqlText(artwork.mimeType));
    query.addBindValue(artwork.byteSize);
    query.addBindValue(appId);
    query.addBindValue(expectedSourceUrl);
    if (!query.exec()) return failureVoid(queryFailure(query));
    if (query.numRowsAffected() == 1) {
        query.finish();
        const auto sharedArtwork = setSteamCatalogArtwork(m_database, appId,
            expectedSourceUrl, {expectedSourceUrl, artwork.cachedFilename,
                                artwork.mimeType, artwork.byteSize});
        if (!sharedArtwork.succeeded())
            return failureVoid(sharedArtwork.error.category());
        if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
        return {};
    }
    query.finish();
    QSqlQuery exists(m_database);
    exists.prepare(QStringLiteral("SELECT 1 FROM steam_release_follows WHERE steam_app_id=?"));
    exists.addBindValue(appId);
    if (!exists.exec()) return failureVoid(queryFailure(exists));
    return failureVoid(exists.next() ? domain::ErrorCategory::Conflict
                                     : domain::ErrorCategory::NotFound);
}

RepositoryResult<bool> SteamReleaseRepository::hasArtworkFilename(const QString &filename) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (filename.isEmpty()) return failure<bool>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM steam_release_follows WHERE artwork_filename=? LIMIT 1"));
    query.addBindValue(filename);
    if (!query.exec()) return failure<bool>(queryFailure(query));
    RepositoryResult<bool> result;
    result.value = query.next();
    return result;
}

RepositoryResult<void> SteamReleaseRepository::remove(quint32 appId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM steam_release_follows WHERE steam_app_id=?"));
    query.addBindValue(appId);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return query.numRowsAffected() == 1 ? RepositoryResult<void>{}
                                        : failureVoid(domain::ErrorCategory::NotFound);
}

SteamGameUpdatesRepository::SteamGameUpdatesRepository(QSqlDatabase &database)
    : m_database(database)
{
}

RepositoryResult<QVector<domain::SteamGameUpdateFollow>> SteamGameUpdatesRepository::list(
    const std::function<bool()> &cancellationRequested) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT steam_app_id FROM steam_game_update_follows "
            "ORDER BY name COLLATE NOCASE,steam_app_id"))) {
        return failure<QVector<domain::SteamGameUpdateFollow>>(queryFailure(query));
    }
    QVector<quint32> appIds;
    while (query.next()) {
        if (cancellationRequested && cancellationRequested())
            return failure<QVector<domain::SteamGameUpdateFollow>>(
                domain::ErrorCategory::Cancelled);
        appIds.push_back(query.value(0).toUInt());
    }
    query.finish();

    RepositoryResult<QVector<domain::SteamGameUpdateFollow>> result;
    result.value.reserve(appIds.size());
    for (quint32 appId : appIds) {
        if (cancellationRequested && cancellationRequested())
            return failure<QVector<domain::SteamGameUpdateFollow>>(
                domain::ErrorCategory::Cancelled);
        const auto follow = load(appId);
        if (!follow.succeeded())
            return failure<QVector<domain::SteamGameUpdateFollow>>(
                follow.error.category());
        result.value.push_back(follow.value);
    }
    return result;
}

RepositoryResult<domain::SteamGameUpdateFollow> SteamGameUpdatesRepository::get(
    quint32 appId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0)
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::Validation);
    return load(appId);
}

RepositoryResult<domain::SteamGameUpdateFollow> SteamGameUpdatesRepository::load(
    quint32 appId) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT steam_app_id,store_url,name,artwork_source_url,artwork_filename,"
        "artwork_mime_type,artwork_byte_size,ownership_status,added_at_utc,"
        "last_successful_news_check_at_utc,last_attempted_news_check_at_utc,"
        "refresh_state,last_refresh_error "
        "FROM steam_game_update_follows WHERE steam_app_id=?"));
    query.addBindValue(appId);
    if (!query.exec()) return failure<domain::SteamGameUpdateFollow>(queryFailure(query));
    if (!query.next())
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::NotFound);

    domain::SteamGameUpdateFollow follow;
    follow.appId = query.value(0).toUInt();
    follow.canonicalStoreUrl = query.value(1).toString();
    follow.name = query.value(2).toString();
    follow.artwork.sourceUrl = query.value(3).toString();
    follow.artwork.cachedFilename = query.value(4).toString();
    follow.artwork.mimeType = query.value(5).toString();
    follow.artwork.byteSize = query.value(6).toLongLong();
    const auto ownership = domain::steamGameUpdateOwnershipStatusFromName(
        query.value(7).toString());
    follow.addedAtUtc = query.value(8).toString();
    follow.lastSuccessfulNewsCheckAtUtc = query.value(9).toString();
    follow.lastAttemptedNewsCheckAtUtc = query.value(10).toString();
    const auto refreshState = domain::steamGameUpdateRefreshStateFromName(
        query.value(11).toString());
    follow.lastRefreshError = query.value(12).toString();
    if (!ownership || !refreshState)
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::Storage);
    follow.ownershipStatus = *ownership;
    follow.refreshState = *refreshState;
    if (!domain::isValidSteamGameUpdateFollow(follow))
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::Storage);

    RepositoryResult<domain::SteamGameUpdateFollow> result;
    result.value = std::move(follow);
    return result;
}

RepositoryResult<domain::SteamGameUpdateFollow> SteamGameUpdatesRepository::add(
    domain::SteamGameUpdateFollow follow,
    const std::function<bool()> &cancellationRequested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    follow.name = follow.name.trimmed();
    const domain::SteamStoreUrl normalized =
        domain::normalizeSteamStoreUrl(follow.canonicalStoreUrl);
    if (normalized.isValid()) follow.canonicalStoreUrl = normalized.canonicalUrl;
    if (!domain::isValidSteamGameUpdateFollow(follow))
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::Validation);
    if (cancellationRequested && cancellationRequested())
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::Cancelled);

    Transaction transaction(m_database);
    if (!transaction.active())
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::Storage);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO steam_game_update_follows(steam_app_id,store_url,name,"
        "artwork_source_url,artwork_filename,artwork_mime_type,artwork_byte_size,"
        "ownership_status,added_at_utc,last_successful_news_check_at_utc,"
        "last_attempted_news_check_at_utc,refresh_state,last_refresh_error) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(follow.appId);
    query.addBindValue(follow.canonicalStoreUrl);
    query.addBindValue(follow.name);
    query.addBindValue(sqlText(follow.artwork.sourceUrl));
    query.addBindValue(sqlText(follow.artwork.cachedFilename));
    query.addBindValue(sqlText(follow.artwork.mimeType));
    query.addBindValue(follow.artwork.byteSize);
    query.addBindValue(domain::steamGameUpdateOwnershipStatusName(follow.ownershipStatus));
    query.addBindValue(follow.addedAtUtc);
    query.addBindValue(nullableString(follow.lastSuccessfulNewsCheckAtUtc));
    query.addBindValue(nullableString(follow.lastAttemptedNewsCheckAtUtc));
    query.addBindValue(domain::steamGameUpdateRefreshStateName(follow.refreshState));
    query.addBindValue(sqlText(follow.lastRefreshError));
    if (!query.exec())
        return failure<domain::SteamGameUpdateFollow>(queryFailure(query));
    query.finish();
    const auto catalog = upsertSteamCatalogDetails(m_database, {
        follow.appId, follow.canonicalStoreUrl, follow.name, follow.artwork.sourceUrl});
    if (!catalog.succeeded())
        return failure<domain::SteamGameUpdateFollow>(catalog.error.category());
    if (cancellationRequested && cancellationRequested())
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::Cancelled);
    const auto inserted = load(follow.appId);
    if (!inserted.succeeded())
        return failure<domain::SteamGameUpdateFollow>(inserted.error.category());
    if (!transaction.commit())
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::Storage);
    return inserted;
}

RepositoryResult<domain::SteamGameUpdateFollow>
SteamGameUpdatesRepository::setOwnershipStatus(
    quint32 appId, domain::SteamGameUpdateOwnershipStatus status)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0
        || (status != domain::SteamGameUpdateOwnershipStatus::Owned
            && status != domain::SteamGameUpdateOwnershipStatus::NotOwned)) {
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::Validation);
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE steam_game_update_follows SET ownership_status=?,"
        "updated_at=CURRENT_TIMESTAMP WHERE steam_app_id=?"));
    query.addBindValue(domain::steamGameUpdateOwnershipStatusName(status));
    query.addBindValue(appId);
    if (!query.exec())
        return failure<domain::SteamGameUpdateFollow>(queryFailure(query));
    if (query.numRowsAffected() != 1)
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::NotFound);
    return load(appId);
}

RepositoryResult<domain::SteamGameUpdateFollow>
SteamGameUpdatesRepository::recordNewsCheck(
    quint32 appId, QString attemptedAtUtc, bool succeeded, QString safeError)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    attemptedAtUtc = attemptedAtUtc.trimmed();
    safeError = safeError.trimmed();
    if (appId == 0 || !validUtcInstant(attemptedAtUtc)
        || (succeeded && !safeError.isEmpty())
        || (!succeeded && (safeError.isEmpty() || safeError.size() > 500))) {
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::Validation);
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE steam_game_update_follows SET "
        "last_successful_news_check_at_utc=CASE WHEN ?=1 THEN ? "
        "ELSE last_successful_news_check_at_utc END,"
        "last_attempted_news_check_at_utc=?,refresh_state=?,last_refresh_error=?,"
        "updated_at=CURRENT_TIMESTAMP WHERE steam_app_id=?"));
    query.addBindValue(succeeded ? 1 : 0);
    query.addBindValue(succeeded ? QVariant(attemptedAtUtc) : QVariant());
    query.addBindValue(attemptedAtUtc);
    query.addBindValue(domain::steamGameUpdateRefreshStateName(
        succeeded ? domain::SteamGameUpdateRefreshState::Succeeded
                  : domain::SteamGameUpdateRefreshState::Failed));
    query.addBindValue(sqlText(safeError));
    query.addBindValue(appId);
    if (!query.exec())
        return failure<domain::SteamGameUpdateFollow>(queryFailure(query));
    if (query.numRowsAffected() != 1)
        return failure<domain::SteamGameUpdateFollow>(domain::ErrorCategory::NotFound);
    return load(appId);
}

RepositoryResult<void> SteamGameUpdatesRepository::setArtwork(
    quint32 appId, const QString &expectedSourceUrl,
    const domain::SteamGameUpdateArtworkMetadata &artwork)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    const auto current = get(appId);
    if (!current.succeeded()) return failureVoid(current.error.category());
    if (expectedSourceUrl.isEmpty()
        || domain::normalizeSteamArtworkUrl(expectedSourceUrl) != expectedSourceUrl
        || current.value.artwork.sourceUrl != expectedSourceUrl) {
        return failureVoid(domain::ErrorCategory::Conflict);
    }
    domain::SteamGameUpdateFollow updated = current.value;
    updated.artwork = artwork;
    if (!domain::isValidSteamGameUpdateFollow(updated))
        return failureVoid(domain::ErrorCategory::Validation);

    Transaction transaction(m_database);
    if (!transaction.active()) return failureVoid(domain::ErrorCategory::Storage);
    const auto catalog = upsertSteamCatalogDetails(m_database, {
        appId, current.value.canonicalStoreUrl, current.value.name, expectedSourceUrl});
    if (!catalog.succeeded()) return failureVoid(catalog.error.category());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE steam_game_update_follows SET artwork_filename=?,artwork_mime_type=?,"
        "artwork_byte_size=?,updated_at=CURRENT_TIMESTAMP "
        "WHERE steam_app_id=? AND artwork_source_url=?"));
    query.addBindValue(sqlText(artwork.cachedFilename));
    query.addBindValue(sqlText(artwork.mimeType));
    query.addBindValue(artwork.byteSize);
    query.addBindValue(appId);
    query.addBindValue(expectedSourceUrl);
    if (!query.exec()) return failureVoid(queryFailure(query));
    if (query.numRowsAffected() != 1)
        return failureVoid(domain::ErrorCategory::Conflict);
    query.finish();
    const auto sharedArtwork = setSteamCatalogArtwork(m_database, appId,
        expectedSourceUrl, {expectedSourceUrl, artwork.cachedFilename,
                            artwork.mimeType, artwork.byteSize});
    if (!sharedArtwork.succeeded())
        return failureVoid(sharedArtwork.error.category());
    if (!transaction.commit()) return failureVoid(domain::ErrorCategory::Storage);
    return {};
}

RepositoryResult<bool> SteamGameUpdatesRepository::hasArtworkFilename(
    const QString &filename) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (filename.isEmpty()) return failure<bool>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM steam_game_update_follows WHERE artwork_filename=? LIMIT 1"));
    query.addBindValue(filename);
    if (!query.exec()) return failure<bool>(queryFailure(query));
    RepositoryResult<bool> result;
    result.value = query.next();
    return result;
}

RepositoryResult<SteamGameUpdateNewsPage> SteamGameUpdatesRepository::newsPage(
    std::optional<quint32> appId, bool unreadOnly, QString titleQuery, int pageSize,
    const SteamGameUpdateNewsCursor &before,
    const std::function<bool()> &cancellationRequested) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    const bool hasCursor = before.publishedAtUnixSeconds != 0 || before.appId != 0
        || !before.newsId.isEmpty();
    if ((appId && *appId == 0) || pageSize <= 0 || pageSize > maximumNewsPageSize
        || titleQuery.size() > 120
        || (hasCursor && (!before.isValid() || !validSteamNewsId(before.newsId)))
        || (appId && hasCursor && before.appId != *appId)) {
        return failure<SteamGameUpdateNewsPage>(domain::ErrorCategory::Validation);
    }
    if (cancellationRequested && cancellationRequested())
        return failure<SteamGameUpdateNewsPage>(domain::ErrorCategory::Cancelled);

    QString sql = QStringLiteral(
        "SELECT n.steam_app_id,n.news_id,n.title,n.published_at_unix_seconds,n.source_url,"
        "n.author,n.feed_label,n.feed_name,n.feed_type,n.content_text,n.external_url,"
        "n.is_read,n.discovered_at_utc,f.name "
        "FROM steam_game_update_news_items n "
        "JOIN steam_game_update_follows f ON f.steam_app_id=n.steam_app_id WHERE 1=1");
    if (appId) sql += QStringLiteral(" AND n.steam_app_id=?");
    if (unreadOnly) sql += QStringLiteral(" AND n.is_read=0");
    if (!titleQuery.trimmed().isEmpty())
        sql += QStringLiteral(" AND n.title COLLATE NOCASE LIKE ? ESCAPE '\\'");
    if (hasCursor) {
        sql += QStringLiteral(
            " AND (n.published_at_unix_seconds<? OR "
            "(n.published_at_unix_seconds=? AND n.steam_app_id<?) OR "
            "(n.published_at_unix_seconds=? AND n.steam_app_id=? AND n.news_id<?))");
    }
    sql += QStringLiteral(
        " ORDER BY n.published_at_unix_seconds DESC,n.steam_app_id DESC,n.news_id DESC LIMIT ?");

    QSqlQuery query(m_database);
    query.prepare(sql);
    if (appId) query.addBindValue(*appId);
    if (!titleQuery.trimmed().isEmpty())
        query.addBindValue(taskHistoryTitleLikePattern(titleQuery.trimmed()));
    if (hasCursor) {
        query.addBindValue(before.publishedAtUnixSeconds);
        query.addBindValue(before.publishedAtUnixSeconds);
        query.addBindValue(before.appId);
        query.addBindValue(before.publishedAtUnixSeconds);
        query.addBindValue(before.appId);
        query.addBindValue(before.newsId);
    }
    query.addBindValue(pageSize + 1);
    if (!query.exec()) return failure<SteamGameUpdateNewsPage>(queryFailure(query));

    RepositoryResult<SteamGameUpdateNewsPage> result;
    result.value.items.reserve(pageSize);
    while (query.next()) {
        if (cancellationRequested && cancellationRequested())
            return failure<SteamGameUpdateNewsPage>(domain::ErrorCategory::Cancelled);
        if (result.value.items.size() == pageSize) {
            result.value.hasMore = true;
            break;
        }
        SteamGameUpdateNewsItem item;
        item.article.appId = query.value(0).toUInt();
        item.article.newsId = query.value(1).toString();
        item.article.title = query.value(2).toString();
        item.article.publishedAtUnixSeconds = query.value(3).toLongLong();
        item.article.sourceUrl = query.value(4).toString();
        item.article.author = query.value(5).toString();
        item.article.feedLabel = query.value(6).toString();
        item.article.feedName = query.value(7).toString();
        if (!query.value(8).isNull()) item.article.feedType = query.value(8).toInt();
        item.article.contentText = query.value(9).toString();
        item.article.externalUrl = query.value(10).toBool();
        item.isRead = query.value(11).toBool();
        item.discoveredAtUtc = query.value(12).toString();
        item.gameName = query.value(13).toString();
        result.value.items.push_back(std::move(item));
    }
    query.finish();

    if (result.value.hasMore && !result.value.items.isEmpty()) {
        const SteamGameUpdateNewsItem &last = result.value.items.last();
        result.value.nextCursor = {last.article.publishedAtUnixSeconds,
                                   last.article.appId, last.article.newsId};
    }
    return result;
}

RepositoryResult<std::optional<qint64>>
SteamGameUpdatesRepository::oldestNewsPublishedAt(quint32 appId) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0)
        return failure<std::optional<qint64>>(domain::ErrorCategory::Validation);

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT MIN(published_at_unix_seconds) FROM steam_game_update_news_items "
        "WHERE steam_app_id=?"));
    query.addBindValue(appId);
    if (!query.exec())
        return failure<std::optional<qint64>>(queryFailure(query));

    RepositoryResult<std::optional<qint64>> result;
    if (query.next() && !query.value(0).isNull())
        result.value = query.value(0).toLongLong();
    return result;
}

RepositoryResult<QHash<quint32, int>> SteamGameUpdatesRepository::unreadNewsCounts() const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT steam_app_id,COUNT(*) FROM steam_game_update_news_items "
            "WHERE is_read=0 GROUP BY steam_app_id"))) {
        return failure<QHash<quint32, int>>(queryFailure(query));
    }
    RepositoryResult<QHash<quint32, int>> result;
    while (query.next())
        result.value.insert(query.value(0).toUInt(), query.value(1).toInt());
    return result;
}

RepositoryResult<SteamGameUpdateNewsUpsertResult>
SteamGameUpdatesRepository::upsertNewsPage(
    quint32 appId, const QVector<domain::SteamNewsArticle> &articles,
    const QString &discoveredAtUtc,
    const std::function<bool()> &cancellationRequested)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0 || articles.size() > maximumNewsPageSize
        || !validUtcInstant(discoveredAtUtc)) {
        return failure<SteamGameUpdateNewsUpsertResult>(domain::ErrorCategory::Validation);
    }
    for (const domain::SteamNewsArticle &article : articles) {
        if (!validCachedSteamNewsArticle(article, appId))
            return failure<SteamGameUpdateNewsUpsertResult>(domain::ErrorCategory::Validation);
    }
    if (cancellationRequested && cancellationRequested())
        return failure<SteamGameUpdateNewsUpsertResult>(domain::ErrorCategory::Cancelled);

    QSqlQuery followQuery(m_database);
    followQuery.prepare(QStringLiteral(
        "SELECT 1 FROM steam_game_update_follows WHERE steam_app_id=? LIMIT 1"));
    followQuery.addBindValue(appId);
    if (!followQuery.exec())
        return failure<SteamGameUpdateNewsUpsertResult>(queryFailure(followQuery));
    if (!followQuery.next())
        return failure<SteamGameUpdateNewsUpsertResult>(domain::ErrorCategory::NotFound);
    followQuery.finish();

    Transaction transaction(m_database);
    if (!transaction.active())
        return failure<SteamGameUpdateNewsUpsertResult>(domain::ErrorCategory::Storage);

    SteamGameUpdateNewsUpsertResult counts;
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO steam_game_update_news_items(steam_app_id,news_id,title,"
        "published_at_unix_seconds,source_url,author,feed_label,feed_name,feed_type,"
        "content_text,external_url,is_read,discovered_at_utc) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,0,?) ON CONFLICT(steam_app_id,news_id) DO NOTHING"));
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE steam_game_update_news_items SET title=?,published_at_unix_seconds=?,"
        "source_url=?,author=?,feed_label=?,feed_name=?,feed_type=?,content_text=?,external_url=? "
        "WHERE steam_app_id=? AND news_id=?"));
    for (const domain::SteamNewsArticle &article : articles) {
        if (cancellationRequested && cancellationRequested())
            return failure<SteamGameUpdateNewsUpsertResult>(domain::ErrorCategory::Cancelled);
        const QVariant feedType = article.feedType ? QVariant(*article.feedType) : QVariant();
        insert.bindValue(0, appId);
        insert.bindValue(1, sqlText(article.newsId));
        insert.bindValue(2, sqlText(article.title));
        insert.bindValue(3, article.publishedAtUnixSeconds);
        insert.bindValue(4, sqlText(article.sourceUrl));
        insert.bindValue(5, sqlText(article.author));
        insert.bindValue(6, sqlText(article.feedLabel));
        insert.bindValue(7, sqlText(article.feedName));
        insert.bindValue(8, feedType);
        insert.bindValue(9, sqlText(article.contentText));
        insert.bindValue(10, article.externalUrl ? 1 : 0);
        insert.bindValue(11, discoveredAtUtc);
        if (!insert.exec())
            return failure<SteamGameUpdateNewsUpsertResult>(queryFailure(insert));
        const bool inserted = insert.numRowsAffected() == 1;
        insert.finish();
        if (inserted) {
            ++counts.insertedCount;
            continue;
        }

        update.bindValue(0, sqlText(article.title));
        update.bindValue(1, article.publishedAtUnixSeconds);
        update.bindValue(2, sqlText(article.sourceUrl));
        update.bindValue(3, sqlText(article.author));
        update.bindValue(4, sqlText(article.feedLabel));
        update.bindValue(5, sqlText(article.feedName));
        update.bindValue(6, feedType);
        update.bindValue(7, sqlText(article.contentText));
        update.bindValue(8, article.externalUrl ? 1 : 0);
        update.bindValue(9, appId);
        update.bindValue(10, sqlText(article.newsId));
        if (!update.exec())
            return failure<SteamGameUpdateNewsUpsertResult>(queryFailure(update));
        update.finish();
        ++counts.refreshedCount;
    }

    if (cancellationRequested && cancellationRequested())
        return failure<SteamGameUpdateNewsUpsertResult>(domain::ErrorCategory::Cancelled);

    QSqlQuery trimGame(m_database);
    trimGame.prepare(QStringLiteral(
        "DELETE FROM steam_game_update_news_items WHERE steam_app_id=? AND news_id IN ("
        "SELECT news_id FROM steam_game_update_news_items WHERE steam_app_id=? "
        "ORDER BY published_at_unix_seconds DESC,news_id DESC LIMIT -1 OFFSET ?)"));
    trimGame.addBindValue(appId);
    trimGame.addBindValue(appId);
    trimGame.addBindValue(maximumArticlesPerGame);
    if (!trimGame.exec())
        return failure<SteamGameUpdateNewsUpsertResult>(queryFailure(trimGame));
    trimGame.finish();

    QSqlQuery trimAll(m_database);
    trimAll.prepare(QStringLiteral(
            "DELETE FROM steam_game_update_news_items WHERE rowid IN ("
            "SELECT rowid FROM steam_game_update_news_items "
            "ORDER BY published_at_unix_seconds DESC,steam_app_id DESC,news_id DESC "
            "LIMIT -1 OFFSET ?)"));
    trimAll.addBindValue(maximumArticlesOverall);
    if (!trimAll.exec()) {
        return failure<SteamGameUpdateNewsUpsertResult>(queryFailure(trimAll));
    }
    trimAll.finish();
    if (cancellationRequested && cancellationRequested())
        return failure<SteamGameUpdateNewsUpsertResult>(domain::ErrorCategory::Cancelled);
    if (!transaction.commit())
        return failure<SteamGameUpdateNewsUpsertResult>(domain::ErrorCategory::Storage);

    RepositoryResult<SteamGameUpdateNewsUpsertResult> result;
    result.value = counts;
    return result;
}

RepositoryResult<void> SteamGameUpdatesRepository::setNewsRead(
    quint32 appId, const QString &newsId, bool isRead)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0 || !validSteamNewsId(newsId))
        return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery exists(m_database);
    exists.prepare(QStringLiteral(
        "SELECT 1 FROM steam_game_update_news_items WHERE steam_app_id=? AND news_id=? LIMIT 1"));
    exists.addBindValue(appId);
    exists.addBindValue(newsId);
    if (!exists.exec()) return failureVoid(queryFailure(exists));
    if (!exists.next()) return failureVoid(domain::ErrorCategory::NotFound);
    exists.finish();

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE steam_game_update_news_items SET is_read=? "
        "WHERE steam_app_id=? AND news_id=?"));
    query.addBindValue(isRead ? 1 : 0);
    query.addBindValue(appId);
    query.addBindValue(newsId);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return {};
}

RepositoryResult<int> SteamGameUpdatesRepository::setAllNewsRead(
    quint32 appId, bool isRead)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0) return failure<int>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE steam_game_update_news_items SET is_read=? "
        "WHERE steam_app_id=? AND is_read<>?"));
    query.addBindValue(isRead ? 1 : 0);
    query.addBindValue(appId);
    query.addBindValue(isRead ? 1 : 0);
    if (!query.exec()) return failure<int>(queryFailure(query));
    RepositoryResult<int> result;
    result.value = query.numRowsAffected();
    return result;
}

RepositoryResult<void> SteamGameUpdatesRepository::remove(quint32 appId)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (appId == 0) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "DELETE FROM steam_game_update_follows WHERE steam_app_id=?"));
    query.addBindValue(appId);
    if (!query.exec()) return failureVoid(queryFailure(query));
    return query.numRowsAffected() == 1 ? RepositoryResult<void>{}
                                       : failureVoid(domain::ErrorCategory::NotFound);
}

namespace {

struct SearchCandidate final {
    QVariantMap result;
    QStringList titleFields;
    QStringList categoryFields;
    QStringList descriptionFields;
};

QVariant queryValue(const QSqlQuery &query, const QString &name)
{
    const QSqlRecord record = query.record();
    const int index = record.indexOf(name);
    return index >= 0 ? record.value(index) : QVariant{};
}

QString escapedSearchText(QString text)
{
    text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    text.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    text.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return text;
}

QStringList searchVariants(const QString &text)
{
    QStringList values;
    const QString nfc = text.normalized(QString::NormalizationForm_C);
    const QString nfd = text.normalized(QString::NormalizationForm_D);
    for (const QString &form : {nfc, nfd}) {
        for (const QString &variant : {form, form.toLower(), form.toUpper(), form.toCaseFolded()}) {
            if (!variant.isEmpty() && !values.contains(variant)) values.push_back(variant);
        }
    }
    return values;
}

QVariantList actions(std::initializer_list<QPair<QString, QString>> values)
{
    QVariantList result;
    for (const auto &[id, label] : values)
        result.push_back(QVariantMap{{QStringLiteral("id"), id},
                                     {QStringLiteral("label"), label}});
    return result;
}

SearchCandidate candidate(QString id, QString type, QString group, QString title,
                          QString subtitle, QString source, QString routeId,
                          QVariant targetId, QVariant secondaryId, QString icon,
                          QVariantList resultActions, QVariantMap payload,
                          QStringList titleFields, QStringList categoryFields = {},
                          QStringList descriptionFields = {})
{
    SearchCandidate result;
    result.result = {{QStringLiteral("id"), std::move(id)},
                     {QStringLiteral("type"), std::move(type)},
                     {QStringLiteral("group"), std::move(group)},
                     {QStringLiteral("title"), std::move(title)},
                     {QStringLiteral("subtitle"), std::move(subtitle)},
                     {QStringLiteral("source"), std::move(source)},
                     {QStringLiteral("routeId"), std::move(routeId)},
                     {QStringLiteral("targetId"), std::move(targetId)},
                     {QStringLiteral("secondaryId"), std::move(secondaryId)},
                     {QStringLiteral("icon"), std::move(icon)},
                     {QStringLiteral("actions"), std::move(resultActions)},
                     {QStringLiteral("payload"), std::move(payload)}};
    result.titleFields = std::move(titleFields);
    result.categoryFields = std::move(categoryFields);
    result.descriptionFields = std::move(descriptionFields);
    return result;
}

QString whereForTokens(const QStringList &fields, const QStringList &tokens,
                       QVariantList *bindings)
{
    QStringList tokenClauses;
    for (const QString &token : tokens) {
        QStringList fieldClauses;
        const QStringList variants = searchVariants(token);
        for (const QString &field : fields) {
            for (const QString &variant : variants) {
                fieldClauses.push_back(field + QStringLiteral(" LIKE ? ESCAPE '\\' COLLATE NOCASE"));
                bindings->push_back(QStringLiteral("%") + escapedSearchText(variant)
                                    + QStringLiteral("%"));
            }
        }
        if (fieldClauses.isEmpty()) return QStringLiteral("0");
        tokenClauses.push_back(QStringLiteral("(") + fieldClauses.join(QStringLiteral(" OR "))
                               + QStringLiteral(")"));
    }
    return tokenClauses.join(QStringLiteral(" AND "));
}

QString orderByTitle(const QString &query, QVariantList *bindings)
{
    const QStringList variants = searchVariants(query);
    QStringList exactPlaceholders;
    QStringList prefixClauses;
    for (const QString &variant : variants) {
        exactPlaceholders.push_back(QStringLiteral("?"));
        bindings->push_back(variant);
    }
    for (const QString &variant : variants) {
        prefixClauses.push_back(QStringLiteral("title LIKE ? ESCAPE '\\' COLLATE NOCASE"));
        bindings->push_back(escapedSearchText(variant) + QLatin1Char('%'));
    }
    bindings->push_back(escapedSearchText(query).prepend(QLatin1Char('%')).append(QLatin1Char('%')));
    return QStringLiteral(" ORDER BY CASE WHEN title COLLATE NOCASE IN (%1) THEN 0 "
                          "WHEN (%2) THEN 1 "
                          "WHEN title LIKE ? ESCAPE '\\' COLLATE NOCASE THEN 2 ELSE 3 END, "
                          "title COLLATE NOCASE,target_sort LIMIT ?")
        .arg(exactPlaceholders.join(QLatin1Char(',')),
             prefixClauses.isEmpty() ? QStringLiteral("0")
                                     : prefixClauses.join(QStringLiteral(" OR ")));
}

bool collectSearchCandidates(QSqlDatabase &database, const QString &baseSql,
                             const QStringList &searchFields, const QString &query,
                             int perSourceLimit, const std::function<bool()> &cancelled,
                             QVector<SearchCandidate> *candidates,
                             const std::function<SearchCandidate(const QSqlQuery &)> &make,
                             domain::AppError *error)
{
    if (cancelled && cancelled()) {
        *error = domain::AppError::cancelled();
        return false;
    }
    const QStringList queryTerms = domain::UniversalSearch::terms(query);
    QVariantList bindings;
    const QString where = whereForTokens(searchFields, queryTerms, &bindings);
    QString sql = baseSql + QStringLiteral(" WHERE ") + where;
    sql += orderByTitle(query, &bindings);
    bindings.push_back(perSourceLimit);

    QSqlQuery statement(database);
    statement.setForwardOnly(true);
    statement.prepare(sql);
    for (const QVariant &binding : bindings) statement.addBindValue(binding);
    if (!statement.exec()) {
        *error = domain::AppError(queryFailure(statement));
        return false;
    }
    while (statement.next()) {
        if (cancelled && cancelled()) {
            *error = domain::AppError::cancelled();
            return false;
        }
        SearchCandidate item = make(statement);
        const int itemScore = domain::UniversalSearch::score(
            query, item.titleFields, item.categoryFields, item.descriptionFields);
        if (itemScore <= 0) continue;
        item.result.insert(QStringLiteral("score"), itemScore);
        candidates->push_back(std::move(item));
    }
    statement.finish();
    return true;
}

} // namespace

UniversalSearchRepository::UniversalSearchRepository(QSqlDatabase &database)
    : m_database(database)
{
}

RepositoryResult<QVariantList> UniversalSearchRepository::search(
    const QString &requestedQuery, const std::function<bool()> &cancellationRequested) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    const QString query = requestedQuery.normalized(QString::NormalizationForm_C).trimmed();
    const QStringList terms = domain::UniversalSearch::terms(query);
    if (query.isEmpty() || query.size() > domain::UniversalSearch::maximumQueryLength
        || terms.isEmpty() || terms.size() > domain::UniversalSearch::maximumTerms)
        return failure<QVariantList>(domain::ErrorCategory::Validation);

    constexpr int perSourceLimit = 32;
    QVector<SearchCandidate> found;
    domain::AppError error;
    auto collect = [&](const QString &sql, const QStringList &fields,
                       const std::function<SearchCandidate(const QSqlQuery &)> &make) {
        return collectSearchCandidates(m_database, sql, fields, query, perSourceLimit,
                                       cancellationRequested, &found, make, &error);
    };

    if (!collect(QStringLiteral(
            "SELECT t.id AS target_sort,t.title AS title,t.description,t.is_completed,"
            "t.priority,t.due_date,t.difficulty,t.revision,"
            "COALESCE((SELECT group_concat(g.name,' ') FROM task_tags x "
            "JOIN tags g ON g.id=x.tag_id WHERE x.task_id=t.id),'') AS tag_names FROM tasks t"),
            {QStringLiteral("t.title"), QStringLiteral("t.description"),
             QStringLiteral("COALESCE((SELECT group_concat(g.name,' ') FROM task_tags x "
                            "JOIN tags g ON g.id=x.tag_id WHERE x.task_id=t.id),'')")},
            [](const QSqlQuery &row) {
                const qint64 id = queryValue(row, QStringLiteral("target_sort")).toLongLong();
                const QString title = queryValue(row, QStringLiteral("title")).toString();
                const QString tagText = queryValue(row, QStringLiteral("tag_names")).toString();
                QVariantMap payload{{QStringLiteral("id"), id}, {QStringLiteral("taskId"), id},
                    {QStringLiteral("title"), title},
                    {QStringLiteral("description"), queryValue(row, QStringLiteral("description"))},
                    {QStringLiteral("completed"), queryValue(row, QStringLiteral("is_completed")).toBool()},
                    {QStringLiteral("priority"), queryValue(row, QStringLiteral("priority"))},
                    {QStringLiteral("dueDate"), queryValue(row, QStringLiteral("due_date"))},
                    {QStringLiteral("difficulty"), queryValue(row, QStringLiteral("difficulty"))},
                    {QStringLiteral("revision"), queryValue(row, QStringLiteral("revision")).toLongLong()}};
                QString status = payload.value(QStringLiteral("completed")).toBool()
                    ? QStringLiteral("Complete") : QStringLiteral("Incomplete");
                QString priority = payload.value(QStringLiteral("priority")).toString();
                if (!priority.isEmpty()) priority[0] = priority.at(0).toUpper();
                status += QStringLiteral(" · ") + priority
                    + QStringLiteral(" priority");
                const QString dueDate = payload.value(QStringLiteral("dueDate")).toString();
                if (!dueDate.isEmpty()) status += QStringLiteral(" · Due ") + dueDate;
                return candidate(QStringLiteral("task:%1").arg(id), QStringLiteral("task"),
                    QStringLiteral("tasks"), title, status, QStringLiteral("Tasks"),
                    QStringLiteral("tasks"), id, {}, QStringLiteral("✓"),
                    actions({{QStringLiteral("open"), QStringLiteral("Open task")},
                             {payload.value(QStringLiteral("completed")).toBool()
                                  ? QStringLiteral("markIncomplete") : QStringLiteral("markComplete"),
                              payload.value(QStringLiteral("completed")).toBool()
                                  ? QStringLiteral("Mark incomplete") : QStringLiteral("Mark complete")}}),
                    payload, {title}, {tagText},
                    {payload.value(QStringLiteral("description")).toString()});
            })) return failure<QVariantList>(error.category());

    if (!collect(QStringLiteral(
            "SELECT a.id AS target_sort,a.id,a.display_name AS title,a.executable_path,"
            "a.category_id,c.name AS category_name,c.description AS category_description,"
            "i.filename AS icon_filename FROM desktop_apps a "
            "LEFT JOIN application_categories c ON c.id=a.category_id "
            "LEFT JOIN desktop_app_icons i ON i.app_id=a.id"),
            {QStringLiteral("a.display_name"), QStringLiteral("a.executable_path"),
             QStringLiteral("c.name"), QStringLiteral("c.description")},
            [](const QSqlQuery &row) {
                const qint64 id = queryValue(row, QStringLiteral("id")).toLongLong();
                const QString title = queryValue(row, QStringLiteral("title")).toString();
                const QString category = queryValue(row, QStringLiteral("category_name")).toString();
                const QString path = queryValue(row, QStringLiteral("executable_path")).toString();
                const QString filename = QFileInfo(path).fileName();
                const qint64 categoryId = queryValue(row, QStringLiteral("category_id")).toLongLong();
                const QVariantMap payload{{QStringLiteral("id"), id},
                    {QStringLiteral("displayName"), title}, {QStringLiteral("executablePath"), path},
                    {QStringLiteral("categoryId"), categoryId}, {QStringLiteral("categoryName"), category},
                    {QStringLiteral("iconFilename"), queryValue(row, QStringLiteral("icon_filename"))}};
                QString subtitle = filename;
                if (!category.isEmpty()) subtitle = category + QStringLiteral(" · ") + filename;
                QVariantList available = actions({{QStringLiteral("open"), QStringLiteral("Open entry")},
                    {QStringLiteral("launch"), QStringLiteral("Launch")}});
                if (categoryId > 0)
                    available.push_back(QVariantMap{{QStringLiteral("id"), QStringLiteral("category")},
                        {QStringLiteral("label"), QStringLiteral("Open category")}});
                return candidate(QStringLiteral("desktopApplication:%1").arg(id),
                    QStringLiteral("desktopApplication"), QStringLiteral("applications"), title,
                    subtitle, QStringLiteral("Saved applications"), QStringLiteral("applications"),
                    id, categoryId, QStringLiteral("▣"), available, payload, {title},
                    {filename, category}, {path, queryValue(row, QStringLiteral("category_description")).toString()});
            })) return failure<QVariantList>(error.category());

    if (!collect(QStringLiteral(
            "SELECT s.id AS target_sort,s.id,s.name AS title,s.description,"
            "COALESCE(i.filename,'') AS icon_filename FROM setups s "
            "LEFT JOIN setup_icons i ON i.setup_id=s.id"),
            {QStringLiteral("s.name"), QStringLiteral("s.description")},
            [](const QSqlQuery &row) {
                const qint64 id = queryValue(row, QStringLiteral("id")).toLongLong();
                const QString title = queryValue(row, QStringLiteral("title")).toString();
                QVariantList appIds;
                const QVariantMap payload{{QStringLiteral("id"), id},
                    {QStringLiteral("name"), title},
                    {QStringLiteral("description"), queryValue(row, QStringLiteral("description"))},
                    {QStringLiteral("applicationIds"), appIds},
                    {QStringLiteral("iconFilename"), queryValue(row, QStringLiteral("icon_filename"))}};
                return candidate(QStringLiteral("setup:%1").arg(id), QStringLiteral("setup"),
                    QStringLiteral("setups"), title, queryValue(row, QStringLiteral("description")).toString(),
                    QStringLiteral("Setups"), QStringLiteral("setups"), id, {}, QStringLiteral("▱"),
                    actions({{QStringLiteral("open"), QStringLiteral("Open setup")},
                             {QStringLiteral("launch"), QStringLiteral("Launch setup")}}),
                    payload, {title}, {}, {queryValue(row, QStringLiteral("description")).toString()});
            })) return failure<QVariantList>(error.category());

    if (!collect(QStringLiteral(
            "SELECT g.id AS target_sort,g.id,g.steam_app_id,g.name AS title,g.price_status "
            "FROM steam_games g"), {QStringLiteral("g.name")},
            [](const QSqlQuery &row) {
                const qint64 id = queryValue(row, QStringLiteral("id")).toLongLong();
                const quint32 appId = queryValue(row, QStringLiteral("steam_app_id")).toUInt();
                const QString title = queryValue(row, QStringLiteral("title")).toString();
                const QVariantMap payload{{QStringLiteral("id"), id}, {QStringLiteral("appId"), appId},
                    {QStringLiteral("name"), title}, {QStringLiteral("priceStatus"), queryValue(row, QStringLiteral("price_status"))}};
                return candidate(QStringLiteral("steamPrice:%1").arg(appId),
                    QStringLiteral("steamPrice"), QStringLiteral("games"), title,
                    {}, QStringLiteral("Price Tracking"),
                    QStringLiteral("steam"), id, appId, QStringLiteral("◉"),
                    actions({{QStringLiteral("open"), QStringLiteral("Price history")},
                             {QStringLiteral("store"), QStringLiteral("Open Steam Store")}}),
                    payload, {title});
            })) return failure<QVariantList>(error.category());

    if (!collect(QStringLiteral(
            "SELECT r.steam_app_id AS target_sort,r.steam_app_id,r.title,r.release_date_text,"
            "r.release_status FROM steam_release_follows r"),
            {QStringLiteral("r.title"), QStringLiteral("r.release_date_text")},
            [](const QSqlQuery &row) {
                const quint32 appId = queryValue(row, QStringLiteral("steam_app_id")).toUInt();
                const QString title = queryValue(row, QStringLiteral("title")).toString();
                QString subtitle = queryValue(row, QStringLiteral("release_status")).toString()
                    .replace(QLatin1Char('_'), QLatin1Char(' '));
                const QString dateText = queryValue(row, QStringLiteral("release_date_text")).toString();
                if (!dateText.isEmpty()) subtitle += QStringLiteral(" · ") + dateText;
                const QVariantMap payload{{QStringLiteral("appId"), appId}, {QStringLiteral("title"), title}};
                return candidate(QStringLiteral("steamRelease:%1").arg(appId),
                    QStringLiteral("steamRelease"), QStringLiteral("games"), title, subtitle,
                    QStringLiteral("Release Radar"), QStringLiteral("steam"), appId, {},
                    QStringLiteral("◉"), actions({{QStringLiteral("open"), QStringLiteral("Open release")},
                        {QStringLiteral("store"), QStringLiteral("Open Steam Store")}}),
                    payload, {title}, {}, {dateText});
            })) return failure<QVariantList>(error.category());

    if (!collect(QStringLiteral(
            "SELECT g.steam_app_id AS target_sort,g.steam_app_id,g.name AS title,g.ownership_status "
            "FROM steam_game_update_follows g"), {QStringLiteral("g.name")},
            [](const QSqlQuery &row) {
                const quint32 appId = queryValue(row, QStringLiteral("steam_app_id")).toUInt();
                const QString title = queryValue(row, QStringLiteral("title")).toString();
                const QString ownership = queryValue(row, QStringLiteral("ownership_status")).toString()
                    == QStringLiteral("owned") ? QStringLiteral("Owned") : QStringLiteral("Not owned");
                const QVariantMap payload{{QStringLiteral("appId"), appId}, {QStringLiteral("name"), title}};
                return candidate(QStringLiteral("steamGameUpdates:%1").arg(appId),
                    QStringLiteral("steamGameUpdates"), QStringLiteral("games"), title,
                    ownership, QStringLiteral("Game Updates"),
                    QStringLiteral("gameUpdates"), appId, {}, QStringLiteral("◉"),
                    actions({{QStringLiteral("open"), QStringLiteral("Open game news")},
                             {QStringLiteral("store"), QStringLiteral("Open Steam Store")}}),
                    payload, {title});
            })) return failure<QVariantList>(error.category());

    if (!collect(QStringLiteral(
            "SELECT c.id AS target_sort,c.id,c.name AS title,c.description FROM steam_collections c"),
            {QStringLiteral("c.name"), QStringLiteral("c.description")},
            [](const QSqlQuery &row) {
                const qint64 id = queryValue(row, QStringLiteral("id")).toLongLong();
                const QString title = queryValue(row, QStringLiteral("title")).toString();
                const QString description = queryValue(row, QStringLiteral("description")).toString();
                const QVariantMap payload{{QStringLiteral("id"), id}, {QStringLiteral("name"), title},
                    {QStringLiteral("description"), description}};
                return candidate(QStringLiteral("steamCollection:%1").arg(id),
                    QStringLiteral("steamCollection"), QStringLiteral("games"), title,
                    QStringLiteral("Steam Collection"), QStringLiteral("Collections"),
                    QStringLiteral("steam"), id, {}, QStringLiteral("▤"),
                    actions({{QStringLiteral("open"), QStringLiteral("Open collection")}}),
                    payload, {title}, {}, {description});
            })) return failure<QVariantList>(error.category());

    if (!collect(QStringLiteral(
            "SELECT game.steam_app_id AS target_sort,game.steam_app_id,c.id AS collection_id,"
            "c.name AS collection_name,game.name AS title,game.store_url "
            "FROM steam_collection_games cg JOIN steam_collections c ON c.id=cg.collection_id "
            "JOIN steam_game_catalog game ON game.steam_app_id=cg.steam_app_id"),
            {QStringLiteral("game.name"), QStringLiteral("c.name")},
            [](const QSqlQuery &row) {
                const quint32 appId = queryValue(row, QStringLiteral("steam_app_id")).toUInt();
                const qint64 collectionId = queryValue(row, QStringLiteral("collection_id")).toLongLong();
                const QString title = queryValue(row, QStringLiteral("title")).toString();
                const QString collectionName = queryValue(row, QStringLiteral("collection_name")).toString();
                const QVariantMap payload{{QStringLiteral("appId"), appId},
                    {QStringLiteral("collectionId"), collectionId}, {QStringLiteral("name"), title},
                    {QStringLiteral("storeUrl"), queryValue(row, QStringLiteral("store_url"))}};
                return candidate(QStringLiteral("steamCollectionGame:%1:%2").arg(collectionId).arg(appId),
                    QStringLiteral("steamCollectionGame"), QStringLiteral("games"), title,
                    QStringLiteral("Collection · ") + collectionName, QStringLiteral("Collections"),
                    QStringLiteral("steam"), appId, collectionId, QStringLiteral("▤"),
                    actions({{QStringLiteral("open"), QStringLiteral("Open collection member")},
                             {QStringLiteral("store"), QStringLiteral("Open Steam Store")}}),
                    payload, {title}, {collectionName});
            })) return failure<QVariantList>(error.category());

    if (!collect(QStringLiteral(
            "SELECT c.id AS target_sort,c.id,c.name AS title,c.source FROM favorite_categories c"),
            {QStringLiteral("c.name")},
            [](const QSqlQuery &row) {
                const qint64 id = queryValue(row, QStringLiteral("id")).toLongLong();
                const QString title = queryValue(row, QStringLiteral("title")).toString();
                const QString source = queryValue(row, QStringLiteral("source")).toString();
                const QString provider = source == QStringLiteral("telegram")
                    ? QStringLiteral("Telegram") : QStringLiteral("YouTube");
                const QVariantMap payload{{QStringLiteral("categoryId"), id}, {QStringLiteral("source"), source}};
                return candidate(QStringLiteral("favoriteCategory:%1:%2").arg(source).arg(id),
                    QStringLiteral("favoriteCategory"), QStringLiteral("favorites"), title,
                    QStringLiteral("Category"), provider, source, id, source,
                    source == QStringLiteral("telegram") ? QStringLiteral("➤") : QStringLiteral("▶"),
                    actions({{QStringLiteral("open"), QStringLiteral("Open category")}}),
                    payload, {title});
            })) return failure<QVariantList>(error.category());

    if (!collect(QStringLiteral(
            "SELECT f.username AS target_sort,f.username AS source_id,f.username AS title,"
            "c.name AS category_name FROM telegram_favorites f "
            "LEFT JOIN favorite_categories c ON c.id=f.category_id"),
            {QStringLiteral("f.username"), QStringLiteral("c.name")},
            [](const QSqlQuery &row) {
                const QString username = queryValue(row, QStringLiteral("source_id")).toString();
                const QString category = queryValue(row, QStringLiteral("category_name")).toString();
                const QVariantMap payload{{QStringLiteral("source"), QStringLiteral("telegram")},
                    {QStringLiteral("sourceId"), username},
                    {QStringLiteral("externalUrl"), QStringLiteral("https://t.me/") + username}};
                const QString subtitle = category.isEmpty() ? QStringLiteral("Saved channel") : category;
                return candidate(QStringLiteral("telegramFavorite:%1").arg(username),
                    QStringLiteral("telegramFavorite"), QStringLiteral("favorites"), username,
                    subtitle, QStringLiteral("Telegram"), QStringLiteral("telegram"), username,
                    queryValue(row, QStringLiteral("category_name")), QStringLiteral("➤"),
                    actions({{QStringLiteral("open"), QStringLiteral("Open channel")},
                             {QStringLiteral("external"), QStringLiteral("Open external link")}}),
                    payload, {username}, {category});
            })) return failure<QVariantList>(error.category());

    if (!collect(QStringLiteral(
            "SELECT f.channel_id AS target_sort,f.channel_id AS source_id,"
            "COALESCE(NULLIF(f.username,''),f.channel_id) AS title,f.username,"
            "c.name AS category_name FROM youtube_favorites f "
            "LEFT JOIN favorite_categories c ON c.id=f.category_id"),
            {QStringLiteral("f.channel_id"), QStringLiteral("f.username"), QStringLiteral("c.name")},
            [](const QSqlQuery &row) {
                const QString channelId = queryValue(row, QStringLiteral("source_id")).toString();
                const QString title = queryValue(row, QStringLiteral("title")).toString();
                const QString username = queryValue(row, QStringLiteral("username")).toString();
                const QString category = queryValue(row, QStringLiteral("category_name")).toString();
                const QVariantMap payload{{QStringLiteral("source"), QStringLiteral("youtube")},
                    {QStringLiteral("sourceId"), channelId}, {QStringLiteral("username"), username},
                    {QStringLiteral("externalUrl"), QStringLiteral("https://www.youtube.com/channel/") + channelId}};
                const QString subtitle = category.isEmpty() ? QStringLiteral("Saved channel") : category;
                return candidate(QStringLiteral("youtubeFavorite:%1").arg(channelId),
                    QStringLiteral("youtubeFavorite"), QStringLiteral("favorites"), title,
                    subtitle, QStringLiteral("YouTube"), QStringLiteral("youtube"), channelId,
                    queryValue(row, QStringLiteral("category_name")), QStringLiteral("▶"),
                    actions({{QStringLiteral("open"), QStringLiteral("Open channel")},
                             {QStringLiteral("external"), QStringLiteral("Open external link")}}),
                    payload, {title}, {channelId, username, category});
            })) return failure<QVariantList>(error.category());

    if (cancellationRequested && cancellationRequested())
        return failure<QVariantList>(domain::ErrorCategory::Cancelled);

    for (SearchCandidate &item : found) {
        if (cancellationRequested && cancellationRequested())
            return failure<QVariantList>(domain::ErrorCategory::Cancelled);
        const QString type = item.result.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("setup")) {
            const qint64 setupId = item.result.value(QStringLiteral("targetId")).toLongLong();
            QSqlQuery applicationIds(m_database);
            applicationIds.prepare(QStringLiteral("SELECT app_id FROM setup_apps "
                                                   "WHERE setup_id=? ORDER BY position,app_id"));
            applicationIds.addBindValue(setupId);
            if (!applicationIds.exec()) return failure<QVariantList>(queryFailure(applicationIds));
            QVariantList orderedIds;
            while (applicationIds.next()) orderedIds.push_back(applicationIds.value(0).toLongLong());
            applicationIds.finish();
            QVariantMap payload = item.result.value(QStringLiteral("payload")).toMap();
            payload.insert(QStringLiteral("applicationIds"), orderedIds);
            item.result.insert(QStringLiteral("payload"), payload);
            continue;
        }
        if (type != QStringLiteral("task")) continue;
        const qint64 taskId = item.result.value(QStringLiteral("targetId")).toLongLong();
        QSqlQuery tags(m_database);
        tags.prepare(QStringLiteral("SELECT name FROM tags t JOIN task_tags x ON x.tag_id=t.id "
                                    "WHERE x.task_id=? ORDER BY t.name_normalized"));
        tags.addBindValue(taskId);
        if (!tags.exec()) return failure<QVariantList>(queryFailure(tags));
        QStringList tagNames;
        while (tags.next()) tagNames.push_back(tags.value(0).toString());
        tags.finish();
        QVariantList tagValues;
        for (const QString &tag : tagNames) tagValues.push_back(tag);

        QSqlQuery subtasks(m_database);
        subtasks.prepare(QStringLiteral("SELECT id,title,is_completed FROM task_subtasks "
                                        "WHERE task_id=? ORDER BY position,id"));
        subtasks.addBindValue(taskId);
        if (!subtasks.exec()) return failure<QVariantList>(queryFailure(subtasks));
        QVariantList subtaskValues;
        while (subtasks.next()) subtaskValues.push_back(QVariantMap{
            {QStringLiteral("id"), subtasks.value(0).toLongLong()},
            {QStringLiteral("title"), subtasks.value(1).toString()},
            {QStringLiteral("completed"), subtasks.value(2).toBool()}});
        subtasks.finish();
        QVariantMap payload = item.result.value(QStringLiteral("payload")).toMap();
        payload.insert(QStringLiteral("tags"), tagValues);
        payload.insert(QStringLiteral("subtasks"), subtaskValues);
        item.result.insert(QStringLiteral("payload"), payload);
        item.categoryFields = tagNames;
    }

    QVariantList results;
    for (SearchCandidate &item : found) {
        const int rank = domain::UniversalSearch::score(query, item.titleFields,
            item.categoryFields, item.descriptionFields);
        if (rank <= 0) continue;
        item.result.insert(QStringLiteral("score"), rank);
        results.push_back(item.result);
    }
    return {results, domain::AppError::none()};
}

StorageRepositories::StorageRepositories(QSqlDatabase &database)
    : tasks(database)
    , taskHistory(database)
    , taskAutomations(database)
    , scheduling(database)
    , currencies(database)
    , favorites(database)
    , news(database)
    , weather(database)
    , weatherComparison(database)
    , preferences(database)
    , applications(database)
    , setups(database)
    , wallpapers(database)
    , steamCatalog(database)
    , steamCollections(database)
    , steam(database)
    , steamReleases(database)
    , gameUpdates(database)
    , dataImport(database)
    , applicationUsage(database)
    , universalSearch(database)
{
}

} // namespace something::storage




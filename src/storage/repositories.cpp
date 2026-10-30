#include "repositories.h"

#include "../domain/task_rules.h"

#include <QDate>
#include <QDateTime>
#include <QRegularExpression>
#include <QMap>
#include <QSet>
#include <QSqlError>
#include <QSqlDriver>
#include <QSqlQuery>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <optional>

namespace something::storage {
namespace {

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

QString sqlLikePattern(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    value.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return QStringLiteral("%") + value.toCaseFolded() + QStringLiteral("%");
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

void bindAsset(QSqlQuery &query, int firstIndex, const AssetMetadata &asset)
{
    query.bindValue(firstIndex, asset.filename);
    query.bindValue(firstIndex + 1, asset.mimeType);
    query.bindValue(firstIndex + 2, asset.byteSize);
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
    if (!transaction.commit()) return failure<Task>(domain::ErrorCategory::Storage);
    return load(id);
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
    if (!transaction.commit()) return failure<Task>(domain::ErrorCategory::Storage);
    return load(id);
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
    if (!transaction.commit()) return failure<Task>(domain::ErrorCategory::Storage);
    return load(id);
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
    if (!transaction.commit()) return failure<Task>(domain::ErrorCategory::Storage);
    return load(id);
}

RepositoryResult<void> TaskRepository::remove(qint64 id, qint64 expectedRevision)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (id <= 0 || expectedRevision <= 0) return failureVoid(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM tasks WHERE id=? AND revision=?"));
    query.addBindValue(id);
    query.addBindValue(expectedRevision);
    if (!query.exec()) return failureVoid(queryFailure(query));
    if (query.numRowsAffected() != 1) {
        QSqlQuery exists(m_database);
        exists.prepare(QStringLiteral("SELECT 1 FROM tasks WHERE id=?"));
        exists.addBindValue(id);
        if (!exists.exec()) return failureVoid(queryFailure(exists));
        return failureVoid(exists.next() ? domain::ErrorCategory::Conflict : domain::ErrorCategory::NotFound);
    }
    return {};
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

PreferencesRepository::PreferencesRepository(QSqlDatabase &database) : m_database(database) {}

RepositoryResult<QString> PreferencesRepository::get(QString key) const
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    key = key.trimmed();
    static const QSet<QString> retainedKeys{
        QStringLiteral("tasks.week_start"), QStringLiteral("appearance.selected_wallpaper"),
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
    insert.addBindValue(normalizedName(name));
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
    update.addBindValue(normalizedName(name));
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
        insert.addBindValue(setup.description);
        if (!insert.exec()) return failure<Setup>(queryFailure(insert));
        setup.id = insert.lastInsertId().toLongLong();
    } else {
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral("UPDATE setups SET name=?,description=?,updated_at=CURRENT_TIMESTAMP WHERE id=?"));
        update.addBindValue(setup.name);
        update.addBindValue(setup.description);
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

RepositoryResult<SteamGame> SteamRepository::add(SteamGame game)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    game.name = game.name.trimmed();
    game.priceStatus = game.priceStatus.trimmed().toLower();
    game.currency = game.currency.trimmed().toUpper();
    game.priceCountryCode = game.priceCountryCode.trimmed().toUpper();
    static const QRegularExpression currencyPattern(QStringLiteral("^[A-Z]{3}$"));
    static const QRegularExpression countryPattern(QStringLiteral("^[A-Z]{2}$"));
    if (game.steamAppId <= 0 || game.storeUrl.trimmed().isEmpty() || game.name.isEmpty() || game.name.toUcs4().size() > 300
        || game.priceCountryCode.isEmpty() || !countryPattern.match(game.priceCountryCode).hasMatch()
        || (game.priceStatus != QStringLiteral("priced") && game.priceStatus != QStringLiteral("free")
            && game.priceStatus != QStringLiteral("unavailable"))
        || (!game.currency.isEmpty() && !currencyPattern.match(game.currency).hasMatch())
        || game.discountPercent < 0 || game.discountPercent > 100
        || (game.regularPriceMinor && *game.regularPriceMinor < 0)
        || (game.currentPriceMinor && *game.currentPriceMinor < 0))
        return failure<SteamGame>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO steam_games(steam_app_id,store_url,name,image_source_url,price_status,currency,regular_price_minor,current_price_minor,discount_percent,price_country_code,last_checked_at,last_attempted_at,last_refresh_error) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(game.steamAppId);
    query.addBindValue(game.storeUrl);
    query.addBindValue(game.name);
    query.addBindValue(game.imageSourceUrl);
    query.addBindValue(game.priceStatus);
    query.addBindValue(nullableString(game.currency));
    query.addBindValue(game.regularPriceMinor ? QVariant(*game.regularPriceMinor) : QVariant());
    query.addBindValue(game.currentPriceMinor ? QVariant(*game.currentPriceMinor) : QVariant());
    query.addBindValue(game.discountPercent);
    query.addBindValue(game.priceCountryCode);
    query.addBindValue(nullableString(game.lastCheckedAt));
    query.addBindValue(nullableString(game.lastAttemptedAt));
    query.addBindValue(game.lastRefreshError);
    if (!query.exec()) return failure<SteamGame>(queryFailure(query));
    return load(query.lastInsertId().toLongLong());
}

RepositoryResult<SteamGame> SteamRepository::updateRefresh(SteamGame game)
{
    Q_ASSERT(QThread::currentThread() == m_database.driver()->thread());
    if (game.id <= 0) return failure<SteamGame>(domain::ErrorCategory::Validation);
    game.name = game.name.trimmed();
    game.priceStatus = game.priceStatus.trimmed().toLower();
    game.currency = game.currency.trimmed().toUpper();
    game.priceCountryCode = game.priceCountryCode.trimmed().toUpper();
    static const QRegularExpression currencyPattern(QStringLiteral("^[A-Z]{3}$"));
    static const QRegularExpression countryPattern(QStringLiteral("^[A-Z]{2}$"));
    if (game.storeUrl.trimmed().isEmpty() || game.name.isEmpty() || game.name.toUcs4().size() > 300 || !countryPattern.match(game.priceCountryCode).hasMatch()
        || (game.priceStatus != QStringLiteral("priced") && game.priceStatus != QStringLiteral("free")
            && game.priceStatus != QStringLiteral("unavailable"))
        || (!game.currency.isEmpty() && !currencyPattern.match(game.currency).hasMatch())
        || game.discountPercent < 0 || game.discountPercent > 100
        || (game.regularPriceMinor && *game.regularPriceMinor < 0)
        || (game.currentPriceMinor && *game.currentPriceMinor < 0))
        return failure<SteamGame>(domain::ErrorCategory::Validation);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE steam_games SET store_url=?,name=?,image_source_url=?,price_status=?,currency=?,regular_price_minor=?,current_price_minor=?,discount_percent=?,price_country_code=?,last_checked_at=?,last_attempted_at=?,last_refresh_error=?,updated_at=CURRENT_TIMESTAMP WHERE id=?"));
    query.addBindValue(game.storeUrl);
    query.addBindValue(game.name);
    query.addBindValue(game.imageSourceUrl);
    query.addBindValue(game.priceStatus);
    query.addBindValue(nullableString(game.currency));
    query.addBindValue(game.regularPriceMinor ? QVariant(*game.regularPriceMinor) : QVariant());
    query.addBindValue(game.currentPriceMinor ? QVariant(*game.currentPriceMinor) : QVariant());
    query.addBindValue(game.discountPercent);
    query.addBindValue(game.priceCountryCode);
    query.addBindValue(nullableString(game.lastCheckedAt));
    query.addBindValue(nullableString(game.lastAttemptedAt));
    query.addBindValue(game.lastRefreshError);
    query.addBindValue(game.id);
    if (!query.exec()) return failure<SteamGame>(queryFailure(query));
    if (query.numRowsAffected() != 1) return failure<SteamGame>(domain::ErrorCategory::NotFound);
    return load(game.id);
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
            || game.priceStatus == QStringLiteral("unavailable");
        if (game.id != update.gameId || game.name.trimmed().isEmpty()
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
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO steam_game_images(game_id,filename,mime_type,byte_size) VALUES(?,?,?,?) ON CONFLICT(game_id) DO UPDATE SET filename=excluded.filename,mime_type=excluded.mime_type,byte_size=excluded.byte_size,updated_at=CURRENT_TIMESTAMP"));
    query.addBindValue(gameId);
    bindAsset(query, 1, asset);
    if (!query.exec()) return failureVoid(queryFailure(query));
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

StorageRepositories::StorageRepositories(QSqlDatabase &database)
    : tasks(database)
    , currencies(database)
    , favorites(database)
    , news(database)
    , weather(database)
    , preferences(database)
    , applications(database)
    , setups(database)
    , wallpapers(database)
    , steam(database)
    , dataImport(database)
{
}

} // namespace something::storage




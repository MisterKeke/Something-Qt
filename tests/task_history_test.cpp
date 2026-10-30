#include "domain/task_history.h"
#include "storage/repositories.h"
#include "storage/schema_migrator.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QPair>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QVector>
#include <QtTest>

#include <utility>

using namespace something;

namespace {

class DatabaseFixture final {
public:
    DatabaseFixture()
        : connectionName(QStringLiteral("task-history-test-%1")
                             .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
        , database(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName))
    {
        database.setDatabaseName(QStringLiteral(":memory:"));
        opened = database.open() && storage::SchemaMigrator::apply(database);
    }

    ~DatabaseFixture()
    {
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }

    QString connectionName;
    QSqlDatabase database;
    bool opened{false};
};

const domain::TaskHistoryChange *changeFor(
    const domain::TaskHistoryEvent &event, const QString &field)
{
    for (const auto &change : event.changes) {
        if (change.field == field) return &change;
    }
    return nullptr;
}

const domain::TaskHistoryEvent *eventOfType(
    const storage::TaskHistoryPage &page, const QString &type)
{
    for (const auto &event : page.events) {
        if (domain::taskHistoryEventTypeName(event.type) == type) return &event;
    }
    return nullptr;
}

const domain::TaskHistoryEvent *eventAtRevision(
    const storage::TaskHistoryPage &page, qint64 revision)
{
    for (const auto &event : page.events) {
        if (event.taskRevision && event.taskRevision.value() == revision) return &event;
    }
    return nullptr;
}

storage::TaskInput basicTask(QString title)
{
    storage::TaskInput input;
    input.title = std::move(title);
    input.priority = QStringLiteral("medium");
    return input;
}

} // namespace

class TaskHistoryTest final : public QObject {
    Q_OBJECT

private slots:
    void serializedDiffPreservesNullEmptyUnsetAndUnicode();
    void taskCreationUpdatesNoOpsAndRevisionConflictsAreRecordedCorrectly();
    void singleFieldMutationsUseSpecificEventTypes();
    void parentCompletionProducesCompletedAndReopenedEvents();
    void failedHistoryInsertRollsBackTaskMutation();
    void subtaskLifecycleProducesOneReadableEventPerOperation();
    void deletionKeepsHistoricalRowsAndTitle();
    void recentHistoryFiltersAndPaginatesWithStableOrdering();
    void malformedDiffIsSkippedAndPaginationAdvances();
    void versionEightDatabaseMigratesWithoutFabricatedHistory();
};

void TaskHistoryTest::serializedDiffPreservesNullEmptyUnsetAndUnicode()
{
    QVector<domain::TaskHistoryChange> changes{
        {QStringLiteral("title"), domain::TaskHistoryValue::unset(),
         domain::TaskHistoryValue::fromJsonValue(QJsonValue(QStringLiteral("東京 🧭")))},
        {QStringLiteral("description"),
         domain::TaskHistoryValue::fromJsonValue(QJsonValue(QStringLiteral("Earlier"))),
         domain::TaskHistoryValue::fromJsonValue(QJsonValue(QStringLiteral("")))},
        {QStringLiteral("dueDate"), domain::TaskHistoryValue::null(),
         domain::TaskHistoryValue::fromJsonValue(QJsonValue(QStringLiteral("2026-11-07")))}
    };

    const auto serialized = domain::serializeTaskHistoryDiff(changes);
    QVERIFY(serialized.has_value());
    QCOMPARE(serialized->version, domain::taskHistorySerializationVersion);

    const auto decoded = domain::deserializeTaskHistoryDiff(
        serialized->version, serialized->changedFieldsJson,
        serialized->previousValuesJson, serialized->newValuesJson);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->size(), 3);
    QVERIFY(decoded->at(0).previous.state == domain::TaskHistoryValue::State::Unset);
    QCOMPARE(decoded->at(0).current.value.toString(), QStringLiteral("東京 🧭"));
    QVERIFY(decoded->at(1).previous.state == domain::TaskHistoryValue::State::Value);
    QCOMPARE(decoded->at(1).current.value.toString(), QStringLiteral(""));
    QVERIFY(decoded->at(2).previous.state == domain::TaskHistoryValue::State::Null);
    QCOMPARE(decoded->at(2).current.value.toString(), QStringLiteral("2026-11-07"));

    QVERIFY(!domain::deserializeTaskHistoryDiff(
        serialized->version + 1, serialized->changedFieldsJson,
        serialized->previousValuesJson, serialized->newValuesJson).has_value());
    QVERIFY(!domain::deserializeTaskHistoryDiff(
        serialized->version, QStringLiteral("[]"), QStringLiteral("{}"),
        QStringLiteral("{}")).has_value());
}

void TaskHistoryTest::taskCreationUpdatesNoOpsAndRevisionConflictsAreRecordedCorrectly()
{
    DatabaseFixture fixture;
    QVERIFY(fixture.opened);
    storage::StorageRepositories repositories(fixture.database);

    storage::TaskInput original = basicTask(QStringLiteral("Plan café 🧭"));
    original.description = QStringLiteral("Draft résumé 😀");
    original.dueDate.clear();
    original.difficulty = QStringLiteral("easy");
    original.tags = QStringList{QStringLiteral("mañana"), QStringLiteral("готово")};
    const auto created = repositories.tasks.create(original);
    QVERIFY(created.succeeded());

    storage::TaskInput replacement = original;
    replacement.title = QStringLiteral("计划 東京 🧪");
    replacement.description.clear();
    replacement.dueDate = QStringLiteral("2026-11-07");
    replacement.priority = QStringLiteral("high");
    replacement.difficulty = QStringLiteral("hard");
    replacement.tags = QStringList{QStringLiteral("überarbeitet")};
    const auto updated = repositories.tasks.update(
        created.value.id, created.value.revision, replacement);
    QVERIFY(updated.succeeded());

    storage::TaskInput stale = replacement;
    stale.title = QStringLiteral("Stale write");
    const auto conflict = repositories.tasks.update(
        created.value.id, created.value.revision, stale);
    QVERIFY(!conflict.succeeded());
    QVERIFY(conflict.error.category() == domain::ErrorCategory::Conflict);

    const auto noOp = repositories.tasks.update(
        created.value.id, updated.value.revision, replacement);
    QVERIFY(noOp.succeeded());

    const auto page = repositories.taskHistory.forTask(
        created.value.id, std::nullopt, 10);
    QVERIFY(page.succeeded());
    QCOMPARE(page.value.events.size(), 2);
    const auto *updatedEvent = eventOfType(page.value, QStringLiteral("task_updated"));
    const auto *createdEvent = eventOfType(page.value, QStringLiteral("task_created"));
    QVERIFY(updatedEvent);
    QVERIFY(createdEvent);
    QCOMPARE(updatedEvent->changes.size(), 6);
    QVERIFY(updatedEvent->taskRevision.has_value());
    QCOMPARE(updatedEvent->taskRevision.value(), updated.value.revision);
    QVERIFY(updatedEvent->occurredAtUtc.endsWith(QLatin1Char('Z')));

    const auto *titleChange = changeFor(*updatedEvent, QStringLiteral("title"));
    QVERIFY(titleChange);
    QCOMPARE(titleChange->previous.value.toString(), QStringLiteral("Plan café 🧭"));
    QCOMPARE(titleChange->current.value.toString(), QStringLiteral("计划 東京 🧪"));

    const auto *descriptionChange = changeFor(
        *updatedEvent, QStringLiteral("description"));
    QVERIFY(descriptionChange);
    QCOMPARE(descriptionChange->previous.value.toString(), QStringLiteral("Draft résumé 😀"));
    QVERIFY(descriptionChange->current.state == domain::TaskHistoryValue::State::Value);
    QCOMPARE(descriptionChange->current.value.toString(), QStringLiteral(""));

    const auto *dueDateChange = changeFor(*updatedEvent, QStringLiteral("dueDate"));
    QVERIFY(dueDateChange);
    QVERIFY(dueDateChange->previous.state == domain::TaskHistoryValue::State::Null);
    QCOMPARE(dueDateChange->current.value.toString(), QStringLiteral("2026-11-07"));

    const auto *priorityChange = changeFor(*updatedEvent, QStringLiteral("priority"));
    QVERIFY(priorityChange);
    QCOMPARE(priorityChange->previous.value.toString(), QStringLiteral("medium"));
    QCOMPARE(priorityChange->current.value.toString(), QStringLiteral("high"));
    const auto *difficultyChange = changeFor(*updatedEvent, QStringLiteral("difficulty"));
    QVERIFY(difficultyChange);
    QCOMPARE(difficultyChange->previous.value.toString(), QStringLiteral("easy"));
    QCOMPARE(difficultyChange->current.value.toString(), QStringLiteral("hard"));
    const auto *tagsChange = changeFor(*updatedEvent, QStringLiteral("tags"));
    QVERIFY(tagsChange);
    QCOMPARE(tagsChange->previous.value.toArray().size(), 2);
    QCOMPARE(tagsChange->current.value.toArray().front().toString(), QStringLiteral("überarbeitet"));
    QVERIFY(!changeFor(*updatedEvent, QStringLiteral("completed")));

    const auto *createdTitle = changeFor(*createdEvent, QStringLiteral("title"));
    QVERIFY(createdTitle);
    QVERIFY(createdTitle->previous.state == domain::TaskHistoryValue::State::Unset);
    QCOMPARE(createdTitle->current.value.toString(), QStringLiteral("Plan café 🧭"));
    const auto *createdDueDate = changeFor(*createdEvent, QStringLiteral("dueDate"));
    QVERIFY(createdDueDate);
    QVERIFY(createdDueDate->current.state == domain::TaskHistoryValue::State::Null);
}

void TaskHistoryTest::failedHistoryInsertRollsBackTaskMutation()
{
    DatabaseFixture fixture;
    QVERIFY(fixture.opened);
    storage::StorageRepositories repositories(fixture.database);

    QSqlQuery blocker(fixture.database);
    QVERIFY(blocker.exec(QStringLiteral(
        "CREATE TRIGGER reject_task_history BEFORE INSERT ON task_history "
        "BEGIN SELECT RAISE(ABORT, 'fixture history failure'); END")));

    const auto rejectedCreate = repositories.tasks.create(basicTask(QStringLiteral("Atomic create")));
    QVERIFY(!rejectedCreate.succeeded());
    QVERIFY(blocker.exec(QStringLiteral("SELECT COUNT(*) FROM tasks")));
    QVERIFY(blocker.next());
    QCOMPARE(blocker.value(0).toInt(), 0);

    QVERIFY(blocker.exec(QStringLiteral("DROP TRIGGER reject_task_history")));
    const auto created = repositories.tasks.create(basicTask(QStringLiteral("Stable task")));
    QVERIFY(created.succeeded());
    QVERIFY(blocker.exec(QStringLiteral(
        "CREATE TRIGGER reject_task_history BEFORE INSERT ON task_history "
        "BEGIN SELECT RAISE(ABORT, 'fixture history failure'); END")));

    storage::TaskInput changed = basicTask(QStringLiteral("Must roll back"));
    const auto rejectedUpdate = repositories.tasks.update(
        created.value.id, created.value.revision, changed);
    QVERIFY(!rejectedUpdate.succeeded());
    const auto unchanged = repositories.tasks.get(created.value.id);
    QVERIFY(unchanged.succeeded());
    QCOMPARE(unchanged.value.title, QStringLiteral("Stable task"));
    QCOMPARE(unchanged.value.revision, created.value.revision);

    const auto history = repositories.taskHistory.forTask(
        created.value.id, std::nullopt, 10);
    QVERIFY(history.succeeded());
    QCOMPARE(history.value.events.size(), 1);
    QCOMPARE(domain::taskHistoryEventTypeName(history.value.events.front().type),
             QStringLiteral("task_created"));
}

void TaskHistoryTest::parentCompletionProducesCompletedAndReopenedEvents()
{
    DatabaseFixture fixture;
    QVERIFY(fixture.opened);
    storage::StorageRepositories repositories(fixture.database);

    const auto created = repositories.tasks.create(basicTask(QStringLiteral("Finish report")));
    QVERIFY(created.succeeded());
    const auto completed = repositories.tasks.setCompleted(
        created.value.id, created.value.revision, true);
    QVERIFY(completed.succeeded());

    const auto repeatedCompletion = repositories.tasks.setCompleted(
        created.value.id, completed.value.revision, true);
    QVERIFY(repeatedCompletion.succeeded());
    const auto staleReopen = repositories.tasks.setCompleted(
        created.value.id, completed.value.revision, false);
    QVERIFY(!staleReopen.succeeded());
    QVERIFY(staleReopen.error.category() == domain::ErrorCategory::Conflict);

    const auto reopened = repositories.tasks.setCompleted(
        created.value.id, repeatedCompletion.value.revision, false);
    QVERIFY(reopened.succeeded());

    const auto page = repositories.taskHistory.forTask(
        created.value.id, std::nullopt, 10);
    QVERIFY(page.succeeded());
    QCOMPARE(page.value.events.size(), 3);
    const auto *completedEvent = eventOfType(page.value, QStringLiteral("task_completed"));
    const auto *reopenedEvent = eventOfType(page.value, QStringLiteral("task_reopened"));
    QVERIFY(completedEvent);
    QVERIFY(reopenedEvent);
    const auto *completedChange = changeFor(*completedEvent, QStringLiteral("completed"));
    const auto *reopenedChange = changeFor(*reopenedEvent, QStringLiteral("completed"));
    QVERIFY(completedChange);
    QVERIFY(reopenedChange);
    QVERIFY(!completedChange->previous.value.toBool());
    QVERIFY(completedChange->current.value.toBool());
    QVERIFY(reopenedChange->previous.value.toBool());
    QVERIFY(!reopenedChange->current.value.toBool());
    QCOMPARE(completedEvent->taskRevision.value(), completed.value.revision);
    QCOMPARE(reopenedEvent->taskRevision.value(), reopened.value.revision);
}

void TaskHistoryTest::singleFieldMutationsUseSpecificEventTypes()
{
    DatabaseFixture fixture;
    QVERIFY(fixture.opened);
    storage::StorageRepositories repositories(fixture.database);

    const auto created = repositories.tasks.create(basicTask(QStringLiteral("Initial title")));
    QVERIFY(created.succeeded());
    qint64 revision = created.value.revision;

    storage::TaskInput titleChange = basicTask(QStringLiteral("Updated title"));
    auto updated = repositories.tasks.update(created.value.id, revision, titleChange);
    QVERIFY(updated.succeeded());
    revision = updated.value.revision;

    storage::TaskInput descriptionChange = basicTask(QStringLiteral("Updated title"));
    descriptionChange.description = QStringLiteral("説明 and notes");
    updated = repositories.tasks.update(created.value.id, revision, descriptionChange);
    QVERIFY(updated.succeeded());
    revision = updated.value.revision;

    storage::TaskInput dueDateChange = descriptionChange;
    dueDateChange.dueDate = QStringLiteral("2026-11-09");
    updated = repositories.tasks.update(created.value.id, revision, dueDateChange);
    QVERIFY(updated.succeeded());
    revision = updated.value.revision;

    storage::TaskInput priorityChange = dueDateChange;
    priorityChange.priority = QStringLiteral("high");
    updated = repositories.tasks.update(created.value.id, revision, priorityChange);
    QVERIFY(updated.succeeded());
    revision = updated.value.revision;

    storage::TaskInput difficultyChange = priorityChange;
    difficultyChange.difficulty = QStringLiteral("easy");
    updated = repositories.tasks.update(created.value.id, revision, difficultyChange);
    QVERIFY(updated.succeeded());
    revision = updated.value.revision;

    storage::TaskInput tagsChange = difficultyChange;
    tagsChange.tags = QStringList{QStringLiteral("日本語")};
    updated = repositories.tasks.update(created.value.id, revision, tagsChange);
    QVERIFY(updated.succeeded());

    const auto page = repositories.taskHistory.forTask(
        created.value.id, std::nullopt, 10);
    QVERIFY(page.succeeded());
    QCOMPARE(page.value.events.size(), 7);
    const QVector<QPair<qint64, QString>> expectedTypes{
        {1, QStringLiteral("task_created")},
        {2, QStringLiteral("task_title_changed")},
        {3, QStringLiteral("description_changed")},
        {4, QStringLiteral("due_date_changed")},
        {5, QStringLiteral("priority_changed")},
        {6, QStringLiteral("difficulty_changed")},
        {7, QStringLiteral("tags_changed")}
    };
    for (const auto &expected : expectedTypes) {
        const auto *event = eventAtRevision(page.value, expected.first);
        QVERIFY(event);
        QCOMPARE(domain::taskHistoryEventTypeName(event->type), expected.second);
        QCOMPARE(event->changes.size(), 1);
    }
}

void TaskHistoryTest::subtaskLifecycleProducesOneReadableEventPerOperation()
{
    DatabaseFixture fixture;
    QVERIFY(fixture.opened);
    storage::StorageRepositories repositories(fixture.database);

    storage::TaskInput original = basicTask(QStringLiteral("Documentation task"));
    original.difficulty = QStringLiteral("hard");
    const auto created = repositories.tasks.create(original);
    QVERIFY(created.succeeded());

    storage::TaskInput addSubtask = original;
    addSubtask.subtasks = QVector<storage::SubtaskInput>{{0, QStringLiteral("Write documentation"), false}};
    const auto added = repositories.tasks.update(
        created.value.id, created.value.revision, addSubtask);
    QVERIFY(added.succeeded());
    QCOMPARE(added.value.subtasks.size(), 1);
    const qint64 subtaskId = added.value.subtasks.front().id;

    storage::TaskInput renameSubtask = original;
    renameSubtask.subtasks = QVector<storage::SubtaskInput>{
        {subtaskId, QStringLiteral("Write user documentation"), false}};
    const auto renamed = repositories.tasks.update(
        created.value.id, added.value.revision, renameSubtask);
    QVERIFY(renamed.succeeded());

    const auto completed = repositories.tasks.setSubtaskCompleted(
        created.value.id, subtaskId, renamed.value.revision, true);
    QVERIFY(completed.succeeded());
    const auto reopened = repositories.tasks.setSubtaskCompleted(
        created.value.id, subtaskId, completed.value.revision, false);
    QVERIFY(reopened.succeeded());

    storage::TaskInput removeSubtask = original;
    removeSubtask.subtasks = QVector<storage::SubtaskInput>{};
    const auto removed = repositories.tasks.update(
        created.value.id, reopened.value.revision, removeSubtask);
    QVERIFY(removed.succeeded());

    const auto page = repositories.taskHistory.forTask(
        created.value.id, std::nullopt, 10);
    QVERIFY(page.succeeded());
    QCOMPARE(page.value.events.size(), 6);
    QHash<qint64, QString> typeByRevision;
    for (const auto &event : page.value.events) {
        QVERIFY(event.taskRevision.has_value());
        typeByRevision.insert(event.taskRevision.value(),
                              domain::taskHistoryEventTypeName(event.type));
    }
    QCOMPARE(typeByRevision.size(), 6);
    QCOMPARE(typeByRevision.value(1), QStringLiteral("task_created"));
    QCOMPARE(typeByRevision.value(2), QStringLiteral("subtask_created"));
    QCOMPARE(typeByRevision.value(3), QStringLiteral("subtask_updated"));
    QCOMPARE(typeByRevision.value(4), QStringLiteral("subtask_completed"));
    QCOMPARE(typeByRevision.value(5), QStringLiteral("subtask_reopened"));
    QCOMPARE(typeByRevision.value(6), QStringLiteral("subtask_removed"));

    const auto *completionEvent = eventOfType(page.value, QStringLiteral("subtask_completed"));
    QVERIFY(completionEvent);
    const auto *completion = changeFor(
        *completionEvent, QStringLiteral("subtasks.%1").arg(subtaskId));
    QVERIFY(completion);
    QVERIFY(!completion->previous.value.toObject().value(QStringLiteral("completed")).toBool());
    QVERIFY(completion->current.value.toObject().value(QStringLiteral("completed")).toBool());
    QCOMPARE(completion->current.value.toObject().value(QStringLiteral("title")).toString(),
             QStringLiteral("Write user documentation"));

    const auto *removedEvent = eventOfType(page.value, QStringLiteral("subtask_removed"));
    QVERIFY(removedEvent);
    const auto *removedSubtasks = changeFor(*removedEvent, QStringLiteral("subtasks"));
    QVERIFY(removedSubtasks);
    QCOMPARE(removedSubtasks->previous.value.toArray().front().toObject()
                 .value(QStringLiteral("title")).toString(),
             QStringLiteral("Write user documentation"));
}

void TaskHistoryTest::deletionKeepsHistoricalRowsAndTitle()
{
    DatabaseFixture fixture;
    QVERIFY(fixture.opened);
    storage::StorageRepositories repositories(fixture.database);

    const auto created = repositories.tasks.create(basicTask(QStringLiteral("Retained title Ω")));
    QVERIFY(created.succeeded());
    QVERIFY(repositories.tasks.remove(created.value.id, created.value.revision).succeeded());
    QCOMPARE(repositories.tasks.get(created.value.id).error.category(),
             domain::ErrorCategory::NotFound);

    const auto taskHistory = repositories.taskHistory.forTask(
        created.value.id, std::nullopt, 10);
    QVERIFY(taskHistory.succeeded());
    QCOMPARE(taskHistory.value.events.size(), 2);
    const auto *deletedEvent = eventOfType(taskHistory.value, QStringLiteral("task_deleted"));
    QVERIFY(deletedEvent);
    QCOMPARE(deletedEvent->taskTitle, QStringLiteral("Retained title Ω"));
    QCOMPARE(deletedEvent->taskId, created.value.id);

    const auto globalHistory = repositories.taskHistory.recent(
        QStringLiteral("Retained title Ω"), std::nullopt, 10);
    QVERIFY(globalHistory.succeeded());
    QCOMPARE(globalHistory.value.events.size(), 2);
}

void TaskHistoryTest::recentHistoryFiltersAndPaginatesWithStableOrdering()
{
    DatabaseFixture fixture;
    QVERIFY(fixture.opened);
    storage::StorageRepositories repositories(fixture.database);

    QVector<qint64> taskIds;
    for (int index = 0; index < 5; ++index) {
        const auto created = repositories.tasks.create(
            basicTask(index == 3 ? QStringLiteral("Tarea Ωmega 3")
                                 : QStringLiteral("Batch task %1").arg(index)));
        QVERIFY(created.succeeded());
        taskIds.push_back(created.value.id);
    }

    storage::TaskInput changePriority = basicTask(QStringLiteral("Batch task 2"));
    changePriority.priority = QStringLiteral("high");
    QVERIFY(repositories.tasks.update(taskIds.at(2), 1, changePriority).succeeded());

    QVector<domain::TaskHistoryEvent> events;
    auto page = repositories.taskHistory.recent({}, std::nullopt, 2);
    QVERIFY(page.succeeded());
    int pageCount = 0;
    while (true) {
        ++pageCount;
        QVERIFY(page.succeeded());
        QVERIFY(page.value.events.size() <= 2);
        for (const auto &event : page.value.events) events.push_back(event);
        if (!page.value.hasMore) break;
        QVERIFY(page.value.nextCursor.hasValue());
        page = repositories.taskHistory.recent(
            {}, std::nullopt, 2, page.value.nextCursor);
        QVERIFY(pageCount < 10);
    }
    QCOMPARE(events.size(), 6);
    QCOMPARE(pageCount, 3);
    for (int index = 1; index < events.size(); ++index) {
        const auto &newer = events.at(index - 1);
        const auto &older = events.at(index);
        QVERIFY(newer.occurredAtUtc > older.occurredAtUtc
                || (newer.occurredAtUtc == older.occurredAtUtc
                    && newer.eventId > older.eventId));
    }

    const auto titleAndTypeFilter = repositories.taskHistory.recent(
        QStringLiteral("Batch task 2"), domain::TaskHistoryEventType::PriorityChanged, 10);
    QVERIFY(titleAndTypeFilter.succeeded());
    QCOMPARE(titleAndTypeFilter.value.events.size(), 1);
    QCOMPARE(domain::taskHistoryEventTypeName(titleAndTypeFilter.value.events.front().type),
             QStringLiteral("priority_changed"));

    const auto unicodeTitleFilter = repositories.taskHistory.recent(
        QStringLiteral("Tarea Ωmega"), domain::TaskHistoryEventType::TaskCreated, 10);
    QVERIFY(unicodeTitleFilter.succeeded());
    QCOMPARE(unicodeTitleFilter.value.events.size(), 1);
    QCOMPARE(unicodeTitleFilter.value.events.front().taskTitle, QStringLiteral("Tarea Ωmega 3"));
}

void TaskHistoryTest::malformedDiffIsSkippedAndPaginationAdvances()
{
    DatabaseFixture fixture;
    QVERIFY(fixture.opened);
    storage::StorageRepositories repositories(fixture.database);
    const auto created = repositories.tasks.create(basicTask(QStringLiteral("Valid event")));
    QVERIFY(created.succeeded());

    QSqlQuery corrupt(fixture.database);
    QVERIFY(corrupt.prepare(QStringLiteral(
        "INSERT INTO task_history(task_id,event_type,occurred_at_utc,task_revision,task_title,"
        "serialization_version,changed_fields_json,previous_values_json,new_values_json) "
        "VALUES(?,?,?,?,?,?,?,?,?)")));
    corrupt.addBindValue(created.value.id);
    corrupt.addBindValue(QStringLiteral("task_updated"));
    corrupt.addBindValue(QStringLiteral("9999-12-31T23:59:59.000Z"));
    corrupt.addBindValue(created.value.revision);
    corrupt.addBindValue(QStringLiteral("Valid event"));
    corrupt.addBindValue(domain::taskHistorySerializationVersion + 1);
    corrupt.addBindValue(QStringLiteral("[]"));
    corrupt.addBindValue(QStringLiteral("{}"));
    corrupt.addBindValue(QStringLiteral("{}"));
    QVERIFY(corrupt.exec());

    const auto first = repositories.taskHistory.recent({}, std::nullopt, 1);
    QVERIFY(first.succeeded());
    QVERIFY(first.value.events.isEmpty());
    QCOMPARE(first.value.invalidEventsSkipped, 1);
    QVERIFY(first.value.hasMore);
    QVERIFY(first.value.nextCursor.hasValue());

    const auto next = repositories.taskHistory.recent(
        {}, std::nullopt, 1, first.value.nextCursor);
    QVERIFY(next.succeeded());
    QCOMPARE(next.value.invalidEventsSkipped, 0);
    QCOMPARE(next.value.events.size(), 1);
    QCOMPARE(domain::taskHistoryEventTypeName(next.value.events.front().type),
             QStringLiteral("task_created"));
}

void TaskHistoryTest::versionEightDatabaseMigratesWithoutFabricatedHistory()
{
    DatabaseFixture fixture;
    QVERIFY(fixture.opened);
    storage::StorageRepositories repositories(fixture.database);
    storage::TaskInput legacyInput = basicTask(QStringLiteral("Existing task café"));
    legacyInput.description = QStringLiteral("Keep this row");
    const auto legacyTask = repositories.tasks.create(legacyInput);
    QVERIFY(legacyTask.succeeded());

    QSqlQuery migration(fixture.database);
    QVERIFY(migration.exec(QStringLiteral("DROP TABLE task_history")));
    QVERIFY(migration.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version=9")));
    QVERIFY(migration.exec(QStringLiteral("SELECT MAX(version) FROM schema_migrations")));
    QVERIFY(migration.next());
    QCOMPARE(migration.value(0).toInt(), 8);
    migration.finish();

    QString diagnostic;
    QVERIFY2(storage::SchemaMigrator::apply(fixture.database, &diagnostic),
             qPrintable(diagnostic));
    QCOMPARE(storage::SchemaMigrator::latestVersion(), 9);

    const auto preserved = repositories.tasks.get(legacyTask.value.id);
    QVERIFY(preserved.succeeded());
    QCOMPARE(preserved.value.title, QStringLiteral("Existing task café"));
    QCOMPARE(preserved.value.description, QStringLiteral("Keep this row"));
    QCOMPARE(preserved.value.revision, legacyTask.value.revision);

    const auto history = repositories.taskHistory.forTask(
        legacyTask.value.id, std::nullopt, 10);
    QVERIFY(history.succeeded());
    QVERIFY(history.value.events.isEmpty());

    QVERIFY(migration.exec(QStringLiteral("PRAGMA foreign_key_list(task_history)")));
    QVERIFY(!migration.next());
}

QTEST_GUILESS_MAIN(TaskHistoryTest)
#include "task_history_test.moc"

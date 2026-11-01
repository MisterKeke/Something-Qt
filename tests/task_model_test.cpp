#include "../src/models/tasks_model.h"

#include <QFile>
#include <QSignalSpy>
#include <QtTest>

using namespace something::domain;
using something::models::TasksModel;

class TaskModelTest final : public QObject {
    Q_OBJECT

private slots:
    void mutationNotifiesOnlyAffectedRow();
    void mutationReconcilesPageWithRowSignals();
    void removalAndPageReplacementStayConsistent();
    void identicalPageDoesNotResetModel();
    void taskAutomationRoleIsPresentForConfiguredAndUnconfiguredTasks();
    void taskEditDelegateDeclaresTheAutomationRoleItUses();
};

void TaskModelTest::mutationNotifiesOnlyAffectedRow()
{
    TasksModel model;
    Task first;
    first.id = 1;
    first.title = QStringLiteral("First");
    first.revision = 1;
    Task second;
    second.id = 2;
    second.title = QStringLiteral("Second");
    second.revision = 1;
    model.replacePage({first, second});

    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    second.title = QStringLiteral("Updated second");
    second.revision = 2;
    QVERIFY(model.applyTask(second));
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.at(0).at(0).value<QModelIndex>().row(), 1);
    QCOMPARE(model.data(model.index(1, 0), TasksModel::TitleRole).toString(),
             QStringLiteral("Updated second"));
    QCOMPARE(model.data(model.index(0, 0), TasksModel::TitleRole).toString(),
             QStringLiteral("First"));
}

void TaskModelTest::mutationReconcilesPageWithRowSignals()
{
    TasksModel model;
    Task first;
    first.id = 1;
    first.title = QStringLiteral("First");
    Task second;
    second.id = 2;
    second.title = QStringLiteral("Second");
    model.replacePage({first, second});

    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    second.title = QStringLiteral("Second updated");
    second.revision = 2;
    model.reconcilePage({second, first});

    QCOMPARE(reset.size(), 0);
    QCOMPARE(moved.size(), 1);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(model.taskAt(0)->id, qint64(2));
    QCOMPARE(model.taskAt(0)->title, QStringLiteral("Second updated"));
}

void TaskModelTest::removalAndPageReplacementStayConsistent()
{
    TasksModel model;
    Task task;
    task.id = 41;
    task.title = QStringLiteral("Due yesterday");
    const QDate today(2026, 10, 19);
    model.setToday(today);
    task.dueDate = today.addDays(-1).toString(Qt::ISODate);
    model.replacePage({task});
    QCOMPARE(model.data(model.index(0, 0), TasksModel::OverdueRole).toBool(), true);

    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QVERIFY(model.removeTask(41));
    QCOMPARE(removed.size(), 1);
    QCOMPARE(model.count(), 0);

    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    model.replacePage({task});
    QCOMPARE(reset.size(), 1);
    QCOMPARE(model.count(), 1);
}

void TaskModelTest::identicalPageDoesNotResetModel()
{
    TasksModel model;
    Task task;
    task.id = 19;
    task.title = QStringLiteral("Stable task");
    task.revision = 4;
    model.replacePage({task});

    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    model.replacePage({task});

    QCOMPARE(reset.size(), 0);
    QCOMPARE(model.count(), 1);
    QCOMPARE(model.taskAt(0)->revision, qint64(4));
}

void TaskModelTest::taskAutomationRoleIsPresentForConfiguredAndUnconfiguredTasks()
{
    TasksModel model;
    Task ordinary;
    ordinary.id = 1;
    ordinary.title = QStringLiteral("Ordinary task");
    Task automated;
    automated.id = 2;
    automated.title = QStringLiteral("Automated task");
    TaskAutomation automation;
    automation.id = 8;
    automation.taskId = 2;
    automation.enabled = true;
    automation.actionType = QStringLiteral("show_notification");
    automation.notificationMessage = QStringLiteral("Done: {task.title}");
    automated.automation = automation;
    model.replacePage({ordinary, automated});

    const auto roles = model.roleNames();
    QVERIFY(roles.contains(TasksModel::AutomationRole));
    QCOMPARE(roles.value(TasksModel::AutomationRole), QByteArrayLiteral("automation"));
    QVERIFY(model.data(model.index(0, 0), TasksModel::AutomationRole).toMap().isEmpty());
    const QVariantMap configured = model.data(model.index(1, 0), TasksModel::AutomationRole).toMap();
    QVERIFY(configured.value(QStringLiteral("configured")).toBool());
    QVERIFY(configured.value(QStringLiteral("enabled")).toBool());
    QCOMPARE(configured.value(QStringLiteral("actionType")).toString(),
             QStringLiteral("show_notification"));
}

void TaskModelTest::taskEditDelegateDeclaresTheAutomationRoleItUses()
{
    QFile taskPage(QStringLiteral(SOMETHING_QT_SOURCE_DIR "/qml/pages/TasksPage.qml"));
    QVERIFY2(taskPage.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(taskPage.errorString()));
    const QByteArray source = taskPage.readAll();
    const qsizetype delegateStart = source.indexOf("model: taskController.model");
    const qsizetype automationRole = source.indexOf("required property var automation", delegateStart);
    const qsizetype editCall = source.indexOf("editor.openForTask", delegateStart);
    QVERIFY(delegateStart >= 0);
    QVERIFY(automationRole > delegateStart);
    QVERIFY(editCall > automationRole);
    QVERIFY(source.mid(editCall, 260).contains("subtasks, automation"));
}

QTEST_GUILESS_MAIN(TaskModelTest)
#include "task_model_test.moc"

#include "../src/models/tasks_model.h"

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

QTEST_GUILESS_MAIN(TaskModelTest)
#include "task_model_test.moc"

#include "application/application_service_graph.h"
#include "controllers/desktop_applications_controller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>

using namespace something;

// No native actions: the real graph/storage/pool run against a temporary profile.
class SavedApplicationsPlatform final : public platform::DesktopApplicationsPlatform {
public:
    bool supported() const noexcept override { return true; }
    domain::AppError resolveExecutable(const QString &path, QString *canonical) const override
    {
        if (!canonical || !path.endsWith(QStringLiteral(".exe")))
            return domain::AppError(domain::ErrorCategory::Validation);
        *canonical = path;
        return {};
    }
    QImage extractExecutableIcon(const QString &, const application::CancellationToken &) const override
    {
        return {};
    }
    domain::AppError launchExecutable(const QString &,
        const application::CancellationToken &cancellation) const override
    {
        if (cancellation.isCancellationRequested()) return domain::AppError::cancelled();
        ++launches;
        return {};
    }
    mutable std::atomic_int launches{0};
};

class DesktopApplicationsControllerTest final : public QObject {
    Q_OBJECT
private slots:
    void workflowsCompleteAcrossWorkerAndStorageStages();
    void rejectedContinuationClearsBusyAndAllowsRetry();
    void stagingCleanupRequiresTheProfileLock();
};

void DesktopApplicationsControllerTest::workflowsCompleteAcrossWorkerAndStorageStages()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    SavedApplicationsPlatform native;
    application::ApplicationServiceGraph graph(QStringLiteral("saved-apps"), temporary.path());
    providers::ApplicationIconImageProvider images(graph.profileDirectory());
    controllers::DesktopApplicationsController controller(&graph, &native, &images);
    controller.setPageVisible(true);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(graph.lifecycle().state() == application::ApplicationLifecycle::State::Running, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.loading() && graph.operations().activeCount() == 0, 5000);
    QCOMPARE(controller.applications()->count(), 0);

    QSignalSpy mutations(&controller, &controllers::DesktopApplicationsController::mutationFinished);
    const QString path = QDir(temporary.path()).filePath(QString::fromUtf8("demo # ğ%.exe"));
    controller.addFromFile(QUrl::fromLocalFile(path));
    QTRY_COMPARE_WITH_TIMEOUT(mutations.count(), 1, 5000);
    QVERIFY(mutations.takeFirst().at(0).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.loading() && graph.operations().activeCount() == 0, 5000);
    QCOMPARE(controller.applications()->count(), 1);
    const qint64 id = controller.applications()->data(controller.applications()->index(0),
        models::DesktopApplicationsModel::IdRole).toLongLong();
    QVERIFY(id > 0);
    QCOMPARE(controller.applications()->rowForId(id).value(QStringLiteral("executablePath")).toString(), path);
    QVERIFY(controller.applications()->rowForId(id).value(QStringLiteral("available")).toBool());
    QVERIFY(!controller.busy());

    controller.renameApplication(id, QStringLiteral("Renamed"));
    QTRY_COMPARE_WITH_TIMEOUT(mutations.count(), 1, 5000);
    QVERIFY(mutations.takeFirst().at(0).toBool());
    QCOMPARE(controller.applications()->rowForId(id).value(QStringLiteral("displayName")).toString(),
             QStringLiteral("Renamed"));

    const QString relocated = QDir(temporary.path()).filePath(QString::fromUtf8("moved # ş.exe"));
    controller.relocateFromFile(id, QUrl::fromLocalFile(relocated));
    QTRY_COMPARE_WITH_TIMEOUT(mutations.count(), 1, 5000);
    QVERIFY(mutations.takeFirst().at(0).toBool());
    QCOMPARE(controller.applications()->rowForId(id).value(QStringLiteral("executablePath")).toString(), relocated);

    controller.launchApplication(id);
    QTRY_COMPARE_WITH_TIMEOUT(mutations.count(), 1, 5000);
    QVERIFY(mutations.takeFirst().at(0).toBool());
    QCOMPARE(native.launches.load(), 1);
    QVERIFY(!controller.busy());

    const QString iconPath = QDir(temporary.path()).filePath(QString::fromUtf8("icon # ü%.png"));
    QImage icon(8, 8, QImage::Format_ARGB32);
    icon.fill(Qt::blue);
    QVERIFY(icon.save(iconPath, "PNG"));
    controller.importIconFile(id, QUrl::fromLocalFile(iconPath));
    QTRY_COMPARE_WITH_TIMEOUT(mutations.count(), 1, 5000);
    QVERIFY(mutations.takeFirst().at(0).toBool());
    const QString filename = controller.applications()->rowForId(id).value(QStringLiteral("iconFilename")).toString();
    QVERIFY(!filename.isEmpty());
    const QDir iconDirectory(QDir(graph.profileDirectory()).filePath(QStringLiteral("assets/application-icons")));
    QVERIFY(QFileInfo::exists(iconDirectory.filePath(filename)));

    controller.resetIcon(id);
    QTRY_COMPARE_WITH_TIMEOUT(mutations.count(), 1, 5000);
    QVERIFY(mutations.takeFirst().at(0).toBool());
    QVERIFY(controller.applications()->rowForId(id).value(QStringLiteral("iconFilename")).toString().isEmpty());
    QVERIFY(!QFileInfo::exists(iconDirectory.filePath(filename)));

    // A failed worker stage must release busy without creating another row.
    controller.addFromPath(QStringLiteral("not-an-executable"));
    QTRY_COMPARE_WITH_TIMEOUT(mutations.count(), 1, 5000);
    QVERIFY(!mutations.takeFirst().at(0).toBool());
    QVERIFY(!controller.busy());
    QCOMPARE(controller.applications()->count(), 1);
    controller.addFromFile(QUrl(QStringLiteral("https://example.invalid/demo.exe")));
    QVERIFY(!controller.busy());
    QVERIFY(!controller.errorMessage().isEmpty());

    // Delete with a custom icon exercises the storage -> storage continuation.
    controller.importIconFile(id, QUrl::fromLocalFile(iconPath));
    QTRY_COMPARE_WITH_TIMEOUT(mutations.count(), 1, 5000);
    QVERIFY(mutations.takeFirst().at(0).toBool());
    const QString removedIcon = controller.applications()->rowForId(id).value(QStringLiteral("iconFilename")).toString();
    controller.removeApplication(id);
    QTRY_COMPARE_WITH_TIMEOUT(mutations.count(), 1, 5000);
    QVERIFY(mutations.takeFirst().at(0).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.loading() && graph.operations().activeCount() == 0, 5000);
    QCOMPARE(controller.applications()->count(), 0);
    QVERIFY(!QFileInfo::exists(iconDirectory.filePath(removedIcon)));
    QVERIFY(!controller.busy());
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void DesktopApplicationsControllerTest::rejectedContinuationClearsBusyAndAllowsRetry()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    SavedApplicationsPlatform native;
    application::ApplicationServiceGraph graph(QStringLiteral("saved-apps-retry"), temporary.path());
    bool rejectNext = true;
    bool enteredMaintenance = false;
    // Connect before the controller to reject the next stage after worker completion.
    connect(&graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
        &graph, [&](quint64, const QVariantMap &, domain::AppError) {
            if (rejectNext) {
                rejectNext = false;
                enteredMaintenance = graph.enterProfileMaintenance();
            }
        });
    controllers::DesktopApplicationsController controller(&graph, &native, nullptr);
    graph.start();
    QTRY_VERIFY_WITH_TIMEOUT(graph.lifecycle().state() == application::ApplicationLifecycle::State::Running, 5000);
    QSignalSpy mutations(&controller, &controllers::DesktopApplicationsController::mutationFinished);
    controller.addFromPath(QStringLiteral("retry.exe"));
    QTRY_COMPARE_WITH_TIMEOUT(mutations.count(), 1, 5000);
    QVERIFY(enteredMaintenance);
    QVERIFY(!mutations.takeFirst().at(0).toBool());
    QVERIFY(!controller.busy());
    QCOMPARE(graph.operations().activeCount(), 0);
    QCOMPARE(controller.applications()->count(), 0);
    graph.leaveProfileMaintenance();
    controller.addFromPath(QStringLiteral("retry.exe"));
    QTRY_COMPARE_WITH_TIMEOUT(mutations.count(), 1, 5000);
    QVERIFY(mutations.takeFirst().at(0).toBool());
    QVERIFY(!controller.busy());
    QCOMPARE(controller.applications()->count(), 1);
    graph.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(graph.isShutdownComplete(), 5000);
}

void DesktopApplicationsControllerTest::stagingCleanupRequiresTheProfileLock()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    SavedApplicationsPlatform native;
    application::ApplicationServiceGraph owner(QStringLiteral("saved-apps-lock"), temporary.path());
    const QDir icons(QDir(owner.profileDirectory()).filePath(QStringLiteral("assets/application-icons")));
    QVERIFY(QDir().mkpath(icons.absolutePath()));
    const QString stagingPath = icons.filePath(QStringLiteral(".app-icon-upload-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.tmp"));
    QFile staging(stagingPath);
    QVERIFY(staging.open(QIODevice::WriteOnly));
    QCOMPARE(staging.write("staged"), qint64(6));
    staging.close();
    controllers::DesktopApplicationsController ownerController(&owner, &native, nullptr);
    QVERIFY(QFileInfo::exists(stagingPath)); // Constructor must leave the profile alone.
    owner.start();
    QTRY_VERIFY_WITH_TIMEOUT(owner.lifecycle().state() == application::ApplicationLifecycle::State::Running, 5000);
    QVERIFY(!QFileInfo::exists(stagingPath)); // The lock owner can recover abandoned staging.

    QVERIFY(staging.open(QIODevice::WriteOnly));
    QCOMPARE(staging.write("active"), qint64(6));
    staging.close();
    const QString deletionPath = icons.filePath(QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.png.deleting"));
    QFile deletion(deletionPath);
    QVERIFY(deletion.open(QIODevice::WriteOnly));
    QCOMPARE(deletion.write("active"), qint64(6));
    deletion.close();
    application::ApplicationServiceGraph second(QStringLiteral("saved-apps-lock"), temporary.path());
    controllers::DesktopApplicationsController secondController(&second, &native, nullptr);
    QVERIFY(QFileInfo::exists(stagingPath));
    QVERIFY(QFileInfo::exists(deletionPath));
    second.start();
    QTRY_VERIFY_WITH_TIMEOUT(second.isShutdownComplete(), 5000);
    QCOMPARE(second.lifecycle().startupError().category(), domain::ErrorCategory::InstanceAlreadyRunning);
    QVERIFY(QFileInfo::exists(stagingPath));
    QVERIFY(QFileInfo::exists(deletionPath));
    owner.shutdown();
    QTRY_VERIFY_WITH_TIMEOUT(owner.isShutdownComplete(), 5000);
}

QTEST_GUILESS_MAIN(DesktopApplicationsControllerTest)
#include "desktop_applications_controller_test.moc"

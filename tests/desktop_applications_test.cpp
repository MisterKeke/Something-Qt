#include "domain/desktop_application_rules.h"
#include "models/desktop_applications_model.h"
#include "models/running_windows_model.h"
#include "platform/desktop_applications_platform.h"
#include "services/desktop_app_icon_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QIODevice>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <utility>
#include <QtTest>

using namespace something;

class FakeDesktopPlatform final : public platform::DesktopApplicationsPlatform {
public:
    bool supported() const noexcept override { return true; }
    domain::AppError resolveExecutable(const QString &selectedPath,
                                      QString *canonicalPath) const override
    {
        if (!canonicalPath || !validPaths.contains(selectedPath))
            return domain::AppError(domain::ErrorCategory::Validation);
        *canonicalPath = selectedPath;
        return domain::AppError::none();
    }
    QImage extractExecutableIcon(const QString &,
                                 const application::CancellationToken &) const override
    {
        return {};
    }
    domain::AppError launchExecutable(const QString &,
                                      const application::CancellationToken &) const override
    {
        ++launchCalls;
        return domain::AppError::none();
    }

    QSet<QString> validPaths{QStringLiteral("C:/Apps/sample.exe")};
    mutable int launchCalls{0};
};

class FakeRunningWindowsPlatform final : public platform::RunningWindowsPlatform {
public:
    bool supported() const noexcept override { return true; }
    QVector<platform::RunningWindow> list(const application::CancellationToken &cancellation,
                                          domain::AppError *error) override
    {
        if (cancellation.isCancellationRequested()) {
            if (error) *error = domain::AppError::cancelled();
            return {};
        }
        if (error) *error = domain::AppError::none();
        return current;
    }
    platform::WindowActionResult activate(const QString &windowId,
                                          const application::CancellationToken &cancellation) override
    {
        if (cancellation.isCancellationRequested()) return platform::WindowActionResult::Cancelled;
        for (const auto &window : current)
            if (window.id == windowId) return platform::WindowActionResult::Activated;
        return platform::WindowActionResult::Unavailable;
    }

    QVector<platform::RunningWindow> current;
};

class DesktopApplicationsTest final : public QObject {
    Q_OBJECT

private slots:
    void applicationNamesUseTrimmedUnicodeLimit();
    void desktopApplicationModelSortsAndNotifiesByRow();
    void runningWindowModelPreservesIdentityAndUpdatesState();
    void fakeAdaptersRejectMissingPathsAndVanishedWindowsWithoutLaunching();
    void customIconsAreValidatedStagedAndSafelyRemoved();
};

static QVariantMap appRow(qint64 id, QString name, QString path, QString icon = {})
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("displayName"), std::move(name)},
            {QStringLiteral("executablePath"), std::move(path)},
            {QStringLiteral("iconFilename"), std::move(icon)},
            {QStringLiteral("available"), true}};
}

void DesktopApplicationsTest::applicationNamesUseTrimmedUnicodeLimit()
{
    const auto normalized = domain::normalizeDesktopApplicationName(QStringLiteral("  Notes ✨  "));
    QVERIFY(normalized.has_value());
    QCOMPARE(*normalized, QStringLiteral("Notes ✨"));
    QVERIFY(!domain::normalizeDesktopApplicationName(QString(121, QLatin1Char('x'))).has_value());
    QVERIFY(!domain::normalizeDesktopApplicationName(QStringLiteral("   ")).has_value());
    QCOMPARE(domain::defaultDesktopApplicationName(QStringLiteral("C:/Apps/my-tool.exe")),
             QStringLiteral("my-tool"));
}

void DesktopApplicationsTest::desktopApplicationModelSortsAndNotifiesByRow()
{
    models::DesktopApplicationsModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy insertedSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy movedSpy(&model, &QAbstractItemModel::rowsMoved);
    QSignalSpy changedSpy(&model, &QAbstractItemModel::dataChanged);

    model.replace({appRow(2, QStringLiteral("Beta"), QStringLiteral("C:/beta.exe")),
                   appRow(1, QStringLiteral("Alpha"), QStringLiteral("C:/alpha.exe"))});
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.data(model.index(0, 0), models::DesktopApplicationsModel::DisplayNameRole).toString(),
             QStringLiteral("Alpha"));
    QCOMPARE(insertedSpy.count(), 2);
    QCOMPARE(resetSpy.count(), 0);

    model.upsert(appRow(2, QStringLiteral("0 Beta"), QStringLiteral("C:/beta.exe"),
                        QString(32, QLatin1Char('a')) + QStringLiteral(".png")));
    QCOMPARE(model.data(model.index(0, 0), models::DesktopApplicationsModel::DisplayNameRole).toString(),
             QStringLiteral("0 Beta"));
    QVERIFY(movedSpy.count() >= 1);
    QVERIFY(changedSpy.count() >= 1);
    QVERIFY(model.removeById(1));
    QCOMPARE(model.count(), 1);
    QCOMPARE(resetSpy.count(), 0);
}

void DesktopApplicationsTest::runningWindowModelPreservesIdentityAndUpdatesState()
{
    models::RunningWindowsModel model;
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changedSpy(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy movedSpy(&model, &QAbstractItemModel::rowsMoved);

    model.replace({{QStringLiteral("22:200"), QStringLiteral("Editor"), QStringLiteral("editor.exe"), {}, true, false},
                   {QStringLiteral("11:100"), QStringLiteral("Browser"), QStringLiteral("browser.exe"), {}, false, true}});
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.idAt(0), QStringLiteral("11:100"));
    QCOMPARE(model.rowForId(QStringLiteral("22:200")), 1);

    model.replace({{QStringLiteral("22:200"), QStringLiteral("Editor"), QStringLiteral("editor.exe"), {}, false, true},
                   {QStringLiteral("11:100"), QStringLiteral("Browser"), QStringLiteral("browser.exe"), {}, false, false}});
    QCOMPARE(model.data(model.index(1, 0), models::RunningWindowsModel::ActiveRole).toBool(), true);
    QCOMPARE(model.data(model.index(1, 0), models::RunningWindowsModel::MinimizedRole).toBool(), false);
    QVERIFY(changedSpy.count() >= 2);
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(movedSpy.count(), 0);

    model.replace({{QStringLiteral("22:200"), QStringLiteral("A Editor"), QStringLiteral("editor.exe"), {}, false, true}});
    QVERIFY(movedSpy.count() >= 1);
    QCOMPARE(model.rowForId(QStringLiteral("11:100")), -1);
}

void DesktopApplicationsTest::fakeAdaptersRejectMissingPathsAndVanishedWindowsWithoutLaunching()
{
    FakeDesktopPlatform applications;
    QString resolved;
    QVERIFY(!applications.resolveExecutable(QStringLiteral("C:/Apps/sample.exe"), &resolved).isError());
    QCOMPARE(resolved, QStringLiteral("C:/Apps/sample.exe"));
    QVERIFY(applications.resolveExecutable(QStringLiteral("C:/Apps/missing.exe"), &resolved).isError());

    FakeRunningWindowsPlatform running;
    running.current = {{QStringLiteral("501:9001"), QStringLiteral("Document"),
                        QStringLiteral("editor.exe"), {}, false, false}};
    domain::AppError error;
    const auto snapshot = running.list({}, &error);
    QVERIFY(!error.isError());
    QCOMPARE(snapshot.size(), 1);
    QCOMPARE(running.activate(QStringLiteral("501:9001"), {}), platform::WindowActionResult::Activated);
    running.current.clear();
    QCOMPARE(running.activate(QStringLiteral("501:9001"), {}), platform::WindowActionResult::Unavailable);
    QCOMPARE(applications.launchCalls, 0); // Tests never start a real or fake child process.
}

void DesktopApplicationsTest::customIconsAreValidatedStagedAndSafelyRemoved()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString profile = QDir(temporary.path()).filePath(QStringLiteral("profile"));
    const QString sourcePath = QDir(temporary.path()).filePath(QStringLiteral("selected-icon.png"));
    QImage source(24, 24, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::magenta);
    QVERIFY(source.save(sourcePath, "PNG"));

    services::StagedDesktopAppIcon staged;
    const application::CancellationToken cancellation;
    QVERIFY(!services::DesktopAppIconStore::stageImport(
        sourcePath, profile, cancellation, &staged).isError());
    QCOMPARE(staged.asset.mimeType, QStringLiteral("image/png"));
    const QString assetDirectory = QDir(profile).filePath(QStringLiteral("assets/application-icons"));
    QVERIFY(QFileInfo::exists(QDir(assetDirectory).filePath(staged.stagingFilename)));
    QVERIFY(services::DesktopAppIconStore::install(profile, staged));
    const QString installed = QDir(assetDirectory).filePath(staged.asset.filename);
    QVERIFY(QFileInfo::exists(installed));

    services::StagedDesktopAppIconDeletion deletion;
    QVERIFY(services::DesktopAppIconStore::stageDeletion(profile, staged.asset.filename, &deletion));
    QVERIFY(deletion.moved);
    QVERIFY(!QFileInfo::exists(installed));
    services::DesktopAppIconStore::restoreDeletion(profile, deletion);
    QVERIFY(QFileInfo::exists(installed));

    const QString invalidPath = QDir(temporary.path()).filePath(QStringLiteral("invalid.png"));
    QFile invalid(invalidPath);
    QVERIFY(invalid.open(QIODevice::WriteOnly));
    QCOMPARE(invalid.write("not an image"), qint64(12));
    invalid.close();
    services::StagedDesktopAppIcon rejected;
    QVERIFY(services::DesktopAppIconStore::stageImport(
        invalidPath, profile, cancellation, &rejected).isError());

    QVERIFY(services::DesktopAppIconStore::stageDeletion(profile, staged.asset.filename, &deletion));
    services::DesktopAppIconStore::removeAbandonedStagingFiles(profile);
    QVERIFY(QFileInfo::exists(installed)); // Interrupted staged deletion restores a referenced asset.
    QVERIFY(services::DesktopAppIconStore::stageDeletion(profile, staged.asset.filename, &deletion));
    services::DesktopAppIconStore::finishDeletion(profile, deletion);
    QVERIFY(!QFileInfo::exists(installed));
}

QTEST_GUILESS_MAIN(DesktopApplicationsTest)
#include "desktop_applications_test.moc"

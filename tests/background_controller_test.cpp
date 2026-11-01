#include "controllers/background_controller.h"

#include <QCoreApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using something::controllers::BackgroundController;

class BackgroundControllerTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void closeToTrayRequiresAnAvailableTrayAndNoExit();
    void closeToTrayHidesAnAttachedWindowWhenAvailable();
    void initialPresentationHonorsArgumentAndPreference();
    void backgroundArgumentIsRecognized();
    void trayDisabledStartupFallsBackToVisiblePresentation();
    void startupCommandQuotesTheExecutablePath();
    void startupPathCanBeParsedAndCheckedForStaleness();
    void backgroundPreferencesPersistAcrossControllerInstances();
    void repeatedRestoreRequestsReuseTheExistingPresentation();
    void repeatedExitRequestsEmitOneShutdownRequest();
    void fallbackNotificationsQueueUntilDismissedAndRejectOverflow();

private:
    QTemporaryDir m_settingsDirectory;
};

void BackgroundControllerTest::initTestCase()
{
    QVERIFY(m_settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDirectory.path());
    QCoreApplication::setOrganizationName(QStringLiteral("SomethingQtBackgroundTests"));
    QCoreApplication::setApplicationName(QStringLiteral("Settings"));
}

void BackgroundControllerTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

void BackgroundControllerTest::closeToTrayRequiresAnAvailableTrayAndNoExit()
{
    QVERIFY(BackgroundController::hideToTrayAllowed(true, true, true, false, false));
    QVERIFY(!BackgroundController::hideToTrayAllowed(false, true, true, false, false));
    QVERIFY(!BackgroundController::hideToTrayAllowed(true, false, true, false, false));
    QVERIFY(!BackgroundController::hideToTrayAllowed(true, true, false, false, false));
    QVERIFY(!BackgroundController::hideToTrayAllowed(true, true, true, true, false));
    QVERIFY(!BackgroundController::hideToTrayAllowed(true, true, true, false, true));
}

void BackgroundControllerTest::closeToTrayHidesAnAttachedWindowWhenAvailable()
{
    BackgroundController controller(false);
    if (!controller.trayAvailable()) QSKIP("The test environment has no system tray.");
    QWindow mainWindow;
    QWindow consoleWindow;
    controller.attachWindows(&mainWindow, &consoleWindow);
    controller.setSystemTrayEnabled(true);
    controller.setMinimizeToTrayOnClose(true);
    mainWindow.show();
    QTRY_VERIFY(mainWindow.isVisible());
    QVERIFY(controller.handleWindowClose(&mainWindow));
    QVERIFY(!mainWindow.isVisible());
}

void BackgroundControllerTest::initialPresentationHonorsArgumentAndPreference()
{
    QVERIFY(!BackgroundController::initialTrayPresentationRequested(false, false));
    QVERIFY(BackgroundController::initialTrayPresentationRequested(true, false));
    QVERIFY(BackgroundController::initialTrayPresentationRequested(false, true));
}

void BackgroundControllerTest::backgroundArgumentIsRecognized()
{
    QCommandLineParser parser;
    const QCommandLineOption backgroundOption(QStringLiteral("background"));
    parser.addOption(backgroundOption);
    QVERIFY(parser.parse(QStringList{QStringLiteral("SomethingQt"),
                                     QStringLiteral("--background")}));
    QVERIFY(parser.isSet(backgroundOption));
}

void BackgroundControllerTest::trayDisabledStartupFallsBackToVisiblePresentation()
{
    BackgroundController controller(true);
    controller.setSystemTrayEnabled(false);
    QVERIFY(!controller.presentInitiallyInTray());
    QVERIFY(controller.statusMessage().contains(QStringLiteral("disabled"),
                                                Qt::CaseInsensitive));
}

void BackgroundControllerTest::startupCommandQuotesTheExecutablePath()
{
    const QString path = QStringLiteral("C:/Program Files/SomethingQt/SomethingQt.exe");
    QCOMPARE(BackgroundController::startupCommand(path, true),
             QStringLiteral("\"%1\" --background").arg(QDir::toNativeSeparators(path)));
    QCOMPARE(BackgroundController::startupCommand(path, false),
             QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(path)));
}

void BackgroundControllerTest::startupPathCanBeParsedAndCheckedForStaleness()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = directory.filePath(QStringLiteral("Something Qt.exe"));
    const QString command = BackgroundController::startupCommand(executable, true);
    QCOMPARE(BackgroundController::startupExecutableFromCommand(command),
             QDir::toNativeSeparators(executable));
    QVERIFY(BackgroundController::startupCommandPathIsStale(command));

    QFile file(executable);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QVERIFY(!BackgroundController::startupCommandPathIsStale(command));
}

void BackgroundControllerTest::backgroundPreferencesPersistAcrossControllerInstances()
{
    {
        BackgroundController controller(false);
        QVERIFY(controller.systemTrayEnabled());
        QVERIFY(controller.minimizeToTrayOnClose());
        QVERIFY(!controller.launchWithWindows());
        QVERIFY(!controller.startMinimizedToTray());
        QVERIFY(!controller.notifyOnFirstHide());

        controller.setSystemTrayEnabled(false);
        controller.setMinimizeToTrayOnClose(false);
        controller.setStartMinimizedToTray(true);
        controller.setNotifyOnFirstHide(true);
    }

    {
        BackgroundController controller(false);
        QVERIFY(!controller.systemTrayEnabled());
        QVERIFY(!controller.minimizeToTrayOnClose());
        QVERIFY(controller.startMinimizedToTray());
        QVERIFY(controller.notifyOnFirstHide());
        QVERIFY(!controller.launchWithWindows());
    }
}

void BackgroundControllerTest::repeatedRestoreRequestsReuseTheExistingPresentation()
{
    BackgroundController controller(false);
    QSignalSpy mainSpy(&controller, &BackgroundController::openMainRequested);
    QSignalSpy consoleSpy(&controller, &BackgroundController::openConsoleRequested);
    QVERIFY(mainSpy.isValid());
    QVERIFY(consoleSpy.isValid());

    controller.openMainWindow();
    controller.openMainWindow();
    controller.openConsoleWindow();
    controller.openConsoleWindow();

    QCOMPARE(mainSpy.count(), 2);
    QCOMPARE(consoleSpy.count(), 2);
}

void BackgroundControllerTest::repeatedExitRequestsEmitOneShutdownRequest()
{
    BackgroundController controller(false);
    QSignalSpy exitSpy(&controller, &BackgroundController::exitRequested);
    QVERIFY(exitSpy.isValid());
    connect(&controller, &BackgroundController::exitRequested, &controller,
            [&controller] { controller.setShutdownRequested(true); });

    controller.exitCompletely();
    controller.exitCompletely();

    QCOMPARE(exitSpy.count(), 1);
}

void BackgroundControllerTest::fallbackNotificationsQueueUntilDismissedAndRejectOverflow()
{
    BackgroundController controller(false);
    controller.setSystemTrayEnabled(false);
    const quint64 startingSequence = controller.notificationSequence();
    QVERIFY(controller.showAutomationNotification(QStringLiteral("Task"),
                                                   QStringLiteral("First message")));
    QVERIFY(controller.showAutomationNotification(QStringLiteral("Task"),
                                                   QStringLiteral("Second message")));
    QCOMPARE(controller.notificationSequence(), startingSequence + 2);
    QCOMPARE(controller.notificationMessage(), QStringLiteral("First message"));

    controller.clearNotificationMessage();
    QCOMPARE(controller.notificationMessage(), QStringLiteral("Second message"));
    controller.clearNotificationMessage();
    QVERIFY(controller.notificationMessage().isEmpty());

    for (int index = 0; index < 100; ++index)
        QVERIFY(controller.showAutomationNotification(QStringLiteral("Task"),
            QStringLiteral("Queued %1").arg(index)));
    QVERIFY(!controller.showAutomationNotification(QStringLiteral("Task"),
                                                   QStringLiteral("Overflow")));
    QVERIFY(controller.statusMessage().contains(QStringLiteral("queue is full"),
                                                Qt::CaseInsensitive));
}

QTEST_MAIN(BackgroundControllerTest)
#include "background_controller_test.moc"

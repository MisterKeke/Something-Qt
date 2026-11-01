#include "background_controller.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QSettings>
#include <utility>

namespace something::controllers {
namespace {

const QString kSystemTrayEnabledKey = QStringLiteral("background/systemTrayEnabled");
const QString kMinimizeToTrayKey = QStringLiteral("background/minimizeToTrayOnClose");
const QString kLaunchWithWindowsKey = QStringLiteral("background/launchWithWindows");
const QString kStartMinimizedKey = QStringLiteral("background/startMinimizedToTray");
const QString kNotifyOnFirstHideKey = QStringLiteral("background/notifyOnFirstHide");
const QString kFirstHideNotificationShownKey = QStringLiteral("background/firstHideNotificationShown");

} // namespace

BackgroundController::BackgroundController(bool backgroundArgumentRequested,
                                           QObject *parent)
    : QObject(parent)
    , m_settings()
    , m_trayIcon(new QSystemTrayIcon(this))
    , m_trayMenu(new QMenu)
    , m_backgroundArgumentRequested(backgroundArgumentRequested)
{
    Q_ASSERT(QApplication::instance());
    readSettings();
    createTrayMenu();

    m_trayIcon->setIcon(QIcon(QStringLiteral(":/something/assets/appicon.png")));
    m_trayIcon->setToolTip(tr("Something is running in the background."));
    m_trayIcon->setContextMenu(m_trayMenu);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick)
            openMainWindow();
    });

    m_watchdog.setInterval(5000);
    m_watchdog.setTimerType(Qt::CoarseTimer);
    connect(&m_watchdog, &QTimer::timeout,
            this, &BackgroundController::checkTrayAvailability);

    updateTrayIcon();
    if (m_launchWithWindows)
        synchronizeWindowsStartup();
}

BackgroundController::~BackgroundController()
{
    if (m_trayIcon)
        m_trayIcon->setContextMenu(nullptr);
    delete m_trayMenu;
    m_trayMenu = nullptr;
}

bool BackgroundController::startupSupported() const noexcept
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool BackgroundController::trayAvailable() const noexcept
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

bool BackgroundController::inTrayMode() const noexcept
{
    return m_trayModeRequested && !m_hasVisibleWindow && !m_shutdownRequested
        && m_trayIcon && m_trayIcon->isVisible() && trayAvailable();
}

void BackgroundController::attachWindows(QWindow *mainWindow, QWindow *consoleWindow)
{
    m_mainWindow = mainWindow;
    m_consoleWindow = consoleWindow;
    if (m_mainWindow) {
        connect(m_mainWindow, &QWindow::visibleChanged, this,
                [this](bool) { updateVisibleWindowState(); });
    }
    if (m_consoleWindow) {
        connect(m_consoleWindow, &QWindow::visibleChanged, this,
                [this](bool) { updateVisibleWindowState(); });
    }
    updateVisibleWindowState();
    updateWatchdog();
}

bool BackgroundController::presentInitiallyInTray()
{
    if (!initialTrayPresentationRequested(m_backgroundArgumentRequested,
                                         m_startMinimizedToTray))
        return false;
    if (!m_systemTrayEnabled || !trayAvailable() || !m_trayIcon->isVisible()) {
        setStatusMessage(!m_systemTrayEnabled
            ? tr("Start in tray was requested, but system tray integration is disabled. The Console is open instead.")
            : tr("The system tray is unavailable. The Console is open instead."));
        return false;
    }

    m_trayModeRequested = true;
    if (m_mainWindow) m_mainWindow->hide();
    if (m_consoleWindow) m_consoleWindow->hide();
    updateVisibleWindowState();
    updateWatchdog();
    return true;
}

bool BackgroundController::hideToTrayAllowed(bool trayEnabled, bool minimizeToTray,
                                             bool available, bool shutdownRequested,
                                             bool systemShutdown) noexcept
{
    return trayEnabled && minimizeToTray && available
        && !shutdownRequested && !systemShutdown;
}

bool BackgroundController::initialTrayPresentationRequested(
    bool backgroundArgument, bool startMinimizedPreference) noexcept
{
    return backgroundArgument || startMinimizedPreference;
}

QString BackgroundController::startupCommand(const QString &executablePath,
                                             bool startMinimized)
{
    const QString nativePath = QDir::toNativeSeparators(executablePath);
    return QStringLiteral("\"%1\"%2")
        .arg(nativePath,
             startMinimized ? QStringLiteral(" --background") : QString());
}

QString BackgroundController::startupExecutableFromCommand(const QString &command)
{
    const QString trimmed = command.trimmed();
    if (trimmed.isEmpty()) return {};
    if (trimmed.startsWith(QLatin1Char('"'))) {
        const qsizetype closingQuote = trimmed.indexOf(QLatin1Char('"'), 1);
        return closingQuote > 1 ? trimmed.mid(1, closingQuote - 1) : QString();
    }
    qsizetype argumentStart = -1;
    for (qsizetype index = 0; index < trimmed.size(); ++index) {
        if (trimmed.at(index).isSpace()) {
            argumentStart = index;
            break;
        }
    }
    return argumentStart < 0 ? trimmed : trimmed.left(argumentStart);
}

bool BackgroundController::startupCommandPathIsStale(const QString &command)
{
    const QString executable = startupExecutableFromCommand(command);
    return executable.isEmpty() || !QFileInfo::exists(executable);
}

bool BackgroundController::handleWindowClose(QObject *window)
{
    if (!hideToTrayAllowed(m_systemTrayEnabled, m_minimizeToTrayOnClose,
                           trayAvailable() && m_trayIcon->isVisible(),
                           m_shutdownRequested, m_systemShutdown)) {
        return false;
    }
    auto *target = qobject_cast<QWindow *>(window);
    if (!target) return false;

    m_trayModeRequested = true;
    target->hide();
    updateVisibleWindowState();
    if (!m_hasVisibleWindow)
        showFirstHideNotification();
    updateWatchdog();
    return true;
}

void BackgroundController::openMainWindow()
{
    if (!m_shutdownRequested)
        emit openMainRequested();
}

void BackgroundController::openConsoleWindow()
{
    if (!m_shutdownRequested)
        emit openConsoleRequested();
}

void BackgroundController::openSettings()
{
    if (!m_shutdownRequested)
        emit openSettingsRequested();
}

void BackgroundController::exitCompletely()
{
    if (m_shutdownRequested) return;
    emit exitRequested();
}

void BackgroundController::beginSystemShutdown()
{
    if (m_systemShutdown) return;
    m_systemShutdown = true;
    exitCompletely();
    emit presentationChanged();
}

void BackgroundController::setShutdownRequested(bool requested)
{
    if (m_shutdownRequested == requested) return;
    m_shutdownRequested = requested;
    if (requested) {
        m_trayModeRequested = false;
        m_watchdog.stop();
        if (m_trayIcon) m_trayIcon->hide();
        if (m_mainWindow) m_mainWindow->hide();
        if (m_consoleWindow) m_consoleWindow->hide();
    } else {
        updateTrayIcon();
    }
    updateVisibleWindowState();
    emit presentationChanged();
}

void BackgroundController::createTrayMenu()
{
    const auto addAction = [this](const QString &label, auto slot) {
        auto *action = new QAction(label, m_trayMenu);
        m_trayMenu->addAction(action);
        connect(action, &QAction::triggered, this, slot);
    };
    addAction(tr("Open Something-Qt"), [this] { openMainWindow(); });
    addAction(tr("Open Console"), [this] { openConsoleWindow(); });
    addAction(tr("Settings"), [this] { openSettings(); });
    m_trayMenu->addSeparator();
    auto *statistics = new QAction(tr("Usage Statistics"), m_trayMenu);
    m_trayMenu->addAction(statistics);
    connect(statistics, &QAction::triggered, this,
            &BackgroundController::openUsageStatisticsRequested);
    m_usageTrackingAction = new QAction(m_trayMenu);
    m_trayMenu->addAction(m_usageTrackingAction);
    connect(m_usageTrackingAction, &QAction::triggered, this,
            &BackgroundController::toggleUsageTrackingRequested);
    m_usageStatusAction = new QAction(m_trayMenu);
    m_usageStatusAction->setEnabled(false);
    m_trayMenu->addAction(m_usageStatusAction);
    updateUsageActions();
    m_trayMenu->addSeparator();
    addAction(tr("Exit Completely"), [this] { exitCompletely(); });
}

void BackgroundController::setUsageTrackingState(bool enabled, QString status)
{
    if (m_usageTrackingEnabled == enabled && m_usageTrackingStatus == status) return;
    m_usageTrackingEnabled = enabled;
    m_usageTrackingStatus = std::move(status);
    updateUsageActions();
    emit presentationChanged();
}

void BackgroundController::updateUsageActions()
{
    if (m_usageTrackingAction)
        m_usageTrackingAction->setText(m_usageTrackingEnabled
            ? tr("Pause Tracking") : tr("Resume Tracking"));
    if (m_usageStatusAction) {
        const QString state = m_usageTrackingEnabled ? tr("Enabled") : tr("Paused");
        m_usageStatusAction->setText(tr("Usage tracking: %1").arg(state));
        m_usageStatusAction->setToolTip(m_usageTrackingStatus);
    }
}

void BackgroundController::readSettings()
{
    m_systemTrayEnabled = m_settings.value(kSystemTrayEnabledKey, true).toBool();
    m_minimizeToTrayOnClose = m_settings.value(kMinimizeToTrayKey, false).toBool();
    m_launchWithWindows = m_settings.value(kLaunchWithWindowsKey, false).toBool();
    m_startMinimizedToTray = m_settings.value(kStartMinimizedKey, false).toBool();
    m_notifyOnFirstHide = m_settings.value(kNotifyOnFirstHideKey, false).toBool();
    m_firstHideNotificationShown = m_settings.value(kFirstHideNotificationShownKey, false).toBool();
}

void BackgroundController::writeSetting(const QString &key, bool value)
{
    m_settings.setValue(key, value);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        setStatusMessage(tr("A background setting could not be saved."));
}

void BackgroundController::setSystemTrayEnabled(bool enabled)
{
    if (m_systemTrayEnabled == enabled) return;
    m_systemTrayEnabled = enabled;
    writeSetting(kSystemTrayEnabledKey, enabled);
    updateTrayIcon();
    emit settingsChanged();
    emit presentationChanged();
}

void BackgroundController::setMinimizeToTrayOnClose(bool enabled)
{
    if (m_minimizeToTrayOnClose == enabled) return;
    m_minimizeToTrayOnClose = enabled;
    writeSetting(kMinimizeToTrayKey, enabled);
    emit settingsChanged();
}

void BackgroundController::setLaunchWithWindows(bool enabled)
{
    if (enabled && !startupSupported()) {
        setStatusMessage(tr("Windows startup is available only on Windows."));
        return;
    }
    if (m_launchWithWindows == enabled) return;
    m_launchWithWindows = enabled;
    writeSetting(kLaunchWithWindowsKey, enabled);
    synchronizeWindowsStartup();
    emit settingsChanged();
}

void BackgroundController::setStartMinimizedToTray(bool enabled)
{
    if (m_startMinimizedToTray == enabled) return;
    m_startMinimizedToTray = enabled;
    writeSetting(kStartMinimizedKey, enabled);
    if (m_launchWithWindows)
        synchronizeWindowsStartup();
    emit settingsChanged();
}

void BackgroundController::setNotifyOnFirstHide(bool enabled)
{
    if (m_notifyOnFirstHide == enabled) return;
    m_notifyOnFirstHide = enabled;
    writeSetting(kNotifyOnFirstHideKey, enabled);
    emit settingsChanged();
}

bool BackgroundController::showAutomationNotification(QString title, QString message)
{
    title = title.trimmed();
    message = message.trimmed();
    if (message.isEmpty()) return false;
    if (m_systemTrayEnabled && trayAvailable() && QSystemTrayIcon::supportsMessages()
        && m_trayIcon && m_trayIcon->isVisible()) {
        m_trayIcon->showMessage(title.isEmpty() ? tr("Task automation") : title,
                                message, QSystemTrayIcon::Information, 5000);
        return true;
    }
    // Keep fallback messages until an active window presents and dismisses
    // them. A bounded queue rejects overflow so the automation history records
    // a failed notification instead of silently losing an accepted message.
    constexpr qsizetype maximumQueuedNotifications = 100;
    if (m_notifications.size() >= maximumQueuedNotifications) {
        setStatusMessage(tr("The in-app notification queue is full."));
        return false;
    }
    m_notifications.enqueue({std::move(title), std::move(message)});
    ++m_notificationSequence;
    emit presentationChanged();
    return true;
}

QString BackgroundController::notificationMessage() const
{
    return m_notifications.isEmpty() ? QString() : m_notifications.head().message;
}

void BackgroundController::clearNotificationMessage()
{
    if (m_notifications.isEmpty()) return;
    m_notifications.dequeue();
    ++m_notificationSequence;
    emit presentationChanged();
}

void BackgroundController::updateTrayIcon()
{
    const bool shouldShow = m_systemTrayEnabled && trayAvailable() && !m_shutdownRequested;
    if (m_trayIcon)
        m_trayIcon->setVisible(shouldShow);
    if (!m_systemTrayEnabled) {
        setStatusMessage(tr("System tray integration is disabled."));
    } else if (!trayAvailable()) {
        setStatusMessage(tr("The Windows notification area is currently unavailable."));
    } else if (m_statusMessage.startsWith(tr("The Windows notification area"))
               || m_statusMessage.startsWith(tr("The system tray is unavailable."))
               || m_statusMessage.startsWith(tr("The notification area did not return."))
               || m_statusMessage == tr("System tray integration is disabled.")) {
        setStatusMessage({});
    }
    updateWatchdog();
}

void BackgroundController::updateVisibleWindowState()
{
    const bool visible = (m_mainWindow && m_mainWindow->isVisible())
        || (m_consoleWindow && m_consoleWindow->isVisible());
    if (m_hasVisibleWindow != visible) {
        m_hasVisibleWindow = visible;
        if (visible) m_trayModeRequested = false;
        emit presentationChanged();
    }
    updateWatchdog();
}

void BackgroundController::updateWatchdog()
{
    if (m_shutdownRequested || !m_systemTrayEnabled) {
        m_watchdog.stop();
        return;
    }
    if (!trayAvailable() || inTrayMode()) {
        if (!m_watchdog.isActive()) m_watchdog.start();
    } else {
        m_watchdog.stop();
        m_consecutiveUnavailableChecks = 0;
    }
}

void BackgroundController::checkTrayAvailability()
{
    const bool available = trayAvailable();
    updateTrayIcon();
    if (available) {
        m_consecutiveUnavailableChecks = 0;
        return;
    }
    if (!m_trayModeRequested || m_hasVisibleWindow)
        return;

    ++m_consecutiveUnavailableChecks;
    if (m_consecutiveUnavailableChecks < 3)
        return;

    m_trayModeRequested = false;
    setStatusMessage(tr("The notification area did not return. The Console has been restored so Something remains accessible."));
    if (m_consoleWindow) {
        if (m_consoleWindow->windowState() & Qt::WindowMinimized) {
            if (m_consoleWindow->windowState() & Qt::WindowMaximized)
                m_consoleWindow->showMaximized();
            else
                m_consoleWindow->showNormal();
        } else if (!m_consoleWindow->isVisible()) {
            m_consoleWindow->setVisible(true);
        }
        m_consoleWindow->raise();
        m_consoleWindow->requestActivate();
    } else {
        emit openConsoleRequested();
    }
    updateVisibleWindowState();
    m_consecutiveUnavailableChecks = 0;
}

void BackgroundController::showFirstHideNotification()
{
    if (!m_notifyOnFirstHide || m_firstHideNotificationShown || !m_trayIcon)
        return;
    m_trayIcon->showMessage(tr("Something is still running"),
                            tr("Open the system tray menu to restore a window or exit."),
                            QSystemTrayIcon::Information, 3500);
    m_firstHideNotificationShown = true;
    writeSetting(kFirstHideNotificationShownKey, true);
}

void BackgroundController::setStatusMessage(QString message)
{
    if (m_statusMessage == message) return;
    m_statusMessage = std::move(message);
    emit presentationChanged();
}

void BackgroundController::synchronizeWindowsStartup()
{
#ifdef Q_OS_WIN
    QSettings runKey(QSettings::NativeFormat, QSettings::UserScope,
                     QStringLiteral("Microsoft"), QStringLiteral("Windows"));
    runKey.beginGroup(QStringLiteral("CurrentVersion/Run"));
    const QString valueName = QStringLiteral("SomethingQt");
    if (!m_launchWithWindows) {
        runKey.remove(valueName);
        runKey.sync();
        if (runKey.status() != QSettings::NoError)
            setStatusMessage(tr("The Windows startup entry could not be removed."));
        return;
    }

    const QString executable = QCoreApplication::applicationFilePath();
    const QString existingCommand = runKey.value(valueName).toString();
    if (!existingCommand.isEmpty() && startupCommandPathIsStale(existingCommand)) {
        setStatusMessage(tr("A stale Windows startup path was found and replaced with this executable."));
    }
    const QString desiredCommand = startupCommand(executable, m_startMinimizedToTray);
    if (existingCommand != desiredCommand)
        runKey.setValue(valueName, desiredCommand);
    runKey.sync();
    if (runKey.status() != QSettings::NoError)
        setStatusMessage(tr("The Windows startup entry could not be updated."));
#else
    Q_UNUSED(m_launchWithWindows);
    Q_UNUSED(m_startMinimizedToTray);
#endif
}

} // namespace something::controllers

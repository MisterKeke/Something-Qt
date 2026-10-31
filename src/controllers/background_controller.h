#pragma once

#include <QPointer>
#include <QSettings>
#include <QString>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWindow>

class QAction;
class QMenu;

namespace something::controllers {

// Owns presentation-only state for the tray and the two existing QML windows.
// Long-running application services remain owned by ApplicationServiceGraph.
class BackgroundController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool systemTrayEnabled READ systemTrayEnabled WRITE setSystemTrayEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool minimizeToTrayOnClose READ minimizeToTrayOnClose WRITE setMinimizeToTrayOnClose NOTIFY settingsChanged)
    Q_PROPERTY(bool launchWithWindows READ launchWithWindows WRITE setLaunchWithWindows NOTIFY settingsChanged)
    Q_PROPERTY(bool startMinimizedToTray READ startMinimizedToTray WRITE setStartMinimizedToTray NOTIFY settingsChanged)
    Q_PROPERTY(bool notifyOnFirstHide READ notifyOnFirstHide WRITE setNotifyOnFirstHide NOTIFY settingsChanged)
    Q_PROPERTY(bool startupSupported READ startupSupported CONSTANT)
    Q_PROPERTY(bool trayAvailable READ trayAvailable NOTIFY presentationChanged)
    Q_PROPERTY(bool hasVisibleWindow READ hasVisibleWindow NOTIFY presentationChanged)
    Q_PROPERTY(bool inTrayMode READ inTrayMode NOTIFY presentationChanged)
    Q_PROPERTY(bool shutdownRequested READ shutdownRequested NOTIFY presentationChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY presentationChanged)
    Q_PROPERTY(bool usageTrackingEnabled READ usageTrackingEnabled NOTIFY presentationChanged)
    Q_PROPERTY(QString usageTrackingStatus READ usageTrackingStatus NOTIFY presentationChanged)

public:
    explicit BackgroundController(bool backgroundArgumentRequested,
                                  QObject *parent = nullptr);
    ~BackgroundController() override;

    bool systemTrayEnabled() const noexcept { return m_systemTrayEnabled; }
    bool minimizeToTrayOnClose() const noexcept { return m_minimizeToTrayOnClose; }
    bool launchWithWindows() const noexcept { return m_launchWithWindows; }
    bool startMinimizedToTray() const noexcept { return m_startMinimizedToTray; }
    bool notifyOnFirstHide() const noexcept { return m_notifyOnFirstHide; }
    bool startupSupported() const noexcept;
    bool trayAvailable() const noexcept;
    bool hasVisibleWindow() const noexcept { return m_hasVisibleWindow; }
    bool inTrayMode() const noexcept;
    bool shutdownRequested() const noexcept { return m_shutdownRequested; }
    QString statusMessage() const { return m_statusMessage; }
    bool usageTrackingEnabled() const noexcept { return m_usageTrackingEnabled; }
    QString usageTrackingStatus() const { return m_usageTrackingStatus; }

    void attachWindows(QWindow *mainWindow, QWindow *consoleWindow);
    bool presentInitiallyInTray();

    void setSystemTrayEnabled(bool enabled);
    void setMinimizeToTrayOnClose(bool enabled);
    void setLaunchWithWindows(bool enabled);
    void setStartMinimizedToTray(bool enabled);
    void setNotifyOnFirstHide(bool enabled);
    void setUsageTrackingState(bool enabled, QString status);

    Q_INVOKABLE bool handleWindowClose(QObject *window);
    Q_INVOKABLE void openMainWindow();
    Q_INVOKABLE void openConsoleWindow();
    Q_INVOKABLE void openSettings();
    Q_INVOKABLE void exitCompletely();
    Q_INVOKABLE void beginSystemShutdown();
    Q_INVOKABLE void setShutdownRequested(bool requested);

    static bool hideToTrayAllowed(bool trayEnabled, bool minimizeToTray,
                                  bool trayAvailable, bool shutdownRequested,
                                  bool systemShutdown) noexcept;
    static bool initialTrayPresentationRequested(bool backgroundArgument,
                                                 bool startMinimizedPreference) noexcept;
    static QString startupCommand(const QString &executablePath,
                                  bool startMinimized);
    static QString startupExecutableFromCommand(const QString &command);
    static bool startupCommandPathIsStale(const QString &command);

signals:
    void settingsChanged();
    void presentationChanged();
    void openMainRequested();
    void openConsoleRequested();
    void openSettingsRequested();
    void exitRequested();
    void openUsageStatisticsRequested();
    void toggleUsageTrackingRequested();

private:
    void createTrayMenu();
    void readSettings();
    void writeSetting(const QString &key, bool value);
    void updateTrayIcon();
    void updateVisibleWindowState();
    void updateWatchdog();
    void checkTrayAvailability();
    void showFirstHideNotification();
    void setStatusMessage(QString message);
    void synchronizeWindowsStartup();

    QSettings m_settings;
    QSystemTrayIcon *m_trayIcon{nullptr};
    QMenu *m_trayMenu{nullptr};
    QAction *m_usageTrackingAction{nullptr};
    QAction *m_usageStatusAction{nullptr};
    QPointer<QWindow> m_mainWindow;
    QPointer<QWindow> m_consoleWindow;
    QTimer m_watchdog;
    QString m_statusMessage;
    QString m_usageTrackingStatus{QStringLiteral("Waiting for local storage")};
    bool m_systemTrayEnabled{true};
    bool m_minimizeToTrayOnClose{false};
    bool m_launchWithWindows{false};
    bool m_startMinimizedToTray{false};
    bool m_notifyOnFirstHide{false};
    bool m_hasVisibleWindow{false};
    bool m_trayModeRequested{false};
    bool m_shutdownRequested{false};
    bool m_systemShutdown{false};
    bool m_firstHideNotificationShown{false};
    bool m_usageTrackingEnabled{true};
    int m_consecutiveUnavailableChecks{0};
    bool m_backgroundArgumentRequested{false};
};

} // namespace something::controllers

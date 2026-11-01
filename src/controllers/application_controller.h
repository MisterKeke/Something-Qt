#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace something::application {
class ApplicationServiceGraph;
}

namespace something::controllers {

class ApplicationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString lifecycleState READ lifecycleState NOTIFY stateChanged)
    Q_PROPERTY(QString startupErrorMessage READ startupErrorMessage NOTIFY stateChanged)
    Q_PROPERTY(QStringList unavailableCapabilities READ unavailableCapabilities NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool shutdownRequested READ shutdownRequested NOTIFY shutdownRequestedChanged)
    Q_PROPERTY(QString shutdownErrorMessage READ shutdownErrorMessage NOTIFY stateChanged)

public:
    explicit ApplicationController(something::application::ApplicationServiceGraph *graph,
                                    QObject *parent = nullptr);

    QString lifecycleState() const;
    QString startupErrorMessage() const;
    QStringList unavailableCapabilities() const;
    bool shutdownRequested() const noexcept { return m_shutdownRequested; }
    QString shutdownErrorMessage() const { return m_shutdownErrorMessage; }

    Q_INVOKABLE void requestShutdown();
    Q_INVOKABLE void requestFullWindow();
    void reportShutdownPreparationResult(bool success, const QString &diagnostic);

signals:
    void stateChanged();
    void capabilitiesChanged();
    void shutdownRequestedChanged(bool requested);
    void shutdownPreparationRequested();
    void shutdownFinished();
    void fullWindowRequested();

private:
    void onGraphShutdownComplete();
    void beginGraphShutdown();

    something::application::ApplicationServiceGraph *m_graph{nullptr};
    QTimer m_shutdownPreparationTimer;
    QString m_shutdownErrorMessage;
    int m_shutdownPreparationsRemaining{0};
    bool m_shutdownPreparationFinished{false};
    bool m_shutdownRequested{false};
};

} // namespace something::controllers

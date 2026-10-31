#pragma once

#include <QObject>
#include <QStringList>

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

public:
    explicit ApplicationController(something::application::ApplicationServiceGraph *graph,
                                    QObject *parent = nullptr);

    QString lifecycleState() const;
    QString startupErrorMessage() const;
    QStringList unavailableCapabilities() const;
    bool shutdownRequested() const noexcept { return m_shutdownRequested; }

    Q_INVOKABLE void requestShutdown();
    Q_INVOKABLE void requestFullWindow();

signals:
    void stateChanged();
    void capabilitiesChanged();
    void shutdownRequestedChanged(bool requested);
    void shutdownFinished();
    void fullWindowRequested();

private:
    void onGraphShutdownComplete();

    something::application::ApplicationServiceGraph *m_graph{nullptr};
    bool m_shutdownRequested{false};
};

} // namespace something::controllers

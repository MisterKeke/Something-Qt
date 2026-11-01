#include "application_controller.h"

#include "../application/application_service_graph.h"
#include "../application/capability_registry.h"
#include "../application/application_lifecycle.h"

#include <QThread>
#include <QDebug>

namespace something::controllers {

ApplicationController::ApplicationController(
    something::application::ApplicationServiceGraph *graph,
    QObject *parent)
    : QObject(parent)
    , m_graph(graph)
{
    Q_ASSERT(m_graph);
    Q_ASSERT(QThread::currentThread() == thread());

    connect(&m_graph->lifecycle(), &something::application::ApplicationLifecycle::stateChanged,
            this, &ApplicationController::stateChanged);
    connect(&m_graph->lifecycle(), &something::application::ApplicationLifecycle::startupErrorChanged,
            this, &ApplicationController::stateChanged);
    connect(&m_graph->capabilities(), &something::application::CapabilityRegistry::capabilitiesChanged,
            this, &ApplicationController::capabilitiesChanged);
    connect(m_graph, &something::application::ApplicationServiceGraph::shutdownComplete,
            this, &ApplicationController::onGraphShutdownComplete);
    m_shutdownPreparationTimer.setSingleShot(true);
    m_shutdownPreparationTimer.setInterval(8000);
    connect(&m_shutdownPreparationTimer, &QTimer::timeout, this, [this] {
        if (m_shutdownPreparationFinished) return;
        m_shutdownErrorMessage = tr("Timed out while saving final application state.");
        qWarning().noquote() << "Shutdown preparation timed out;" << m_shutdownErrorMessage;
        emit stateChanged();
        m_shutdownPreparationsRemaining = 0;
        beginGraphShutdown();
    });
}

QString ApplicationController::lifecycleState() const
{
    return m_graph ? m_graph->lifecycle().stateName() : QStringLiteral("stopped");
}

QString ApplicationController::startupErrorMessage() const
{
    return m_graph ? m_graph->lifecycle().startupError().userMessage() : QString();
}

QStringList ApplicationController::unavailableCapabilities() const
{
    return m_graph ? m_graph->capabilities().unavailableCapabilities() : QStringList{};
}

void ApplicationController::requestFullWindow()
{
    if (!m_shutdownRequested)
        emit fullWindowRequested();
}

void ApplicationController::requestShutdown()
{
    if (m_shutdownRequested || !m_graph) {
        return;
    }
    m_shutdownRequested = true;
    m_shutdownPreparationsRemaining = 2;
    emit shutdownRequestedChanged(true);
    if (m_graph->isShutdownComplete()) {
        emit shutdownFinished();
        return;
    }
    m_shutdownPreparationTimer.start();
    emit shutdownPreparationRequested();
}

void ApplicationController::reportShutdownPreparationResult(
    bool success, const QString &diagnostic)
{
    if (!m_shutdownRequested || m_shutdownPreparationFinished
        || m_shutdownPreparationsRemaining <= 0) return;
    if (!success) {
        m_shutdownErrorMessage = diagnostic.trimmed().isEmpty()
            ? tr("A final application state could not be saved.") : diagnostic.trimmed();
        qWarning().noquote() << "Shutdown preparation failed:" << m_shutdownErrorMessage;
        emit stateChanged();
    }
    --m_shutdownPreparationsRemaining;
    if (m_shutdownPreparationsRemaining == 0) beginGraphShutdown();
}

void ApplicationController::beginGraphShutdown()
{
    if (m_shutdownPreparationFinished) return;
    m_shutdownPreparationFinished = true;
    m_shutdownPreparationTimer.stop();
    if (m_graph->isShutdownComplete()) {
        emit shutdownFinished();
        return;
    }
    m_graph->stopAcceptingOperations();
    m_graph->shutdown();
}

void ApplicationController::onGraphShutdownComplete()
{
    if (m_shutdownRequested) {
        emit shutdownFinished();
    }
}

} // namespace something::controllers

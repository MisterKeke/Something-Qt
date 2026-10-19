#include "application_controller.h"

#include "../application/application_service_graph.h"
#include "../application/capability_registry.h"
#include "../application/application_lifecycle.h"

#include <QThread>

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

void ApplicationController::requestShutdown()
{
    if (m_shutdownRequested || !m_graph) {
        return;
    }
    m_shutdownRequested = true;
    if (m_graph->isShutdownComplete()) {
        emit shutdownFinished();
        return;
    }
    m_graph->shutdown();
}

void ApplicationController::onGraphShutdownComplete()
{
    if (m_shutdownRequested) {
        emit shutdownFinished();
    }
}

} // namespace something::controllers

#include "operation_coordinator.h"

#include <QLoggingCategory>
#include <QThread>

namespace something::application {
namespace {
Q_LOGGING_CATEGORY(lcPerformance, "something.performance", QtWarningMsg)
}

OperationCoordinator::OperationCoordinator(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<OperationId>();
    qRegisterMetaType<State>();
}

OperationContext OperationCoordinator::begin(QString purpose, QString freshnessKey)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_accepting || m_nextId == 0) {
        return {};
    }

    if (!freshnessKey.isEmpty()) {
        const auto previous = m_latestByKey.constFind(freshnessKey);
        if (previous != m_latestByKey.cend()) {
            cancelOnOwnerThread(OperationId{previous.value()});
        }
    }

    const OperationId id{m_nextId++};
    Entry entry;
    entry.purpose = purpose;
    entry.freshnessKey = freshnessKey;
    entry.performanceTiming = lcPerformance().isInfoEnabled();
    if (entry.performanceTiming) entry.startedAt = std::chrono::steady_clock::now();
    const CancellationToken token = entry.cancellation.token();
    m_operations.insert(id.value, std::move(entry));
    if (!freshnessKey.isEmpty()) {
        m_latestByKey.insert(freshnessKey, id.value);
    }

    emit operationStateChanged(id.value, State::Running);
    emit activeCountChanged();
    return OperationContext{id, std::move(purpose), std::move(freshnessKey), token};
}

bool OperationCoordinator::cancel(OperationId id)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_operations.contains(id.value)) {
        return false;
    }
    cancelOnOwnerThread(id);
    return true;
}

void OperationCoordinator::cancelOnOwnerThread(OperationId id)
{
    auto it = m_operations.find(id.value);
    if (it == m_operations.end() || it->state != State::Running) {
        return;
    }

    it->cancellation.requestCancellation();
    it->state = State::Cancelling;
    emit operationStateChanged(id.value, State::Cancelling);
    emit cancellationRequested(id.value);
}

void OperationCoordinator::beginShutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_accepting) {
        return;
    }

    m_accepting = false;
    emit acceptingOperationsChanged();
    const auto ids = m_operations.keys();
    for (quint64 value : ids) {
        cancelOnOwnerThread(OperationId{value});
    }
    if (m_operations.isEmpty()) {
        emit drained();
    }
}

bool OperationCoordinator::finish(OperationId id, something::domain::AppError error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    auto it = m_operations.find(id.value);
    if (it == m_operations.end()) {
        return false;
    }

    const bool cancelled = it->state == State::Cancelling || error.isCancelled();
    const State finalState = cancelled ? State::Cancelled
                                      : (error.isError() ? State::Failed : State::Succeeded);
    if (cancelled) {
        error = something::domain::AppError::cancelled();
    }

    const QString key = it->freshnessKey;
    const bool performanceTiming = it->performanceTiming;
    const QString purpose = performanceTiming ? it->purpose : QString();
    const qint64 elapsedMs = performanceTiming
        ? std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - it->startedAt).count()
        : 0;
    m_operations.erase(it);
    if (!key.isEmpty() && m_latestByKey.value(key) == id.value) {
        m_latestByKey.remove(key);
    }

    emit operationStateChanged(id.value, finalState);
    if (performanceTiming) {
        qCInfo(lcPerformance).nospace()
            << "operation id=" << id.value << " purpose=\"" << purpose
            << "\" elapsed_ms=" << elapsedMs
            << " outcome=" << static_cast<int>(finalState);
    }
    emit operationFinished(id.value, finalState, error);
    emit activeCountChanged();
    if (m_operations.isEmpty()) {
        emit drained();
    }
    return true;
}

bool OperationCoordinator::acceptsResult(OperationId id) const
{
    const auto it = m_operations.constFind(id.value);
    if (it == m_operations.cend() || it->state != State::Running ||
        it->cancellation.token().isCancellationRequested()) {
        return false;
    }
    return it->freshnessKey.isEmpty() || m_latestByKey.value(it->freshnessKey) == id.value;
}

} // namespace something::application

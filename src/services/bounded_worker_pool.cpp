#include "bounded_worker_pool.h"

#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QRunnable>

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace something::services {
namespace {

Q_LOGGING_CATEGORY(lcPerformance, "something.performance", QtWarningMsg)
using SteadyClock = std::chrono::steady_clock;

class OperationRunnable final : public QRunnable {
public:
    OperationRunnable(QPointer<BoundedWorkerPool> owner,
                      something::application::OperationContext operation,
                      BoundedWorkerPool::Work work,
                      SteadyClock::time_point queuedAt)
        : m_owner(owner)
        , m_operation(std::move(operation))
        , m_work(std::move(work))
        , m_queuedAt(queuedAt)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        const bool performanceTiming = lcPerformance().isInfoEnabled();
        const auto startedAt = performanceTiming ? SteadyClock::now() : SteadyClock::time_point{};
        something::domain::AppError result;
        if (m_operation.cancellation.isCancellationRequested()) {
            result = something::domain::AppError::cancelled();
        } else {
            try {
                result = m_work(m_operation.cancellation);
            } catch (const std::exception &) {
                result = something::domain::AppError(something::domain::ErrorCategory::Internal);
            } catch (...) {
                result = something::domain::AppError(something::domain::ErrorCategory::Internal);
            }
        }

        if (m_operation.cancellation.isCancellationRequested()) {
            result = something::domain::AppError::cancelled();
        }

        if (performanceTiming) {
            qCInfo(lcPerformance).nospace()
                << "cpu_job id=" << m_operation.id.value
                << " queue_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                       startedAt - m_queuedAt).count()
                << " work_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                       SteadyClock::now() - startedAt).count();
        }

        const QPointer<BoundedWorkerPool> owner = m_owner;
        const quint64 operationId = m_operation.id.value;
        QMetaObject::invokeMethod(owner, [owner, operationId, result] {
            if (owner) {
                owner->finishOnOwnerThread(operationId, result);
            }
        }, Qt::QueuedConnection);
    }

private:
    QPointer<BoundedWorkerPool> m_owner;
    something::application::OperationContext m_operation;
    BoundedWorkerPool::Work m_work;
    SteadyClock::time_point m_queuedAt;
};

class ValueOperationRunnable final : public QRunnable {
public:
    ValueOperationRunnable(QPointer<BoundedWorkerPool> owner,
                           something::application::OperationContext operation,
                           BoundedWorkerPool::ValueWork work,
                           SteadyClock::time_point queuedAt)
        : m_owner(owner), m_operation(std::move(operation)), m_work(std::move(work)),
          m_queuedAt(queuedAt)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        const bool performanceTiming = lcPerformance().isInfoEnabled();
        const auto startedAt = performanceTiming ? SteadyClock::now() : SteadyClock::time_point{};
        BoundedWorkerPool::ValueResult result;
        if (m_operation.cancellation.isCancellationRequested()) {
            result.error = something::domain::AppError::cancelled();
        } else {
            try {
                result = m_work(m_operation.cancellation);
            } catch (const std::exception &) {
                result.error = something::domain::AppError(something::domain::ErrorCategory::Internal);
            } catch (...) {
                result.error = something::domain::AppError(something::domain::ErrorCategory::Internal);
            }
        }
        if (m_operation.cancellation.isCancellationRequested()) {
            result.values.clear();
            result.error = something::domain::AppError::cancelled();
        }

        if (performanceTiming) {
            qCInfo(lcPerformance).nospace()
                << "cpu_value_job id=" << m_operation.id.value
                << " queue_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                       startedAt - m_queuedAt).count()
                << " work_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                       SteadyClock::now() - startedAt).count();
        }

        const QPointer<BoundedWorkerPool> owner = m_owner;
        const quint64 operationId = m_operation.id.value;
        QMetaObject::invokeMethod(owner, [owner, operationId, result = std::move(result)]() mutable {
            if (owner) owner->finishValueOnOwnerThread(operationId,
                                                       std::move(result.values), result.error);
        }, Qt::QueuedConnection);
    }

private:
    QPointer<BoundedWorkerPool> m_owner;
    something::application::OperationContext m_operation;
    BoundedWorkerPool::ValueWork m_work;
    SteadyClock::time_point m_queuedAt;
};

} // namespace

BoundedWorkerPool::BoundedWorkerPool(int maximumThreads, QObject *parent)
    : QObject(parent)
{
    const int ideal = std::max(1, QThread::idealThreadCount());
    const int requested = maximumThreads > 0 ? maximumThreads : ideal;
    m_pool.setMaxThreadCount(std::clamp(requested, 1, 4));
    m_pool.setExpiryTimeout(30000);
}

BoundedWorkerPool::~BoundedWorkerPool()
{
    m_pool.waitForDone();
}

bool BoundedWorkerPool::submit(const something::application::OperationContext &operation,
                               Work work)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_accepting || !operation.isValid() || !work) {
        return false;
    }
    const int outstanding = m_outstanding.load(std::memory_order_acquire);
    if (outstanding >= maximumOutstandingJobs()) {
        qCWarning(lcPerformance) << "cpu_queue_full outstanding=" << outstanding;
        return false;
    }

    m_outstanding.fetch_add(1, std::memory_order_acq_rel);
    emit outstandingJobsChanged();
    const auto queuedAt = lcPerformance().isInfoEnabled()
        ? SteadyClock::now() : SteadyClock::time_point{};
    m_pool.start(new OperationRunnable(this, operation, std::move(work), queuedAt));
    return true;
}

bool BoundedWorkerPool::submitValue(const something::application::OperationContext &operation,
                                   ValueWork work)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_accepting || !operation.isValid() || !work) return false;
    const int outstanding = m_outstanding.load(std::memory_order_acquire);
    if (outstanding >= maximumOutstandingJobs()) {
        qCWarning(lcPerformance) << "cpu_queue_full outstanding=" << outstanding;
        return false;
    }
    m_outstanding.fetch_add(1, std::memory_order_acq_rel);
    emit outstandingJobsChanged();
    const auto queuedAt = lcPerformance().isInfoEnabled()
        ? SteadyClock::now() : SteadyClock::time_point{};
    m_pool.start(new ValueOperationRunnable(this, operation, std::move(work), queuedAt));
    return true;
}

void BoundedWorkerPool::beginShutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_accepting = false;
    if (isDrained()) {
        emit drained();
    }
}

void BoundedWorkerPool::finishOnOwnerThread(quint64 operationId,
                                            something::domain::AppError error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_outstanding.fetch_sub(1, std::memory_order_acq_rel);
    emit jobFinished(operationId, error);
    emit outstandingJobsChanged();
    if (isDrained()) {
        emit drained();
    }
}

void BoundedWorkerPool::finishValueOnOwnerThread(quint64 operationId,
                                                 QVariantMap values,
                                                 something::domain::AppError error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_outstanding.fetch_sub(1, std::memory_order_acq_rel);
    emit valueJobFinished(operationId, std::move(values), error);
    emit outstandingJobsChanged();
    if (isDrained()) emit drained();
}

} // namespace something::services

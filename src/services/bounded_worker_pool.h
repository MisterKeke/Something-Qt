#pragma once

#include "../application/operation_coordinator.h"

#include <QObject>
#include <QThreadPool>
#include <QVariantMap>
#include <atomic>
#include <functional>

namespace something::services {

class BoundedWorkerPool final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int outstandingJobs READ outstandingJobs NOTIFY outstandingJobsChanged)

public:
    using Work = std::function<something::domain::AppError(
        const something::application::CancellationToken &)>;
    struct ValueResult final {
        QVariantMap values;
        something::domain::AppError error;
    };
    using ValueWork = std::function<ValueResult(
        const something::application::CancellationToken &)>;

    explicit BoundedWorkerPool(int maximumThreads = 0, QObject *parent = nullptr);
    ~BoundedWorkerPool() override;

    static constexpr int maximumOutstandingJobs() noexcept { return 64; }

    bool submit(const something::application::OperationContext &operation, Work work);
    bool submitValue(const something::application::OperationContext &operation, ValueWork work);
    void beginShutdown();
    bool isDrained() const noexcept { return m_outstanding.load(std::memory_order_acquire) == 0; }
    int outstandingJobs() const noexcept { return m_outstanding.load(std::memory_order_acquire); }
    int maximumThreads() const noexcept { return m_pool.maxThreadCount(); }
    void finishOnOwnerThread(quint64 operationId, something::domain::AppError error);
    void finishValueOnOwnerThread(quint64 operationId, QVariantMap values,
                                  something::domain::AppError error);

signals:
    void jobFinished(quint64 operationId, something::domain::AppError error);
    void valueJobFinished(quint64 operationId, QVariantMap values,
                          something::domain::AppError error);
    void outstandingJobsChanged();
    void drained();

private:
    QThreadPool m_pool;
    std::atomic_int m_outstanding{0};
    bool m_accepting{true};
};

} // namespace something::services

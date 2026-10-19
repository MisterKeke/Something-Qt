#pragma once

#include "cancellation_token.h"
#include "../domain/app_error.h"

#include <QObject>
#include <QHash>
#include <QString>
#include <chrono>

namespace something::application {

struct OperationId final {
    quint64 value{0};

    constexpr bool isValid() const noexcept { return value != 0; }
    friend constexpr bool operator==(OperationId left, OperationId right) noexcept
    {
        return left.value == right.value;
    }
};

struct OperationContext final {
    OperationId id;
    QString purpose;
    QString freshnessKey;
    CancellationToken cancellation;

    bool isValid() const noexcept { return id.isValid(); }
};

class OperationCoordinator final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeCountChanged)
    Q_PROPERTY(bool acceptingOperations READ acceptingOperations NOTIFY acceptingOperationsChanged)

public:
    enum class State {
        Running,
        Cancelling,
        Succeeded,
        Failed,
        Cancelled,
    };
    Q_ENUM(State)

    explicit OperationCoordinator(QObject *parent = nullptr);

    OperationContext begin(QString purpose, QString freshnessKey = {});
    bool cancel(OperationId id);
    void beginShutdown();
    bool finish(OperationId id, something::domain::AppError error = {});

    bool acceptsResult(OperationId id) const;
    int activeCount() const noexcept { return m_operations.size(); }
    bool acceptingOperations() const noexcept { return m_accepting; }

signals:
    void operationStateChanged(quint64 id, something::application::OperationCoordinator::State state);
    void operationFinished(quint64 id,
                           something::application::OperationCoordinator::State state,
                           something::domain::AppError error);
    void cancellationRequested(quint64 id);
    void activeCountChanged();
    void acceptingOperationsChanged();
    void drained();

private:
    struct Entry final {
        QString purpose;
        QString freshnessKey;
        CancellationSource cancellation;
        State state{State::Running};
        std::chrono::steady_clock::time_point startedAt;
        bool performanceTiming{false};
    };

    void cancelOnOwnerThread(OperationId id);

    quint64 m_nextId{1};
    bool m_accepting{true};
    QHash<quint64, Entry> m_operations;
    QHash<QString, quint64> m_latestByKey;
};

} // namespace something::application

Q_DECLARE_METATYPE(something::application::OperationId)

#pragma once

#include "../domain/app_error.h"
#include "repositories.h"

#include <QObject>
#include <QMetaType>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace something::storage {

struct StorageReply {
    quint64 operationId{0};
    QVariantMap values;
    domain::AppError error{};
};

using StorageCommand = std::function<StorageReply(StorageRepositories &, const std::function<bool()> &)>;

class StorageWorker final : public QObject {
    Q_OBJECT

public:
    enum class SubmitResult { Accepted, Unavailable, QueueFull, DispatchFailed };

    static constexpr int maximumPendingOperations() noexcept { return 64; }

    explicit StorageWorker(QObject *parent = nullptr);
    ~StorageWorker() override;

    void open(QString connectionName, QString databasePath);
    void close();
    SubmitResult submit(quint64 operationId, StorageCommand command,
                        std::function<bool()> cancellationRequested = {});

signals:
    void opened(something::domain::AppError error);
    void closed(something::domain::AppError error);
    void operationFinished(something::storage::StorageReply reply);

private:
    void removeConnection();

    QString m_connectionName;
    QSqlDatabase m_database;
    std::unique_ptr<StorageRepositories> m_repositories;
    std::atomic_bool m_ready{false};
    std::atomic_bool m_accepting{false};
    std::mutex m_admissionMutex;
    int m_pendingOperations{0};
    bool m_closeDeferred{false}; // storage-thread only
};

} // namespace something::storage

Q_DECLARE_METATYPE(something::storage::StorageReply)

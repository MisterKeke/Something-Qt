#include "storage_worker.h"
#include "schema_migrator.h"

#include <QMetaObject>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>

#include <exception>
#include <utility>

namespace something::storage {
namespace {

Q_LOGGING_CATEGORY(lcPerformance, "something.performance", QtWarningMsg)

bool isDetachedValue(const QVariant &value)
{
    if (!value.isValid() || value.isNull()) return true;
    switch (value.metaType().id()) {
    case QMetaType::Bool:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
    case QMetaType::QString:
    case QMetaType::QStringList:
    case QMetaType::QByteArray:
        return true;
    case QMetaType::QVariantList:
        for (const auto &item : value.toList()) {
            if (!isDetachedValue(item)) return false;
        }
        return true;
    case QMetaType::QVariantMap:
    {
        const auto map = value.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            if (!isDetachedValue(it.value())) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

bool areDetachedValues(const QVariantMap &values)
{
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!isDetachedValue(it.value())) return false;
    }
    return true;
}

} // namespace

StorageWorker::StorageWorker(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<StorageReply>("something::storage::StorageReply");
}

StorageWorker::~StorageWorker()
{
    Q_ASSERT(!m_database.isValid());
}

void StorageWorker::open(QString connectionName, QString databasePath)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_database.isValid() || connectionName.isEmpty() || databasePath.isEmpty()) {
        emit opened(something::domain::AppError(something::domain::ErrorCategory::Storage));
        return;
    }

    m_connectionName = std::move(connectionName);
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    database.setDatabaseName(std::move(databasePath));
    if (!database.open()) {
        database = QSqlDatabase();
        removeConnection();
        emit opened(something::domain::AppError(something::domain::ErrorCategory::Storage));
        return;
    }

    bool foreignKeysEnabled = false;
    {
        QSqlQuery foreignKeys(database);
        foreignKeysEnabled = foreignKeys.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    }
    if (!foreignKeysEnabled) {
        database.close();
        database = QSqlDatabase();
        removeConnection();
        emit opened(something::domain::AppError(something::domain::ErrorCategory::Storage));
        return;
    }

    QString migrationDiagnostic;
    if (!SchemaMigrator::apply(database, &migrationDiagnostic)) {
        database.close();
        database = QSqlDatabase();
        removeConnection();
        emit opened(something::domain::AppError(something::domain::ErrorCategory::Storage));
        return;
    }

    m_database = database;
    database = QSqlDatabase();
    m_repositories = std::make_unique<StorageRepositories>(m_database);
    {
        std::lock_guard lock(m_admissionMutex);
        m_ready.store(true, std::memory_order_release);
        m_accepting.store(true, std::memory_order_release);
    }
    emit opened(something::domain::AppError::none());
}

void StorageWorker::close()
{
    Q_ASSERT(QThread::currentThread() == thread());
    bool deferClose = false;
    {
        std::lock_guard lock(m_admissionMutex);
        m_accepting.store(false, std::memory_order_release);
        if (m_pendingOperations != 0 && !m_closeDeferred) {
            m_closeDeferred = true;
            deferClose = true;
        }
    }
    if (deferClose) {
        // Already accepted commands are serialized ahead of this queued retry.
        QMetaObject::invokeMethod(this, [this] {
            m_closeDeferred = false;
            close();
        }, Qt::QueuedConnection);
        return;
    }
    {
        std::lock_guard lock(m_admissionMutex);
        if (m_pendingOperations != 0) {
            // A retry can race with an already accepted producer. Requeue
            // instead of closing SQLite beneath work.
            if (!m_closeDeferred) {
                m_closeDeferred = true;
                QMetaObject::invokeMethod(this, [this] {
                    m_closeDeferred = false;
                    close();
                }, Qt::QueuedConnection);
            }
            return;
        }
    }
    m_ready.store(false, std::memory_order_release);
    // Repositories contain references to the connection. Destroy them before
    // releasing the worker's final QSqlDatabase handle.
    m_repositories.reset();
    if (m_database.isValid()) {
        m_database.close();
        m_database = QSqlDatabase();
    }
    removeConnection();
    emit closed(something::domain::AppError::none());
}

StorageWorker::SubmitResult StorageWorker::submit(
    quint64 operationId, StorageCommand command,
    std::function<bool()> cancellationRequested)
{
    if (operationId == 0 || !command) {
        return SubmitResult::DispatchFailed;
    }
    {
        std::lock_guard lock(m_admissionMutex);
        if (!m_accepting.load(std::memory_order_acquire)
            || !m_ready.load(std::memory_order_acquire)) {
            return SubmitResult::Unavailable;
        }
        if (m_pendingOperations >= maximumPendingOperations()) {
            qCWarning(lcPerformance) << "storage_queue_full pending=" << m_pendingOperations;
            return SubmitResult::QueueFull;
        }
        ++m_pendingOperations;
        if (m_pendingOperations > 1) {
            qCInfo(lcPerformance) << "storage_queue pending=" << m_pendingOperations;
        }
        const bool queued = QMetaObject::invokeMethod(this,
            [this, operationId, command = std::move(command),
             cancellationRequested = std::move(cancellationRequested)]() mutable {
                Q_ASSERT(QThread::currentThread() == thread());
                StorageReply reply;
                reply.operationId = operationId;
                const std::function<bool()> isCancelled = cancellationRequested
                    ? cancellationRequested : std::function<bool()>([] { return false; });
                if (isCancelled()) {
                    reply.error = domain::AppError::cancelled();
                } else if (!m_repositories || !m_database.isOpen()) {
                    reply.error = domain::AppError(domain::ErrorCategory::Storage);
                } else {
                    try {
                        reply = command(*m_repositories, isCancelled);
                        reply.operationId = operationId;
                        if (!areDetachedValues(reply.values)) {
                            reply.values.clear();
                            reply.error = domain::AppError(domain::ErrorCategory::Internal);
                        }
                    } catch (const std::exception &) {
                        reply.error = domain::AppError(domain::ErrorCategory::Internal);
                    } catch (...) {
                        reply.error = domain::AppError(domain::ErrorCategory::Internal);
                    }
                }
                {
                    std::lock_guard pendingLock(m_admissionMutex);
                    Q_ASSERT(m_pendingOperations > 0);
                    --m_pendingOperations;
                }
                emit operationFinished(std::move(reply));
            }, Qt::QueuedConnection);
        if (!queued) {
            --m_pendingOperations;
            return SubmitResult::DispatchFailed;
        }
    }
    return SubmitResult::Accepted;
}

void StorageWorker::removeConnection()
{
    if (!m_connectionName.isEmpty()) {
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
    }
}

} // namespace something::storage

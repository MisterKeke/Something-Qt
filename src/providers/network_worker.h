#pragma once

#include "provider_policy.h"
#include "../application/operation_coordinator.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace something::providers {

struct ProviderRequest final {
    ProviderId provider{ProviderId::Telegram};
    QUrl url;
    // Refresh bypasses any provider-value cache and uses a separate in-flight
    // key from an ordinary request. Concurrent identical refreshes still join.
    bool bypassCache{false};
    // A caller may shorten a provider deadline, never extend its policy limit.
    int deadlineMs{0};
    // The Frankfurter single-pair endpoint returns 404 for a valid pair with
    // no available quote. This opt-in is rejected for every other endpoint.
    bool allowMissingRate{false};
};

class NetworkWorker final : public QObject {
    Q_OBJECT

public:
    using ManagerFactory = std::function<QNetworkAccessManager *(QObject *parent)>;

    explicit NetworkWorker(QObject *parent = nullptr, ManagerFactory managerFactory = {});

    void initialize();
    void get(something::application::OperationContext operation, ProviderRequest request);
    void cancel(quint64 operationId);
    void shutdown();

signals:
    void ready();
    void initializationFailed(something::domain::AppError error);
    void requestFinished(quint64 operationId,
                         QByteArray body,
                         something::domain::AppError error);
    void drained();

private:
    struct Subscriber final {
        something::application::OperationContext operation;
    };

    struct RequestGroup final {
        ProviderRequest request;
        ProviderPolicy policy;
        QUrl initialUrl;
        QString originalHost;
        QByteArray body;
        QPointer<QNetworkReply> reply;
        QPointer<QTimer> deadline;
        QElapsedTimer lifetime;
        QHash<quint64, Subscriber> subscribers;
        something::domain::AppError terminalError;
        int redirects{0};
        bool active{false};
    };

    static QString requestKey(const ProviderRequest &request);
    static QNetworkRequest makeNetworkRequest(const ProviderRequest &request);
    void pumpQueue();
    void startRequest(const QString &key);
    void readAvailable(const QString &key);
    void onReplyFinished(const QString &key);
    void onDeadline(const QString &key);
    void finishGroup(const QString &key, something::domain::AppError error);
    void removeQueuedKey(const QString &key);
    void finishShutdownIfDrained();

    QNetworkAccessManager *m_manager{nullptr};
    ManagerFactory m_managerFactory;
    QHash<QString, RequestGroup> m_groups;
    QHash<quint64, QString> m_groupForOperation;
    QQueue<QString> m_queue;
    QSet<QString> m_activeKeys;
    bool m_shuttingDown{false};
    bool m_drainEmitted{false};
};

} // namespace something::providers

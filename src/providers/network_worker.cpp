#include "network_worker.h"

#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QThread>

#include <algorithm>
#include <utility>

namespace something::providers {
namespace {

Q_LOGGING_CATEGORY(lcPerformance, "something.performance", QtWarningMsg)

constexpr int kMaximumQueuedGroups = 32;

something::domain::AppError error(something::domain::ErrorCategory category)
{
    return something::domain::AppError(category);
}

bool isFrankfurterPairRateUrl(const QUrl &url)
{
    static const QRegularExpression pathPattern(
        QStringLiteral("^/v2/rate/[A-Z]{3}/[A-Z]{3}$"));
    return url.query().isEmpty() && pathPattern.match(url.path()).hasMatch();
}

} // namespace

NetworkWorker::NetworkWorker(QObject *parent, ManagerFactory managerFactory)
    : QObject(parent)
    , m_managerFactory(std::move(managerFactory))
{
}

void NetworkWorker::initialize()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_manager || m_shuttingDown) {
        return;
    }
    m_manager = m_managerFactory ? m_managerFactory(this) : new QNetworkAccessManager(this);
    if (!m_manager) {
        m_shuttingDown = true;
        m_drainEmitted = true;
        emit initializationFailed(
            something::domain::AppError(something::domain::ErrorCategory::CapabilityUnavailable));
        emit drained();
        return;
    }
    emit ready();
}

void NetworkWorker::get(something::application::OperationContext operation,
                        ProviderRequest request)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const quint64 operationId = operation.id.value;
    if (!operation.isValid() || m_shuttingDown || operation.cancellation.isCancellationRequested()) {
        emit requestFinished(operationId, {}, error(something::domain::ErrorCategory::Cancelled));
        return;
    }
    if (!m_manager) {
        emit requestFinished(operationId, {},
                             error(something::domain::ErrorCategory::CapabilityUnavailable));
        return;
    }
    if (m_groupForOperation.contains(operationId)) {
        emit requestFinished(operationId, {}, error(something::domain::ErrorCategory::Conflict));
        return;
    }

    const ProviderPolicy policy = providerPolicy(request.provider);
    if (!isAllowedProviderUrl(request.provider, request.url)) {
        emit requestFinished(operationId, {}, error(something::domain::ErrorCategory::Validation));
        return;
    }
    if (request.allowMissingRate
        && (request.provider != ProviderId::Frankfurter
            || !isFrankfurterPairRateUrl(request.url))) {
        emit requestFinished(operationId, {}, error(something::domain::ErrorCategory::Validation));
        return;
    }
    const int deadlineMs = request.deadlineMs <= 0
        ? policy.timeoutMs
        : std::min(request.deadlineMs, policy.timeoutMs);
    request.deadlineMs = deadlineMs;
    const QString key = requestKey(request);
    if (m_groups.size() >= kMaximumQueuedGroups + policy.maximumConcurrentRequests &&
        !m_groups.contains(key)) {
        qCWarning(lcPerformance) << "network_queue_full groups=" << m_groups.size();
        emit requestFinished(operationId, {},
                             error(something::domain::ErrorCategory::Busy));
        return;
    }

    auto existing = m_groups.find(key);
    if (existing != m_groups.end()) {
        existing->subscribers.insert(operationId, Subscriber{std::move(operation)});
        m_groupForOperation.insert(operationId, key);
        qCInfo(lcPerformance) << "network_request_join groups=" << m_groups.size()
                              << " subscribers=" << existing->subscribers.size();
        return;
    }

    RequestGroup group;
    group.policy = policy;
    group.originalHost = request.url.host().toLower();
    group.initialUrl = request.url;
    group.request = std::move(request);
    group.subscribers.insert(operationId, Subscriber{std::move(operation)});
    group.deadline = new QTimer(this);
    group.deadline->setSingleShot(true);
    connect(group.deadline, &QTimer::timeout, this, [this, key] { onDeadline(key); });
    group.deadline->start(deadlineMs);
    if (lcPerformance().isInfoEnabled()) group.lifetime.start();

    m_groups.insert(key, std::move(group));
    m_groupForOperation.insert(operationId, key);
    m_queue.enqueue(key);
    qCInfo(lcPerformance) << "network_request_queued groups=" << m_groups.size()
                          << " active=" << m_activeKeys.size();
    pumpQueue();
}

void NetworkWorker::cancel(quint64 operationId)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto operationIt = m_groupForOperation.find(operationId);
    if (operationIt == m_groupForOperation.end()) {
        return;
    }

    const QString key = operationIt.value();
    m_groupForOperation.erase(operationIt);
    auto groupIt = m_groups.find(key);
    if (groupIt == m_groups.end()) {
        emit requestFinished(operationId, {}, error(something::domain::ErrorCategory::Cancelled));
        return;
    }

    groupIt->subscribers.remove(operationId);
    emit requestFinished(operationId, {}, error(something::domain::ErrorCategory::Cancelled));
    if (!groupIt->subscribers.isEmpty()) {
        return;
    }

    groupIt->terminalError = error(something::domain::ErrorCategory::Cancelled);
    if (!groupIt->active) {
        finishGroup(key, groupIt->terminalError);
        pumpQueue();
    } else if (groupIt->reply) {
        groupIt->reply->abort();
    } else {
        finishGroup(key, groupIt->terminalError);
        pumpQueue();
    }
}

void NetworkWorker::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shuttingDown) {
        finishShutdownIfDrained();
        return;
    }
    m_shuttingDown = true;

    const auto keys = m_groups.keys();
    for (const QString &key : keys) {
        auto it = m_groups.find(key);
        if (it == m_groups.end()) {
            continue;
        }
        it->terminalError = error(something::domain::ErrorCategory::Cancelled);
        const auto operationIds = it->subscribers.keys();
        for (quint64 id : operationIds) {
            m_groupForOperation.remove(id);
            emit requestFinished(id, {}, error(something::domain::ErrorCategory::Cancelled));
        }
        it->subscribers.clear();
        if (it->active && it->reply) {
            it->reply->abort();
        } else {
            finishGroup(key, it->terminalError);
        }
    }
    m_queue.clear();
    finishShutdownIfDrained();
}

QString NetworkWorker::requestKey(const ProviderRequest &request)
{
    const QString cacheMode = request.bypassCache ? QStringLiteral("refresh")
                                                  : QStringLiteral("cached");
    return QString::number(static_cast<int>(request.provider)) + QLatin1Char('|') +
        request.url.toString(QUrl::FullyEncoded) + QLatin1Char('|') + cacheMode + QLatin1Char('|') +
        QString::number(request.deadlineMs) + QLatin1Char('|') +
        (request.allowMissingRate ? QStringLiteral("missing-ok") : QStringLiteral("strict-status"));
}

QNetworkRequest NetworkWorker::makeNetworkRequest(const ProviderRequest &request)
{
    const ProviderPolicy policy = providerPolicy(request.provider);
    QNetworkRequest networkRequest(request.url);
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                QNetworkRequest::ManualRedirectPolicy);
    networkRequest.setRawHeader(QByteArrayLiteral("Accept"), policy.accept);
    networkRequest.setRawHeader(QByteArrayLiteral("User-Agent"), policy.userAgent);
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    networkRequest.setSslConfiguration(tls);
    if (!policy.acceptLanguage.isEmpty()) {
        networkRequest.setRawHeader(QByteArrayLiteral("Accept-Language"), policy.acceptLanguage);
    }
    return networkRequest;
}

void NetworkWorker::pumpQueue()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shuttingDown || !m_manager) {
        finishShutdownIfDrained();
        return;
    }

    while (!m_queue.isEmpty()) {
        const QString key = m_queue.head();
        auto it = m_groups.find(key);
        if (it == m_groups.end()) {
            m_queue.dequeue();
            continue;
        }
        if (m_activeKeys.size() >= it->policy.maximumConcurrentRequests) {
            return;
        }
        m_queue.dequeue();
        startRequest(key);
    }
}

void NetworkWorker::startRequest(const QString &key)
{
    auto it = m_groups.find(key);
    if (it == m_groups.end()) {
        return;
    }
    if (it->subscribers.isEmpty()) {
        finishGroup(key, error(something::domain::ErrorCategory::Cancelled));
        return;
    }

    for (auto subscriber = it->subscribers.begin(); subscriber != it->subscribers.end();) {
        if (subscriber->operation.cancellation.isCancellationRequested()) {
            const quint64 id = subscriber.key();
            subscriber = it->subscribers.erase(subscriber);
            m_groupForOperation.remove(id);
            emit requestFinished(id, {}, error(something::domain::ErrorCategory::Cancelled));
        } else {
            ++subscriber;
        }
    }
    if (it->subscribers.isEmpty()) {
        finishGroup(key, error(something::domain::ErrorCategory::Cancelled));
        return;
    }

    it->active = true;
    m_activeKeys.insert(key);
    qCInfo(lcPerformance) << "network_request_started active=" << m_activeKeys.size()
                          << " groups=" << m_groups.size();
    QNetworkReply *reply = m_manager->get(makeNetworkRequest(it->request));
    it->reply = reply;
    reply->setReadBufferSize(it->policy.maximumResponseBytes + 1);
    connect(reply, &QNetworkReply::readyRead, this, [this, key] { readAvailable(key); });
    connect(reply, &QNetworkReply::finished, this, [this, key] { onReplyFinished(key); });
}

void NetworkWorker::readAvailable(const QString &key)
{
    auto it = m_groups.find(key);
    if (it == m_groups.end() || !it->reply || it->terminalError.isError()) {
        return;
    }
    bool contentLengthOk = false;
    const qint64 contentLength = it->reply->header(QNetworkRequest::ContentLengthHeader)
                                     .toLongLong(&contentLengthOk);
    if (contentLengthOk && contentLength > it->policy.maximumResponseBytes) {
        it->terminalError = error(something::domain::ErrorCategory::ResponseTooLarge);
        it->reply->abort();
        return;
    }
    const QByteArray chunk = it->reply->readAll();
    if (chunk.size() > it->policy.maximumResponseBytes - it->body.size()) {
        it->terminalError = error(something::domain::ErrorCategory::ResponseTooLarge);
        it->reply->abort();
        return;
    }
    it->body.append(chunk);
}

void NetworkWorker::onReplyFinished(const QString &key)
{
    auto it = m_groups.find(key);
    if (it == m_groups.end() || !it->reply) {
        return;
    }

    QNetworkReply *reply = it->reply.data();
    bool legitimateMissingRate = false;
    if (it->terminalError.category() == something::domain::ErrorCategory::None) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QVariant redirectValue = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
        const QUrl redirect = redirectValue.toUrl();
        if (status >= 300 && status < 400 && redirect.isValid() && !redirect.isEmpty()) {
            const QUrl target = reply->url().resolved(redirect);
            if (it->redirects >= it->policy.maximumRedirects ||
                !isAllowedProviderUrl(it->request.provider, target, it->originalHost) ||
                (it->request.allowMissingRate && !isFrankfurterPairRateUrl(target))) {
                it->terminalError = error(something::domain::ErrorCategory::Provider);
            } else {
                ++it->redirects;
                it->request.url = target;
                it->body.clear();
                it->reply = nullptr;
                reply->deleteLater();
                startRequest(key);
                return;
            }
        } else if (status == 404 && it->request.allowMissingRate) {
            // Frankfurter's pair endpoint uses 404 to represent no available rate.
            legitimateMissingRate = true;
        } else if (status >= 300) {
            it->terminalError = error(something::domain::ErrorCategory::Http);
        } else if (reply->error() != QNetworkReply::NoError) {
            if (reply->error() == QNetworkReply::TimeoutError) {
                it->terminalError = error(something::domain::ErrorCategory::Timeout);
            } else if (reply->error() == QNetworkReply::OperationCanceledError) {
                it->terminalError = error(something::domain::ErrorCategory::Cancelled);
            } else {
                it->terminalError = error(something::domain::ErrorCategory::Connection);
            }
        } else if (status < 200 || status >= 300) {
            it->terminalError = error(something::domain::ErrorCategory::Connection);
        } else {
            bool contentLengthOk = false;
            const qint64 contentLength = reply->header(QNetworkRequest::ContentLengthHeader)
                                             .toLongLong(&contentLengthOk);
            if (contentLengthOk && contentLength > it->policy.maximumResponseBytes) {
                it->terminalError = error(something::domain::ErrorCategory::ResponseTooLarge);
            }
        }
    }

    if (it->terminalError.category() == something::domain::ErrorCategory::None) {
        readAvailable(key);
        if (legitimateMissingRate
            && it->terminalError.category() == something::domain::ErrorCategory::None) {
            it->body.clear();
        }
    }
    const auto terminalError = it->terminalError;
    finishGroup(key, terminalError);
    pumpQueue();
}

void NetworkWorker::onDeadline(const QString &key)
{
    auto it = m_groups.find(key);
    if (it == m_groups.end()) {
        return;
    }
    it->terminalError = error(something::domain::ErrorCategory::Timeout);
    if (it->active && it->reply) {
        it->reply->abort();
    } else {
        finishGroup(key, it->terminalError);
        pumpQueue();
    }
}

void NetworkWorker::finishGroup(const QString &key, something::domain::AppError terminalError)
{
    auto it = m_groups.find(key);
    if (it == m_groups.end()) {
        return;
    }

    RequestGroup group = std::move(it.value());
    m_groups.erase(it);
    m_activeKeys.remove(key);
    removeQueuedKey(key);
    if (group.deadline) {
        group.deadline->stop();
        group.deadline->deleteLater();
    }
    if (group.reply) {
        group.reply->deleteLater();
    }
    if (lcPerformance().isInfoEnabled()) {
        qCInfo(lcPerformance) << "network_request_finished elapsed_ms=" << group.lifetime.elapsed()
                              << " body_bytes=" << group.body.size()
                              << " outcome=" << static_cast<int>(terminalError.category());
    }

    for (auto subscriber = group.subscribers.cbegin(); subscriber != group.subscribers.cend(); ++subscriber) {
        const quint64 id = subscriber.key();
        m_groupForOperation.remove(id);
        const bool cancelled = subscriber->operation.cancellation.isCancellationRequested();
        const auto resultError = cancelled
            ? error(something::domain::ErrorCategory::Cancelled)
            : terminalError;
        emit requestFinished(id,
                             resultError.category() == something::domain::ErrorCategory::None
                                 ? group.body
                                 : QByteArray{},
                             resultError);
    }
    finishShutdownIfDrained();
}

void NetworkWorker::removeQueuedKey(const QString &key)
{
    if (m_queue.isEmpty()) {
        return;
    }
    QQueue<QString> retained;
    while (!m_queue.isEmpty()) {
        const QString queuedKey = m_queue.dequeue();
        if (queuedKey != key) {
            retained.enqueue(queuedKey);
        }
    }
    m_queue = std::move(retained);
}

void NetworkWorker::finishShutdownIfDrained()
{
    if (!m_shuttingDown || !m_groups.isEmpty() || m_drainEmitted) {
        return;
    }
    m_drainEmitted = true;
    delete m_manager;
    m_manager = nullptr;
    emit drained();
}

} // namespace something::providers

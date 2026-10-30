#include "application/operation_coordinator.h"
#include "domain/app_error.h"
#include "providers/network_worker.h"
#include "providers/provider_parsers.h"
#include "providers/provider_policy.h"
#include "providers/provider_response_cache.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>

#include <cstring>
#include <functional>
#include <utility>

using namespace something;

namespace {

QByteArray fixture(const QString &name)
{
    QFile file(QStringLiteral(":/provider-fixtures/") + name);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

struct FixtureResponse final {
    int status{200};
    QByteArray body;
    QUrl redirect;
    int delayMs{0};
    QNetworkReply::NetworkError networkError{QNetworkReply::NoError};
};

class FixtureReply final : public QNetworkReply {
public:
    FixtureReply(const QNetworkRequest &request,
                 FixtureResponse response,
                 std::function<void()> onFinished,
                 QObject *parent = nullptr)
        : QNetworkReply(parent)
        , m_response(std::move(response))
        , m_onFinished(std::move(onFinished))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, m_response.status);
        if (m_response.redirect.isValid()) {
            setAttribute(QNetworkRequest::RedirectionTargetAttribute, m_response.redirect);
        }
        if (m_response.networkError != QNetworkReply::NoError) {
            setError(m_response.networkError, QStringLiteral("fixture network error"));
        }
        setHeader(QNetworkRequest::ContentLengthHeader, m_response.body.size());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(m_response.delayMs, this, [this] { complete(); });
    }

    void abort() override
    {
        if (m_completed) {
            return;
        }
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("fixture aborted"));
        complete();
    }

    qint64 bytesAvailable() const override
    {
        return m_response.body.size() - m_offset + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        if (m_offset >= m_response.body.size()) {
            return -1;
        }
        const qint64 length = qMin(maximum, qint64(m_response.body.size()) - m_offset);
        std::memcpy(data, m_response.body.constData() + m_offset, size_t(length));
        m_offset += length;
        return length;
    }

private:
    void complete()
    {
        if (m_completed) {
            return;
        }
        m_completed = true;
        if (m_onFinished) {
            m_onFinished();
        }
        setFinished(true);
        emit readyRead();
        emit finished();
    }

    FixtureResponse m_response;
    std::function<void()> m_onFinished;
    qint64 m_offset{0};
    bool m_completed{false};
};

class FixtureManager final : public QNetworkAccessManager {
public:
    explicit FixtureManager(QObject *parent = nullptr)
        : QNetworkAccessManager(parent)
    {
    }

    QHash<QString, FixtureResponse> responses;
    QHash<QString, int> requestCounts;
    int activeRequests{0};
    int maximumActiveRequests{0};

protected:
    QNetworkReply *createRequest(Operation operation,
                                 const QNetworkRequest &request,
                                 QIODevice *outgoingData = nullptr) override
    {
        Q_UNUSED(operation)
        Q_UNUSED(outgoingData)
        const QString key = request.url().toString(QUrl::FullyEncoded);
        ++requestCounts[key];
        ++activeRequests;
        maximumActiveRequests = qMax(maximumActiveRequests, activeRequests);
        return new FixtureReply(request,
                                responses.value(key, FixtureResponse{503, {}, {}, 0}),
                                [this] { --activeRequests; }, this);
    }
};

QVariant errorAt(const QSignalSpy &spy, int index)
{
    return spy.at(index).at(2);
}

} // namespace

class ProviderInfrastructureTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<domain::AppError>();
        qRegisterMetaType<application::OperationId>();
    }

    void providerPoliciesRejectArbitraryDestinations()
    {
        QVERIFY(providers::isAllowedProviderUrl(
            providers::ProviderId::Telegram, QUrl(QStringLiteral("https://t.me/s/example"))));
        QVERIFY(providers::isAllowedProviderUrl(
            providers::ProviderId::SteamArtwork, QUrl(QStringLiteral("https://cdn.steamstatic.com/a.jpg"))));
        QVERIFY(!providers::isAllowedProviderUrl(
            providers::ProviderId::Telegram, QUrl(QStringLiteral("https://example.com/s/example"))));
        QVERIFY(!providers::isAllowedProviderUrl(
            providers::ProviderId::Telegram, QUrl(QStringLiteral("http://t.me/s/example"))));
        QVERIFY(!providers::isAllowedProviderUrl(
            providers::ProviderId::SteamStore, QUrl(QStringLiteral("https://store.steampowered.com/appdetails"))));
        QVERIFY(!providers::isAllowedProviderUrl(
            providers::ProviderId::YouTubeFeed,
            QUrl(QStringLiteral("https://not-youtube.example/feeds/videos.xml"))));
        QVERIFY(!providers::isAllowedProviderUrl(
            providers::ProviderId::YouTubeFeed,
            QUrl(QStringLiteral("https://www.youtube.com/feeds/videos.xml")),
            QStringLiteral("not-youtube.example")));
    }

    void jsonReaderDistinguishesMissingAndMalformedFields()
    {
        providers::ProviderJsonObject parsed;
        domain::AppError error;
        QVERIFY(providers::ProviderJsonObject::parse(fixture(QStringLiteral("json-success.json")),
                                                      &parsed, &error));
        QCOMPARE(error.category(), domain::ErrorCategory::None);
        QCOMPARE(parsed.string(QStringLiteral("name")).value, QStringLiteral("Portal 2"));
        QCOMPARE(parsed.number(QStringLiteral("price")).value, 12.5);
        QVERIFY(parsed.boolean(QStringLiteral("available")).value);
        QVERIFY(parsed.object(QStringLiteral("metadata")).isPresent());
        QVERIFY(parsed.array(QStringLiteral("tags")).isPresent());

        QVERIFY(providers::ProviderJsonObject::parse(fixture(QStringLiteral("json-missing-fields.json")),
                                                      &parsed, &error));
        QVERIFY(parsed.string(QStringLiteral("name")).isMissing());
        QVERIFY(parsed.string(QStringLiteral("available")).hasWrongType());
        QVERIFY(!providers::ProviderJsonObject::parse(fixture(QStringLiteral("json-malformed.json")),
                                                       &parsed, &error));
        QCOMPARE(error.category(), domain::ErrorCategory::InvalidResponse);
    }

    void telegramHtmlParserReturnsPlainTypedValuesAndEmptySuccess()
    {
        const auto parsed = providers::parseTelegramPosts(fixture(QStringLiteral("telegram-success.html")));
        QVERIFY(parsed.succeeded());
        QCOMPARE(parsed.items.size(), 2);
        QCOMPARE(parsed.items.first().postId, QStringLiteral("sample_channel/42"));
        QCOMPARE(parsed.items.last().text, QStringLiteral("Older & plain text"));
        QCOMPARE(parsed.items.last().views, QStringLiteral("10K"));
        QCOMPARE(parsed.items.last().images.size(), 1);
        QVERIFY(!parsed.items.last().text.contains(QStringLiteral("<")));

        const auto missing = providers::parseTelegramPosts(
            fixture(QStringLiteral("telegram-missing-fields.html")));
        QVERIFY(missing.succeeded());
        QCOMPARE(missing.items.size(), 1);
        QVERIFY(missing.items.first().date.isEmpty());
        QVERIFY(missing.items.first().images.isEmpty());

        const auto empty = providers::parseTelegramPosts(fixture(QStringLiteral("telegram-empty.html")));
        QVERIFY(empty.isLegitimateEmpty());
        QVERIFY(!providers::parseTelegramPosts({}).succeeded());
    }

    void youtubeHtmlAndAtomBoundariesHandleMissingAndMalformedData()
    {
        const QString expected = QStringLiteral("UC0000000000000000000000");
        QCOMPARE(providers::parseYouTubeChannelIdPage(
                     fixture(QStringLiteral("youtube-channel-page.html"))), expected);
        QCOMPARE(providers::parseYouTubeChannelIdPage(
                     fixture(QStringLiteral("youtube-channel-script.html"))), expected);

        const auto feed = providers::parseYouTubeFeed(
            fixture(QStringLiteral("youtube-feed-success.xml")), expected);
        QVERIFY(feed.succeeded());
        QCOMPARE(feed.items.size(), 1);
        QCOMPARE(feed.items.first().videoUrl,
                 QStringLiteral("https://www.youtube.com/watch?v=abcdefghijk"));
        QCOMPARE(feed.items.first().title, QStringLiteral("Sample & safe"));
        QCOMPARE(feed.items.first().views, QStringLiteral("123"));

        const auto empty = providers::parseYouTubeFeed(
            fixture(QStringLiteral("youtube-feed-empty.xml")), expected);
        QVERIFY(empty.isLegitimateEmpty());
        const auto malformed = providers::parseYouTubeFeed(
            fixture(QStringLiteral("youtube-feed-malformed.xml")), expected);
        QVERIFY(!malformed.succeeded());
        QCOMPARE(malformed.error.category(), domain::ErrorCategory::InvalidResponse);
    }

    void providerCacheHonorsRefreshTtlCapacityAndEmptyResults()
    {
        auto now = std::chrono::steady_clock::time_point{};
        providers::ProviderResponseCache cache([&now] { return now; });
        QByteArray value;
        cache.put(providers::ProviderCachePartition::TelegramPosts,
                  QStringLiteral("empty"), QByteArrayLiteral("[]"), 0);
        QVERIFY(!cache.get(providers::ProviderCachePartition::TelegramPosts,
                           QStringLiteral("empty"), &value));

        for (int index = 0; index < 128; ++index) {
            cache.put(providers::ProviderCachePartition::TelegramPosts,
                      QString::number(index), QByteArray::number(index));
        }
        QVERIFY(cache.get(providers::ProviderCachePartition::TelegramPosts,
                          QStringLiteral("0"), &value));
        cache.put(providers::ProviderCachePartition::TelegramPosts,
                  QStringLiteral("128"), QByteArrayLiteral("new"));
        QCOMPARE(cache.size(providers::ProviderCachePartition::TelegramPosts), 128);
        QVERIFY(cache.get(providers::ProviderCachePartition::TelegramPosts,
                          QStringLiteral("0"), &value));
        QVERIFY(!cache.get(providers::ProviderCachePartition::TelegramPosts,
                           QStringLiteral("1"), &value));
        QVERIFY(!cache.get(providers::ProviderCachePartition::TelegramPosts,
                           QStringLiteral("0"), &value, true));
        cache.invalidate(providers::ProviderCachePartition::TelegramPosts, QStringLiteral("0"));
        QVERIFY(!cache.get(providers::ProviderCachePartition::TelegramPosts,
                           QStringLiteral("0"), &value));

        for (int index = 0; index < 257; ++index) {
            cache.put(providers::ProviderCachePartition::YouTubeHandleResolution,
                      QString::number(index), QByteArrayLiteral("UC0000000000000000000000"));
        }
        QCOMPARE(cache.size(providers::ProviderCachePartition::YouTubeHandleResolution), 256);
        QVERIFY(!cache.get(providers::ProviderCachePartition::YouTubeHandleResolution,
                           QStringLiteral("0"), &value));

        now += std::chrono::minutes(6);
        QVERIFY(!cache.get(providers::ProviderCachePartition::TelegramPosts,
                           QStringLiteral("0"), &value));
    }

    void networkRedirectsAreRevalidatedAndHttpFailuresStayDistinct()
    {
        auto *manager = static_cast<FixtureManager *>(nullptr);
        const QByteArray feed = fixture(QStringLiteral("youtube-feed-empty.xml"));
        const QUrl base(QStringLiteral("https://www.youtube.com/feeds/videos.xml"));
        const QUrl redirectUrl(QStringLiteral("https://www.youtube.com/feeds/videos.xml?redirect=1"));
        const QUrl crossHostUrl(QStringLiteral("https://www.youtube.com/feeds/videos.xml?cross=1"));
        const QUrl httpErrorUrl(QStringLiteral("https://www.youtube.com/feeds/videos.xml?http=1"));
        const QUrl connectionErrorUrl(QStringLiteral("https://www.youtube.com/feeds/videos.xml?connection=1"));
        const QUrl redirectLimitUrl(QStringLiteral("https://www.youtube.com/feeds/videos.xml?limit=1"));

        providers::NetworkWorker worker(nullptr, [&manager](QObject *parent) {
            manager = new FixtureManager(parent);
            return manager;
        });
        QSignalSpy ready(&worker, &providers::NetworkWorker::ready);
        QSignalSpy finished(&worker, &providers::NetworkWorker::requestFinished);
        worker.initialize();
        QCOMPARE(ready.size(), 1);

        const QJsonObject sameRedirect = QJsonDocument::fromJson(
            fixture(QStringLiteral("network-redirect-same.json"))).object();
        const QJsonObject crossRedirect = QJsonDocument::fromJson(
            fixture(QStringLiteral("network-redirect-cross-host.json"))).object();
        const QJsonObject httpFailure = QJsonDocument::fromJson(
            fixture(QStringLiteral("network-http-failure.json"))).object();
        const QJsonObject connectionFailure = QJsonDocument::fromJson(
            fixture(QStringLiteral("network-connection-failure.json"))).object();
        manager->responses.insert(redirectUrl.toString(QUrl::FullyEncoded),
            FixtureResponse{sameRedirect.value(QStringLiteral("status")).toInt(), {},
                            QUrl(sameRedirect.value(QStringLiteral("location")).toString()), 0});
        manager->responses.insert(base.toString(QUrl::FullyEncoded), FixtureResponse{200, feed, {}, 0});
        manager->responses.insert(crossHostUrl.toString(QUrl::FullyEncoded),
            FixtureResponse{crossRedirect.value(QStringLiteral("status")).toInt(), {},
                            QUrl(crossRedirect.value(QStringLiteral("location")).toString()), 0});
        manager->responses.insert(httpErrorUrl.toString(QUrl::FullyEncoded),
            FixtureResponse{httpFailure.value(QStringLiteral("status")).toInt(),
                            httpFailure.value(QStringLiteral("body")).toString().toUtf8(), {}, 0});
        manager->responses.insert(connectionErrorUrl.toString(QUrl::FullyEncoded),
            FixtureResponse{connectionFailure.value(QStringLiteral("status")).toInt(), {}, {}, 0,
                            QNetworkReply::HostNotFoundError});
        manager->responses.insert(redirectLimitUrl.toString(QUrl::FullyEncoded),
            FixtureResponse{302, {}, QUrl(QStringLiteral("/feeds/videos.xml?limit=1")), 0});

        application::OperationCoordinator operations;
        const auto redirected = operations.begin(QStringLiteral("redirected provider read"));
        worker.get(redirected, {providers::ProviderId::YouTubeFeed, redirectUrl, false, 0});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        QCOMPARE(errorAt(finished, 0).value<domain::AppError>().category(), domain::ErrorCategory::None);
        QCOMPARE(finished.at(0).at(1).toByteArray(), feed);
        QCOMPARE(manager->requestCounts.value(redirectUrl.toString(QUrl::FullyEncoded)), 1);
        QCOMPARE(manager->requestCounts.value(base.toString(QUrl::FullyEncoded)), 1);

        const auto cross = operations.begin(QStringLiteral("cross-host redirect"));
        worker.get(cross, {providers::ProviderId::YouTubeFeed, crossHostUrl, false, 0});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 1000);
        QCOMPARE(errorAt(finished, 1).value<domain::AppError>().category(), domain::ErrorCategory::Provider);

        const auto throttled = operations.begin(QStringLiteral("HTTP failure"));
        worker.get(throttled, {providers::ProviderId::YouTubeFeed, httpErrorUrl, false, 0});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 1000);
        QCOMPARE(errorAt(finished, 2).value<domain::AppError>().category(), domain::ErrorCategory::Http);

        const auto connection = operations.begin(QStringLiteral("connection failure"));
        worker.get(connection, {providers::ProviderId::YouTubeFeed, connectionErrorUrl, false, 0});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 4, 1000);
        QCOMPARE(errorAt(finished, 3).value<domain::AppError>().category(), domain::ErrorCategory::Connection);

        const auto redirectLoop = operations.begin(QStringLiteral("redirect limit"));
        worker.get(redirectLoop, {providers::ProviderId::YouTubeFeed, redirectLimitUrl, false, 0});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 5, 1000);
        QCOMPARE(errorAt(finished, 4).value<domain::AppError>().category(), domain::ErrorCategory::Provider);
        QCOMPARE(manager->requestCounts.value(redirectLimitUrl.toString(QUrl::FullyEncoded)), 3);

        worker.shutdown();
    }

    void frankfurterMissingQuoteIsOptInAndPairPathScoped()
    {
        auto *manager = static_cast<FixtureManager *>(nullptr);
        const QUrl pairUrl(QStringLiteral("https://api.frankfurter.dev/v2/rate/USD/ZZZ"));
        const QUrl redirectPairUrl(QStringLiteral("https://api.frankfurter.dev/v2/rate/GBP/ZZZ"));
        const QUrl ratesUrl(QStringLiteral("https://api.frankfurter.dev/v2/rates?base=USD"));
        const QUrl malformedPair(QStringLiteral("https://api.frankfurter.dev/v2/rate/USD/ZZZ/extra"));
        providers::NetworkWorker worker(nullptr, [&manager](QObject *parent) {
            manager = new FixtureManager(parent);
            return manager;
        });
        worker.initialize();
        manager->responses.insert(pairUrl.toString(QUrl::FullyEncoded),
                                  FixtureResponse{404, QByteArrayLiteral("not found"), {}, 0});
        manager->responses.insert(redirectPairUrl.toString(QUrl::FullyEncoded),
                                  FixtureResponse{302, {}, ratesUrl, 0});
        manager->responses.insert(ratesUrl.toString(QUrl::FullyEncoded),
                                  FixtureResponse{404, QByteArrayLiteral("not found"), {}, 0});
        QSignalSpy finished(&worker, &providers::NetworkWorker::requestFinished);
        application::OperationCoordinator operations;

        auto pair = operations.begin(QStringLiteral("missing Frankfurter pair"));
        worker.get(pair, {providers::ProviderId::Frankfurter, pairUrl, false, 0, true});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        QCOMPARE(errorAt(finished, 0).value<domain::AppError>().category(), domain::ErrorCategory::None);
        QVERIFY(finished.at(0).at(1).toByteArray().isEmpty());

        auto allRates = operations.begin(QStringLiteral("Frankfurter rates HTTP failure"));
        worker.get(allRates, {providers::ProviderId::Frankfurter, ratesUrl, false, 0, false});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 1000);
        QCOMPARE(errorAt(finished, 1).value<domain::AppError>().category(), domain::ErrorCategory::Http);

        auto invalidOptIn = operations.begin(QStringLiteral("invalid missing-rate route"));
        worker.get(invalidOptIn, {providers::ProviderId::Frankfurter, malformedPair, false, 0, true});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 1000);
        QCOMPARE(errorAt(finished, 2).value<domain::AppError>().category(), domain::ErrorCategory::Validation);
        QCOMPARE(manager->requestCounts.value(malformedPair.toString(QUrl::FullyEncoded)), 0);

        auto redirectedPair = operations.begin(QStringLiteral("missing-rate redirect scope"));
        worker.get(redirectedPair, {providers::ProviderId::Frankfurter, redirectPairUrl, false, 0, true});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 4, 1000);
        QCOMPARE(errorAt(finished, 3).value<domain::AppError>().category(), domain::ErrorCategory::Provider);
        QCOMPARE(manager->requestCounts.value(ratesUrl.toString(QUrl::FullyEncoded)), 1);
        worker.shutdown();
    }

    void duplicateRequestsShareOneFlightAndCancellationIsPerSubscriber()
    {
        auto *manager = static_cast<FixtureManager *>(nullptr);
        const QUrl url(QStringLiteral("https://www.youtube.com/feeds/videos.xml?slow=1"));
        const QByteArray body = fixture(QStringLiteral("youtube-feed-empty.xml"));
        providers::NetworkWorker worker(nullptr, [&manager](QObject *parent) {
            manager = new FixtureManager(parent);
            return manager;
        });
        worker.initialize();
        manager->responses.insert(url.toString(QUrl::FullyEncoded), FixtureResponse{200, body, {}, 30});
        QSignalSpy finished(&worker, &providers::NetworkWorker::requestFinished);
        application::OperationCoordinator operations;
        const auto first = operations.begin(QStringLiteral("subscriber one"));
        const auto second = operations.begin(QStringLiteral("subscriber two"));
        worker.get(first, {providers::ProviderId::YouTubeFeed, url, false, 0});
        worker.get(second, {providers::ProviderId::YouTubeFeed, url, false, 0});
        operations.cancel(first.id);
        worker.cancel(first.id.value);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 1000);
        QCOMPARE(manager->requestCounts.value(url.toString(QUrl::FullyEncoded)), 1);
        domain::ErrorCategory firstCategory = domain::ErrorCategory::None;
        domain::ErrorCategory secondCategory = domain::ErrorCategory::None;
        for (const auto &row : finished) {
            const quint64 id = row.at(0).toULongLong();
            const auto category = row.at(2).value<domain::AppError>().category();
            if (id == first.id.value) firstCategory = category;
            if (id == second.id.value) secondCategory = category;
        }
        QCOMPARE(firstCategory, domain::ErrorCategory::Cancelled);
        QCOMPARE(secondCategory, domain::ErrorCategory::None);
        worker.shutdown();
    }

    void openMeteoCityRequestsKeepOfflineAndCancellationResultsIndependent()
    {
        auto *manager = static_cast<FixtureManager *>(nullptr);
        const QUrl successfulCity(QStringLiteral(
            "https://api.open-meteo.com/v1/forecast?latitude=35.68&longitude=139.65"));
        const QUrl offlineCity(QStringLiteral(
            "https://api.open-meteo.com/v1/forecast?latitude=41.01&longitude=28.98"));
        const QUrl cancelledCity(QStringLiteral(
            "https://api.open-meteo.com/v1/forecast?latitude=-33.87&longitude=151.21"));
        providers::NetworkWorker worker(nullptr, [&manager](QObject *parent) {
            manager = new FixtureManager(parent);
            return manager;
        });
        worker.initialize();
        manager->responses.insert(successfulCity.toString(QUrl::FullyEncoded),
            FixtureResponse{200, fixture(QStringLiteral("weather-forecast-success.json")), {}, 0});
        manager->responses.insert(offlineCity.toString(QUrl::FullyEncoded),
            FixtureResponse{200, {}, {}, 0, QNetworkReply::HostNotFoundError});
        manager->responses.insert(cancelledCity.toString(QUrl::FullyEncoded),
            FixtureResponse{200, fixture(QStringLiteral("weather-forecast-success.json")), {}, 100});

        QSignalSpy finished(&worker, &providers::NetworkWorker::requestFinished);
        application::OperationCoordinator operations;
        const auto success = operations.begin(QStringLiteral("Open-Meteo city one"));
        const auto offline = operations.begin(QStringLiteral("Open-Meteo city two"));
        const auto cancelled = operations.begin(QStringLiteral("Open-Meteo city three"));
        worker.get(success, {providers::ProviderId::OpenMeteoForecast, successfulCity, false, 0});
        worker.get(offline, {providers::ProviderId::OpenMeteoForecast, offlineCity, false, 0});
        worker.get(cancelled, {providers::ProviderId::OpenMeteoForecast, cancelledCity, false, 0});
        operations.cancel(cancelled.id);
        worker.cancel(cancelled.id.value);

        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 1000);
        QHash<quint64, domain::ErrorCategory> categories;
        QHash<quint64, QByteArray> bodies;
        for (const auto &row : finished) {
            const quint64 id = row.at(0).toULongLong();
            categories.insert(id, row.at(2).value<domain::AppError>().category());
            bodies.insert(id, row.at(1).toByteArray());
        }
        QCOMPARE(categories.value(success.id.value), domain::ErrorCategory::None);
        QVERIFY(!bodies.value(success.id.value).isEmpty());
        QCOMPARE(categories.value(offline.id.value), domain::ErrorCategory::Connection);
        QVERIFY(bodies.value(offline.id.value).isEmpty());
        QCOMPARE(categories.value(cancelled.id.value), domain::ErrorCategory::Cancelled);
        QCOMPARE(manager->requestCounts.value(successfulCity.toString(QUrl::FullyEncoded)), 1);
        QCOMPARE(manager->requestCounts.value(offlineCity.toString(QUrl::FullyEncoded)), 1);
        QCOMPARE(manager->requestCounts.value(cancelledCity.toString(QUrl::FullyEncoded)), 1);
        worker.shutdown();
    }

    void networkConcurrencyNeverExceedsFourRequests()
    {
        auto *manager = static_cast<FixtureManager *>(nullptr);
        const QByteArray body = fixture(QStringLiteral("youtube-feed-empty.xml"));
        providers::NetworkWorker worker(nullptr, [&manager](QObject *parent) {
            manager = new FixtureManager(parent);
            return manager;
        });
        worker.initialize();
        QSignalSpy finished(&worker, &providers::NetworkWorker::requestFinished);
        application::OperationCoordinator operations;
        for (int index = 0; index < 6; ++index) {
            const QUrl url(QStringLiteral("https://www.youtube.com/feeds/videos.xml?batch=%1").arg(index));
            manager->responses.insert(url.toString(QUrl::FullyEncoded),
                                      FixtureResponse{200, body, {}, 30});
            const auto operation = operations.begin(QStringLiteral("bounded request"));
            worker.get(operation, {providers::ProviderId::YouTubeFeed, url, false, 0});
        }
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 6, 1000);
        QCOMPARE(manager->maximumActiveRequests, 4);
        QCOMPARE(manager->activeRequests, 0);
        worker.shutdown();
    }

    void cancellingFinalSubscriberAbortsItsFlight()
    {
        auto *manager = static_cast<FixtureManager *>(nullptr);
        const QUrl url(QStringLiteral("https://www.youtube.com/feeds/videos.xml?cancel=last"));
        providers::NetworkWorker worker(nullptr, [&manager](QObject *parent) {
            manager = new FixtureManager(parent);
            return manager;
        });
        worker.initialize();
        manager->responses.insert(url.toString(QUrl::FullyEncoded),
                                 FixtureResponse{200, fixture(QStringLiteral("youtube-feed-empty.xml")), {}, 100});
        QSignalSpy finished(&worker, &providers::NetworkWorker::requestFinished);
        application::OperationCoordinator operations;
        const auto operation = operations.begin(QStringLiteral("last subscriber"));
        worker.get(operation, {providers::ProviderId::YouTubeFeed, url, false, 0});
        operations.cancel(operation.id);
        worker.cancel(operation.id.value);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        QCOMPARE(errorAt(finished, 0).value<domain::AppError>().category(),
                 domain::ErrorCategory::Cancelled);
        QCOMPARE(manager->activeRequests, 0);
        worker.shutdown();
    }

    void deadlineAndBodyLimitHaveStableCategories()
    {
        auto *manager = static_cast<FixtureManager *>(nullptr);
        const QUrl delayedUrl(QStringLiteral("https://www.youtube.com/feeds/videos.xml?delay=1"));
        const QUrl largeUrl(QStringLiteral("https://www.youtube.com/feeds/videos.xml?large=1"));
        providers::NetworkWorker worker(nullptr, [&manager](QObject *parent) {
            manager = new FixtureManager(parent);
            return manager;
        });
        worker.initialize();
        manager->responses.insert(delayedUrl.toString(QUrl::FullyEncoded),
                                  FixtureResponse{200, QByteArrayLiteral("late"), {}, 50});
        // The production limit is 2 MiB; the worker must reject an oversized
        // body without exposing it. This allocation is test-local and offline.
        manager->responses.insert(largeUrl.toString(QUrl::FullyEncoded),
                                  FixtureResponse{200, QByteArray(2 * 1024 * 1024 + 1, 'x'), {}, 0});
        QSignalSpy finished(&worker, &providers::NetworkWorker::requestFinished);
        application::OperationCoordinator operations;
        const auto deadline = operations.begin(QStringLiteral("short deadline"));
        worker.get(deadline, {providers::ProviderId::YouTubeFeed, delayedUrl, false, 10});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        QCOMPARE(errorAt(finished, 0).value<domain::AppError>().category(), domain::ErrorCategory::Timeout);

        const auto oversized = operations.begin(QStringLiteral("oversized body"));
        worker.get(oversized, {providers::ProviderId::YouTubeFeed, largeUrl, false, 0});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 1000);
        QCOMPARE(errorAt(finished, 1).value<domain::AppError>().category(),
                 domain::ErrorCategory::ResponseTooLarge);
        worker.shutdown();
    }
};

QTEST_GUILESS_MAIN(ProviderInfrastructureTest)
#include "provider_infrastructure_test.moc"

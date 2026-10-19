#include "currency_controller.h"

#include "../domain/weather_currency.h"
#include "../providers/feature_parsers.h"
#include "../services/currency_service.h"

#include <QThread>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace something::controllers {
namespace {

using namespace something;
constexpr int kMaximumFavoriteRateRequests = 4;

QString errorText(domain::AppError error, const QString &fallback)
{
    const QString message = error.userMessage();
    return message.isEmpty() ? fallback : message;
}

QUrl rateUrl(const QString &base, const QString &quote)
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(QStringLiteral("api.frankfurter.dev"));
    url.setPath(QStringLiteral("/v2/rate/%1/%2").arg(base, quote));
    return url;
}

QUrl ratesUrl(const QString &base)
{
    QUrl url(QStringLiteral("https://api.frankfurter.dev/v2/rates"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("base"), base);
    url.setQuery(query);
    return url;
}

QVariantMap rateMap(const domain::CurrencyRate &rate, double amount = 0.0,
                    bool conversion = false)
{
    QVariantMap values{{QStringLiteral("baseCode"), rate.baseCode},
                       {QStringLiteral("quoteCode"), rate.quoteCode},
                       {QStringLiteral("found"), rate.found},
                       {QStringLiteral("date"), rate.date},
                       {QStringLiteral("identity"), rate.baseCode == rate.quoteCode}};
    if (rate.found) {
        values.insert(QStringLiteral("rate"), rate.rate);
        if (conversion) {
            values.insert(QStringLiteral("amount"), amount);
            values.insert(QStringLiteral("converted"), amount * rate.rate);
        }
    }
    return values;
}

QVariantList rateRows(const domain::CurrencyRates &rates)
{
    QVariantList rows;
    rows.reserve(rates.rows.size());
    for (const domain::CurrencyRate &rate : rates.rows) {
        rows.push_back(QVariantMap{{QStringLiteral("code"), rate.quoteCode},
                                   {QStringLiteral("rate"), rate.rate},
                                   {QStringLiteral("date"), rate.date}});
    }
    return rows;
}

QString favoriteFreshnessKey(const QString &base, const QString &quote)
{
    return QStringLiteral("currency-favorite-rate:%1:%2").arg(base, quote);
}

} // namespace

CurrencyController::CurrencyController(application::ApplicationServiceGraph *graph,
                                       QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_allRates(this)
    , m_favorites(this)
{
    Q_ASSERT(m_graph);
    Q_ASSERT(QThread::currentThread() == thread());
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &CurrencyController::onStorageFinished);
    connect(m_graph, &application::ApplicationServiceGraph::httpsFinished,
            this, &CurrencyController::onHttpsFinished);
    connect(m_graph, &application::ApplicationServiceGraph::expensiveValueJobFinished,
            this, &CurrencyController::onValueJobFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, [this](application::ApplicationLifecycle::State state) { onLifecycleState(state); });
    onLifecycleState(m_graph->lifecycle().state());
}

void CurrencyController::onLifecycleState(application::ApplicationLifecycle::State state)
{
    if (state == application::ApplicationLifecycle::State::Running) {
        if (!m_ready) {
            m_ready = true;
            loadFavoritesSnapshot(true);
        }
    } else if (state == application::ApplicationLifecycle::State::ShuttingDown
               || state == application::ApplicationLifecycle::State::Failed
               || state == application::ApplicationLifecycle::State::Stopped) {
        m_ready = false;
        const auto ids = m_pending.keys();
        for (quint64 id : ids) cancelPending(id);
        m_waitingFavoriteRates.clear();
        m_favoriteRateOperations.clear();
        m_activeFavoriteRateRequests = 0;
        m_singleLoading = m_allLoading = m_conversionLoading = false;
        m_favoritesLoading = m_favoritesBusy = false;
        emit singleChanged();
        emit allChanged();
        emit conversionChanged();
        emit favoritesChanged();
    }
}

quint64 CurrencyController::submitStorage(Kind kind, storage::StorageCommand command,
                                          QString freshnessKey, QString base, QString quote)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("currency storage operation"),
                                                    std::move(freshnessKey));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    Pending pending;
    pending.kind = kind;
    pending.base = std::move(base);
    pending.quote = std::move(quote);
    m_pending.insert(id, pending);
    if (!m_graph->submitStorage(operation, std::move(command))) {
        m_pending.remove(id);
        return 0;
    }
    return id;
}

quint64 CurrencyController::submitNetwork(Kind kind, providers::ProviderRequest request,
                                          QString freshnessKey, const Pending &pending)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("currency provider request"),
                                                    std::move(freshnessKey));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    m_pending.insert(id, pending);
    if (!m_graph->requestHttps(operation, std::move(request))) {
        m_pending.remove(id);
        return 0;
    }
    switch (kind) {
    case Kind::SingleRequest: m_singleOperation = id; break;
    case Kind::AllRequest: m_allOperation = id; break;
    case Kind::ConversionRequest: m_conversionOperation = id; break;
    case Kind::FavoriteRequest: m_favoriteRateOperations.insert(id); break;
    default: break;
    }
    return id;
}

quint64 CurrencyController::submitParse(Kind kind, QByteArray body, QString freshnessKey,
                                        const Pending &pending)
{
    const auto operation = m_graph->beginOperation(QStringLiteral("parse currency provider response"),
                                                    std::move(freshnessKey));
    if (!operation.isValid()) return 0;
    const quint64 id = operation.id.value;
    Pending parsePending = pending;
    parsePending.kind = kind;
    m_pending.insert(id, parsePending);
    const bool submitted = m_graph->submitExpensiveValue(operation,
        [kind, body = std::move(body), base = pending.base, quote = pending.quote,
         amount = pending.amount]
        (const application::CancellationToken &cancellation) {
            services::BoundedWorkerPool::ValueResult result;
            if (cancellation.isCancellationRequested()) {
                result.error = domain::AppError::cancelled();
                return result;
            }
            if (kind == Kind::AllParse) {
                const auto parsed = providers::parseCurrencyRates(body, base);
                if (!parsed.succeeded()) {
                    result.error = parsed.error;
                    return result;
                }
                result.values.insert(QStringLiteral("baseCode"), parsed.value.baseCode);
                result.values.insert(QStringLiteral("date"), parsed.value.date);
                result.values.insert(QStringLiteral("rows"), rateRows(parsed.value));
                return result;
            }
            const auto parsed = providers::parseCurrencyRate(body, base, quote);
            if (!parsed.succeeded()) {
                result.error = parsed.error;
                return result;
            }
            result.values = rateMap(parsed.value, amount,
                                    kind == Kind::ConversionParse);
            return result;
        });
    if (!submitted) {
        m_pending.remove(id);
        return 0;
    }
    switch (kind) {
    case Kind::SingleParse: m_singleOperation = id; break;
    case Kind::AllParse: m_allOperation = id; break;
    case Kind::ConversionParse: m_conversionOperation = id; break;
    case Kind::FavoriteParse: m_favoriteRateOperations.insert(id); break;
    default: break;
    }
    return id;
}

void CurrencyController::cancelPending(quint64 id)
{
    if (!id) return;
    m_pending.remove(id);
    m_favoriteRateOperations.remove(id);
    m_graph->cancelOperation({id});
}

void CurrencyController::cancelKind(Kind first, Kind second)
{
    const auto ids = m_pending.keys();
    for (quint64 id : ids) {
        const Kind kind = m_pending.value(id).kind;
        if (kind == first || kind == second) cancelPending(id);
    }
}

void CurrencyController::lookupSingle(QString baseCode, QString quoteCode)
{
    const QString base = domain::normalizeCurrencyCode(std::move(baseCode));
    const QString quote = domain::normalizeCurrencyCode(std::move(quoteCode));
    cancelKind(Kind::SingleRequest, Kind::SingleParse);
    const quint64 generation = ++m_singleGeneration;
    m_singleOperation = 0;
    m_singleResult.clear();
    m_singleError.clear();
    if (base.isEmpty() || quote.isEmpty()) {
        m_singleError = QStringLiteral("Enter both currency codes using three ASCII letters.");
        m_singleLoading = false;
        emit singleChanged();
        return;
    }
    if (base == quote) {
        m_singleResult = rateMap(domain::CurrencyRate{base, quote, {}, 1.0, true});
        m_singleLoading = false;
        emit singleChanged();
        return;
    }
    m_singleLoading = true;
    emit singleChanged();
    Pending pending{Kind::SingleRequest, base, quote, 0.0, generation};
    providers::ProviderRequest request{providers::ProviderId::Frankfurter,
                                       rateUrl(base, quote), false, 0, true};
    if (!submitNetwork(Kind::SingleRequest, std::move(request),
                       QStringLiteral("currency-single"), pending))
        setSingleFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable),
                         QStringLiteral("The rate could not be loaded."));
}

void CurrencyController::loadAllRates(QString baseCode)
{
    const QString base = domain::normalizeCurrencyCode(std::move(baseCode));
    cancelKind(Kind::AllRequest, Kind::AllParse);
    const quint64 generation = ++m_allGeneration;
    m_allOperation = 0;
    m_allBase.clear();
    m_allDate.clear();
    m_allRates.clear();
    m_allRates.setFilter({});
    m_allError.clear();
    if (base.isEmpty()) {
        m_allError = QStringLiteral("Enter a base currency code using three ASCII letters.");
        m_allLoading = false;
        emit allChanged();
        return;
    }
    m_allBase = base;
    m_allLoading = true;
    emit allChanged();
    Pending pending{Kind::AllRequest, base, {}, 0.0, generation};
    providers::ProviderRequest request{providers::ProviderId::Frankfurter,
                                       ratesUrl(base), false, 0, false};
    if (!submitNetwork(Kind::AllRequest, std::move(request),
                       QStringLiteral("currency-all-rates"), pending))
        setAllFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable),
                      QStringLiteral("Rates could not be loaded."));
}

void CurrencyController::convert(double amount, QString baseCode, QString quoteCode)
{
    const QString base = domain::normalizeCurrencyCode(std::move(baseCode));
    const QString quote = domain::normalizeCurrencyCode(std::move(quoteCode));
    cancelKind(Kind::ConversionRequest, Kind::ConversionParse);
    const quint64 generation = ++m_conversionGeneration;
    m_conversionOperation = 0;
    m_conversionResult.clear();
    m_conversionError.clear();
    if (!domain::isValidCurrencyAmount(amount)) {
        m_conversionError = QStringLiteral("Enter an amount greater than zero.");
        m_conversionLoading = false;
        emit conversionChanged();
        return;
    }
    if (base.isEmpty() || quote.isEmpty()) {
        m_conversionError = QStringLiteral("Enter both currency codes using three ASCII letters.");
        m_conversionLoading = false;
        emit conversionChanged();
        return;
    }
    if (base == quote) {
        m_conversionResult = rateMap(domain::CurrencyRate{base, quote, {}, 1.0, true}, amount, true);
        m_conversionLoading = false;
        emit conversionChanged();
        return;
    }
    m_conversionLoading = true;
    emit conversionChanged();
    Pending pending{Kind::ConversionRequest, base, quote, amount, generation};
    providers::ProviderRequest request{providers::ProviderId::Frankfurter,
                                       rateUrl(base, quote), false, 0, true};
    if (!submitNetwork(Kind::ConversionRequest, std::move(request),
                       QStringLiteral("currency-conversion"), pending))
        setConversionFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable),
                             QStringLiteral("Conversion could not be completed."));
}

void CurrencyController::addFavorite(QString baseCode, QString quoteCode)
{
    const QString base = domain::normalizeCurrencyCode(std::move(baseCode));
    const QString quote = domain::normalizeCurrencyCode(std::move(quoteCode));
    if (!m_ready) return;
    if (base.isEmpty() || quote.isEmpty() || base == quote) {
        m_favoritesError = base == quote && !base.isEmpty()
            ? QStringLiteral("Cannot save a favorite for the same currency.")
            : QStringLiteral("Enter two different three-letter ASCII currency codes.");
        emit favoritesChanged();
        return;
    }
    if (m_mutationOperation) cancelPending(m_mutationOperation);
    m_favoritesError.clear();
    m_favoritesBusy = true;
    emit favoritesChanged();
    const quint64 id = submitStorage(Kind::AddFavorite,
        services::CurrencyService::addFavorite(base, quote),
        QStringLiteral("currency-favorite-mutation"), base, quote);
    m_mutationOperation = id;
    if (!id) {
        m_favoritesBusy = false;
        m_favoritesError = QStringLiteral("The favorite could not be saved.");
        emit favoritesChanged();
    }
}

void CurrencyController::removeFavorite(QString baseCode, QString quoteCode)
{
    const QString base = domain::normalizeCurrencyCode(std::move(baseCode));
    const QString quote = domain::normalizeCurrencyCode(std::move(quoteCode));
    if (!m_ready || base.isEmpty() || quote.isEmpty()) return;
    if (m_mutationOperation) cancelPending(m_mutationOperation);
    m_favoritesError.clear();
    m_favoritesBusy = true;
    emit favoritesChanged();
    const quint64 id = submitStorage(Kind::RemoveFavorite,
        services::CurrencyService::removeFavorite(base, quote),
        QStringLiteral("currency-favorite-mutation"), base, quote);
    m_mutationOperation = id;
    if (!id) {
        m_favoritesBusy = false;
        m_favoritesError = QStringLiteral("The favorite could not be removed.");
        emit favoritesChanged();
    }
}

void CurrencyController::selectFavorite(QString baseCode, QString quoteCode)
{
    const QString base = domain::normalizeCurrencyCode(std::move(baseCode));
    const QString quote = domain::normalizeCurrencyCode(std::move(quoteCode));
    if (base.isEmpty() || quote.isEmpty()) return;
    cancelKind(Kind::SingleRequest, Kind::SingleParse);
    ++m_singleGeneration;
    m_singleOperation = 0;
    m_singleResult.clear();
    m_singleError.clear();
    m_singleLoading = false;
    emit singleChanged();

    cancelKind(Kind::AllRequest, Kind::AllParse);
    ++m_allGeneration;
    m_allOperation = 0;
    m_allBase.clear();
    m_allDate.clear();
    m_allRates.clear();
    m_allRates.setFilter({});
    m_allError.clear();
    m_allLoading = false;
    emit allChanged();
    emit favoriteSelected(base, quote);
}

void CurrencyController::refreshFavorites()
{
    if (!m_ready) return;
    loadFavoritesSnapshot(true);
}

void CurrencyController::setAllRatesFilter(QString filter)
{
    m_allRates.setFilter(std::move(filter));
}

void CurrencyController::clearNotification()
{
    if (m_notification.isEmpty()) return;
    m_notification.clear();
    emit favoritesChanged();
}

void CurrencyController::loadFavoritesSnapshot(bool refreshAfterLoad)
{
    if (!m_ready) return;
    if (m_favoritesLoadOperation) cancelPending(m_favoritesLoadOperation);
    m_refreshAfterFavoriteLoad = refreshAfterLoad;
    if (refreshAfterLoad) {
        ++m_favoritesGeneration;
        const auto oldIds = m_favoriteRateOperations.values();
        for (quint64 oldId : oldIds) cancelPending(oldId);
        m_favoriteRateOperations.clear();
        m_activeFavoriteRateRequests = 0;
        m_waitingFavoriteRates.clear();
        m_favorites.markAll(QStringLiteral("loading"));
    }
    m_favoritesLoading = true;
    m_favoritesBusy = true;
    m_favoritesError.clear();
    emit favoritesChanged();
    const quint64 id = submitStorage(Kind::LoadFavorites,
        services::CurrencyService::listFavorites(),
        QStringLiteral("currency-favorites-load"));
    m_favoritesLoadOperation = id;
    if (!id) {
        m_favoritesLoading = false;
        m_favoritesError = QStringLiteral("Saved currency pairs could not be loaded.");
        if (refreshAfterLoad)
            m_favorites.markAll(QStringLiteral("error"), m_favoritesError);
        m_favoritesBusy = false;
        emit favoritesChanged();
    }
}

void CurrencyController::dispatchFavoriteRates()
{
    while (m_activeFavoriteRateRequests < kMaximumFavoriteRateRequests
           && !m_waitingFavoriteRates.isEmpty()) {
        const storage::CurrencyPair pair = m_waitingFavoriteRates.dequeue();
        Pending pending{Kind::FavoriteRequest, pair.baseCode, pair.quoteCode, 0.0,
                        m_favoritesGeneration};
        providers::ProviderRequest request{providers::ProviderId::Frankfurter,
            rateUrl(pair.baseCode, pair.quoteCode), false, 0, true};
        m_favorites.setRate(pair.baseCode, pair.quoteCode, false, 0.0, {},
                            QStringLiteral("loading"));
        const quint64 id = submitNetwork(Kind::FavoriteRequest, std::move(request),
            favoriteFreshnessKey(pair.baseCode, pair.quoteCode), pending);
        if (!id) {
            m_favorites.setRate(pair.baseCode, pair.quoteCode, false, 0.0, {},
                                QStringLiteral("error"),
                                QStringLiteral("The rate could not be loaded."));
            continue;
        }
        ++m_activeFavoriteRateRequests;
    }
    if (m_waitingFavoriteRates.isEmpty() && m_activeFavoriteRateRequests == 0) {
        m_favoritesBusy = false;
        emit favoritesChanged();
    }
}

void CurrencyController::completeFavoriteRate(const Pending &pending, bool found,
                                              double rate, QString date,
                                              QString status, QString error)
{
    m_favorites.setRate(pending.base, pending.quote, found, rate, std::move(date),
                        std::move(status), std::move(error));
    if (m_activeFavoriteRateRequests > 0) --m_activeFavoriteRateRequests;
    dispatchFavoriteRates();
}

void CurrencyController::onStorageFinished(quint64 id, const QVariantMap &values,
                                           domain::AppError error)
{
    const auto pendingIt = m_pending.find(id);
    if (pendingIt == m_pending.end()) return;
    const Pending pending = pendingIt.value();
    m_pending.erase(pendingIt);
    if (pending.kind == Kind::LoadFavorites) {
        if (id != m_favoritesLoadOperation) return;
        m_favoritesLoadOperation = 0;
        m_favoritesLoading = false;
        if (error.isError()) {
            m_favoritesError = errorText(error, QStringLiteral("Saved currency pairs could not be loaded."));
            if (m_refreshAfterFavoriteLoad)
                m_favorites.markAll(QStringLiteral("error"), m_favoritesError);
            m_favoritesBusy = false;
            emit favoritesChanged();
            return;
        }
        QVariantList rows;
        for (const QVariant &entry : values.value(QStringLiteral("favorites")).toList()) {
            QVariantMap row = entry.toMap();
            row.insert(QStringLiteral("found"), false);
            row.insert(QStringLiteral("rateStatus"), QStringLiteral("idle"));
            rows.push_back(row);
        }
        m_favorites.replace(rows);
        emit favoritesChanged();
        if (m_refreshAfterFavoriteLoad) {
            const auto oldIds = m_favoriteRateOperations.values();
            for (quint64 oldId : oldIds) cancelPending(oldId);
            m_favoriteRateOperations.clear();
            m_activeFavoriteRateRequests = 0;
            m_waitingFavoriteRates.clear();
            for (const QVariant &entry : std::as_const(rows)) {
                const QVariantMap row = entry.toMap();
                m_waitingFavoriteRates.enqueue({row.value(QStringLiteral("baseCode")).toString(),
                                                row.value(QStringLiteral("quoteCode")).toString()});
            }
            m_favoritesBusy = !m_waitingFavoriteRates.isEmpty();
            m_favorites.markAll(QStringLiteral("loading"));
            dispatchFavoriteRates();
        } else {
            m_favoritesBusy = false;
        }
        return;
    }
    if (pending.kind == Kind::AddFavorite || pending.kind == Kind::RemoveFavorite) {
        if (id != m_mutationOperation) return;
        m_mutationOperation = 0;
        m_favoritesBusy = false;
        if (error.isError()) {
            m_favoritesError = errorText(error, QStringLiteral("The saved pair could not be changed."));
            emit favoritesChanged();
            return;
        }
        if (pending.kind == Kind::AddFavorite) {
            m_notification = values.value(QStringLiteral("exists")).toBool()
                ? QStringLiteral("%1 is already in your favorites.").arg(values.value(QStringLiteral("pair")).toString())
                : QStringLiteral("%1 added to favorites.").arg(values.value(QStringLiteral("pair")).toString());
        } else {
            m_notification = QStringLiteral("%1 removed from favorites.").arg(values.value(QStringLiteral("pair")).toString());
        }
        emit favoritesChanged();
        loadFavoritesSnapshot(true);
    }
}

void CurrencyController::onHttpsFinished(quint64 id, const QByteArray &body,
                                         domain::AppError error)
{
    const auto pendingIt = m_pending.find(id);
    if (pendingIt == m_pending.end()) return;
    Pending pending = pendingIt.value();
    m_pending.erase(pendingIt);
    m_favoriteRateOperations.remove(id);
    const bool favorite = pending.kind == Kind::FavoriteRequest;
    if (pending.kind == Kind::SingleRequest && pending.generation != m_singleGeneration) return;
    if (pending.kind == Kind::AllRequest && pending.generation != m_allGeneration) return;
    if (pending.kind == Kind::ConversionRequest && pending.generation != m_conversionGeneration) return;
    if (favorite && pending.generation != m_favoritesGeneration) return;

    if (error.isError()) {
        if (favorite) {
            failFavoriteRequest(pending, error);
        } else if (pending.kind == Kind::SingleRequest) {
            setSingleFailure(error, QStringLiteral("The rate could not be loaded."));
        } else if (pending.kind == Kind::AllRequest) {
            setAllFailure(error, QStringLiteral("Rates could not be loaded."));
        } else {
            setConversionFailure(error, QStringLiteral("Conversion could not be completed."));
        }
        return;
    }

    if (favorite && body.isEmpty()) {
        completeFavoriteRate(pending, false, 0.0, {}, QStringLiteral("missing"));
        return;
    }
    if (!favorite && (pending.kind == Kind::SingleRequest
                      || pending.kind == Kind::ConversionRequest) && body.isEmpty()) {
        domain::CurrencyRate missing{pending.base, pending.quote, {}, 0.0, false};
        if (pending.kind == Kind::SingleRequest) {
            m_singleResult = rateMap(missing);
            m_singleLoading = false;
            emit singleChanged();
        } else {
            m_conversionResult = rateMap(missing, pending.amount, true);
            m_conversionLoading = false;
            emit conversionChanged();
        }
        return;
    }

    Kind parseKind = Kind::SingleParse;
    QString key = QStringLiteral("currency-single");
    if (pending.kind == Kind::AllRequest) { parseKind = Kind::AllParse; key = QStringLiteral("currency-all-rates"); }
    else if (pending.kind == Kind::ConversionRequest) { parseKind = Kind::ConversionParse; key = QStringLiteral("currency-conversion"); }
    else if (favorite) { parseKind = Kind::FavoriteParse; key = favoriteFreshnessKey(pending.base, pending.quote); }
    const quint64 parsedId = submitParse(parseKind, body, key, pending);
    if (!parsedId) {
        if (favorite) failFavoriteRequest(pending,
            domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
        else if (pending.kind == Kind::AllRequest)
            setAllFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable),
                          QStringLiteral("Rates could not be parsed."));
        else if (pending.kind == Kind::ConversionRequest)
            setConversionFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable),
                                 QStringLiteral("Conversion could not be completed."));
        else setSingleFailure(domain::AppError(domain::ErrorCategory::CapabilityUnavailable),
                              QStringLiteral("The rate could not be parsed."));
    }
}

void CurrencyController::onValueJobFinished(quint64 id, const QVariantMap &values,
                                            domain::AppError error)
{
    const auto pendingIt = m_pending.find(id);
    if (pendingIt == m_pending.end()) return;
    const Pending pending = pendingIt.value();
    m_pending.erase(pendingIt);
    m_favoriteRateOperations.remove(id);
    if (pending.kind == Kind::SingleParse) {
        if (pending.generation != m_singleGeneration) return;
        m_singleLoading = false;
        if (error.isError()) {
            setSingleFailure(error, QStringLiteral("The rate could not be read."));
            return;
        }
        m_singleResult = values;
        m_singleError.clear();
        emit singleChanged();
    } else if (pending.kind == Kind::AllParse) {
        if (pending.generation != m_allGeneration) return;
        m_allLoading = false;
        if (error.isError()) {
            setAllFailure(error, QStringLiteral("Rates could not be read."));
            return;
        }
        m_allBase = values.value(QStringLiteral("baseCode")).toString();
        m_allDate = values.value(QStringLiteral("date")).toString();
        m_allRates.replace(values.value(QStringLiteral("rows")).toList());
        m_allError.clear();
        emit allChanged();
    } else if (pending.kind == Kind::ConversionParse) {
        if (pending.generation != m_conversionGeneration) return;
        m_conversionLoading = false;
        if (error.isError()) {
            setConversionFailure(error, QStringLiteral("The conversion could not be read."));
            return;
        }
        m_conversionResult = values;
        m_conversionError.clear();
        emit conversionChanged();
    } else if (pending.kind == Kind::FavoriteParse) {
        if (pending.generation != m_favoritesGeneration) return;
        if (error.isError()) {
            failFavoriteRequest(pending, error);
            return;
        }
        completeFavoriteRate(pending, values.value(QStringLiteral("found")).toBool(),
                             values.value(QStringLiteral("rate")).toDouble(),
                             values.value(QStringLiteral("date")).toString(),
                             values.value(QStringLiteral("found")).toBool()
                                 ? QStringLiteral("available") : QStringLiteral("missing"));
    }
}

void CurrencyController::failFavoriteRequest(const Pending &pending, domain::AppError error)
{
    if (pending.generation != m_favoritesGeneration) return;
    completeFavoriteRate(pending, false, 0.0, {}, QStringLiteral("error"),
                         errorText(error, QStringLiteral("The rate could not be loaded.")));
}

void CurrencyController::setSingleFailure(domain::AppError error, const QString &fallback)
{
    m_singleLoading = false;
    m_singleError = errorText(error, fallback);
    emit singleChanged();
}
void CurrencyController::setAllFailure(domain::AppError error, const QString &fallback)
{
    m_allLoading = false;
    m_allError = errorText(error, fallback);
    emit allChanged();
}
void CurrencyController::setConversionFailure(domain::AppError error, const QString &fallback)
{
    m_conversionLoading = false;
    m_conversionError = errorText(error, fallback);
    emit conversionChanged();
}

} // namespace something::controllers

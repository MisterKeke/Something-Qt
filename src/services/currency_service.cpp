#include "currency_service.h"

#include "../domain/weather_currency.h"

namespace something::services {
namespace {

storage::StorageReply failed(domain::AppError error)
{
    storage::StorageReply reply;
    reply.error = error;
    return reply;
}

QVariantMap pairMap(const storage::CurrencyPair &pair)
{
    return {{QStringLiteral("baseCode"), pair.baseCode},
            {QStringLiteral("quoteCode"), pair.quoteCode},
            {QStringLiteral("code"), pair.baseCode + QLatin1Char(':') + pair.quoteCode}};
}

} // namespace

storage::StorageCommand CurrencyService::listFavorites()
{
    return [](storage::StorageRepositories &repositories,
              const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        const auto loaded = repositories.currencies.list();
        if (!loaded.succeeded()) return failed(loaded.error);
        QVariantList rows;
        rows.reserve(loaded.value.size());
        for (const storage::CurrencyPair &pair : loaded.value) rows.push_back(pairMap(pair));
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("favorites"), rows);
        return reply;
    };
}

storage::StorageCommand CurrencyService::addFavorite(QString baseCode, QString quoteCode)
{
    baseCode = domain::normalizeCurrencyCode(std::move(baseCode));
    quoteCode = domain::normalizeCurrencyCode(std::move(quoteCode));
    return [baseCode = std::move(baseCode), quoteCode = std::move(quoteCode)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (baseCode.isEmpty() || quoteCode.isEmpty() || baseCode == quoteCode)
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        const auto existing = repositories.currencies.list();
        if (!existing.succeeded()) return failed(existing.error);
        bool found = false;
        for (const storage::CurrencyPair &pair : existing.value)
            found = found || (pair.baseCode == baseCode && pair.quoteCode == quoteCode);
        if (!found) {
            const auto added = repositories.currencies.add(baseCode, quoteCode);
            if (!added.succeeded()) return failed(added.error);
        }
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("added"), !found);
        reply.values.insert(QStringLiteral("exists"), found);
        reply.values.insert(QStringLiteral("pair"), baseCode + QLatin1Char(':') + quoteCode);
        return reply;
    };
}

storage::StorageCommand CurrencyService::removeFavorite(QString baseCode, QString quoteCode)
{
    baseCode = domain::normalizeCurrencyCode(std::move(baseCode));
    quoteCode = domain::normalizeCurrencyCode(std::move(quoteCode));
    return [baseCode = std::move(baseCode), quoteCode = std::move(quoteCode)](
               storage::StorageRepositories &repositories,
               const std::function<bool()> &cancelled) {
        if (cancelled()) return failed(domain::AppError::cancelled());
        if (baseCode.isEmpty() || quoteCode.isEmpty())
            return failed(domain::AppError(domain::ErrorCategory::Validation));
        const auto removed = repositories.currencies.remove(baseCode, quoteCode);
        if (!removed.succeeded()) return failed(removed.error);
        storage::StorageReply reply;
        reply.values.insert(QStringLiteral("pair"), baseCode + QLatin1Char(':') + quoteCode);
        return reply;
    };
}

} // namespace something::services

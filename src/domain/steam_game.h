#pragma once

#include "app_error.h"

#include <QLocale>
#include <QString>
#include <QVector>

#include <optional>

namespace something::domain {

enum class SteamPriceState { Priced, Free, Unavailable };

struct SteamGameDetails final {
    quint32 appId{0};
    QString name;
    QString artworkUrl;
    SteamPriceState priceState{SteamPriceState::Unavailable};
    QString currency;
    std::optional<qint64> regularPriceMinor;
    std::optional<qint64> currentPriceMinor;
    int discountPercent{0};
};

struct SteamStoreUrl final {
    quint32 appId{0};
    QString canonicalUrl;
    AppError error;
    bool isValid() const noexcept { return appId != 0 && !error.isError(); }
};

struct SteamCountry final {
    QString code;
    QString name;
};

SteamStoreUrl normalizeSteamStoreUrl(QString value);
QString normalizeSteamArtworkUrl(QString value);
QString steamPriceStateName(SteamPriceState state);
QVector<SteamCountry> supportedSteamCountries();
bool isSupportedSteamCountry(QString code);
QString formatSteamPrice(qint64 amountMinor, QString currencyCode,
                         const QLocale &locale = QLocale());
int effectiveSteamDiscount(qint64 regularMinor, qint64 currentMinor, int providerDiscount);

} // namespace something::domain

#include "steam_game.h"

#include <QLocale>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <limits>

namespace something::domain {
namespace {

constexpr quint64 maximumSteamAppId = std::numeric_limits<quint32>::max();

int currencyDigits(const QString &code)
{
    static const QStringList zeroDigits{
        QStringLiteral("BIF"), QStringLiteral("CLP"), QStringLiteral("DJF"),
        QStringLiteral("GNF"), QStringLiteral("ISK"), QStringLiteral("JPY"),
        QStringLiteral("KMF"), QStringLiteral("KRW"), QStringLiteral("PYG"),
        QStringLiteral("RWF"), QStringLiteral("UGX"), QStringLiteral("UYI"),
        QStringLiteral("VND"), QStringLiteral("VUV"), QStringLiteral("XAF"),
        QStringLiteral("XOF"), QStringLiteral("XPF")};
    static const QStringList threeDigits{
        QStringLiteral("BHD"), QStringLiteral("IQD"), QStringLiteral("JOD"),
        QStringLiteral("KWD"), QStringLiteral("LYD"), QStringLiteral("OMR"),
        QStringLiteral("TND")};
    if (zeroDigits.contains(code)) return 0;
    if (threeDigits.contains(code)) return 3;
    if (code == QStringLiteral("CLF")) return 4;
    return 2;
}

} // namespace

SteamStoreUrl normalizeSteamStoreUrl(QString value)
{
    value = value.trimmed();
    if (value.isEmpty() || value.toUtf8().size() > 4096)
        return {0, {}, AppError(ErrorCategory::Validation)};

    const QUrl url(value, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    const int port = url.port(-1);
    const int standardPort = scheme == QStringLiteral("https") ? 443 : 80;
    if (!url.isValid() || (scheme != QStringLiteral("http")
                           && scheme != QStringLiteral("https"))
        || !url.userInfo().isEmpty()
        || (port != -1 && port != standardPort)
        || url.host().compare(QStringLiteral("store.steampowered.com"),
                              Qt::CaseInsensitive) != 0) {
        return {0, {}, AppError(ErrorCategory::Validation)};
    }

    static const QRegularExpression appPath(
        QStringLiteral("(?i)^/app/([0-9]+)(?:/.*)?$"));
    const auto match = appPath.match(url.path());
    if (!match.hasMatch()) return {0, {}, AppError(ErrorCategory::Validation)};
    bool idOk = false;
    const quint64 parsed = match.captured(1).toULongLong(&idOk, 10);
    if (!idOk || parsed == 0 || parsed > maximumSteamAppId)
        return {0, {}, AppError(ErrorCategory::Validation)};

    const quint32 appId = static_cast<quint32>(parsed);
    return {appId,
            QStringLiteral("https://store.steampowered.com/app/%1/").arg(appId),
            AppError::none()};
}

QString normalizeSteamArtworkUrl(QString value)
{
    value = value.trimmed();
    if (value.isEmpty()) return {};
    const QUrl url(value, QUrl::StrictMode);
    if (!url.isValid() || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
        || !url.userInfo().isEmpty() || (url.port(-1) != -1 && url.port(-1) != 443)
        || url.hasFragment()) {
        return {};
    }
    const QString host = url.host().toLower();
    const bool steamStatic = host == QStringLiteral("steamstatic.com")
        || host.endsWith(QStringLiteral(".steamstatic.com"));
    if (!steamStatic && host != QStringLiteral("steamcdn-a.akamaihd.net")) return {};
    return url.toString(QUrl::FullyEncoded);
}

QString steamPriceStateName(SteamPriceState state)
{
    switch (state) {
    case SteamPriceState::Priced: return QStringLiteral("priced");
    case SteamPriceState::Free: return QStringLiteral("free");
    case SteamPriceState::Unavailable: return QStringLiteral("unavailable");
    case SteamPriceState::Unsupported: return QStringLiteral("unsupported");
    }
    return QStringLiteral("unavailable");
}

QVector<SteamCountry> supportedSteamCountries()
{
    return {
        {QStringLiteral("AU"), QStringLiteral("Australia")},
        {QStringLiteral("BR"), QStringLiteral("Brazil")},
        {QStringLiteral("CA"), QStringLiteral("Canada")},
        {QStringLiteral("CN"), QStringLiteral("China")},
        {QStringLiteral("FR"), QStringLiteral("France")},
        {QStringLiteral("DE"), QStringLiteral("Germany")},
        {QStringLiteral("IN"), QStringLiteral("India")},
        {QStringLiteral("JP"), QStringLiteral("Japan")},
        {QStringLiteral("MX"), QStringLiteral("Mexico")},
        {QStringLiteral("NL"), QStringLiteral("Netherlands")},
        {QStringLiteral("PL"), QStringLiteral("Poland")},
        {QStringLiteral("KR"), QStringLiteral("South Korea")},
        {QStringLiteral("SE"), QStringLiteral("Sweden")},
        {QStringLiteral("TR"), QStringLiteral("Türkiye")},
        {QStringLiteral("UA"), QStringLiteral("Ukraine")},
        {QStringLiteral("GB"), QStringLiteral("United Kingdom")},
        {QStringLiteral("US"), QStringLiteral("United States")},
    };
}

bool isSupportedSteamCountry(QString code)
{
    code = code.trimmed().toUpper();
    const auto countries = supportedSteamCountries();
    return std::any_of(countries.cbegin(), countries.cend(), [&code](const SteamCountry &country) {
        return country.code == code;
    });
}

QString formatSteamPrice(qint64 amountMinor, QString currencyCode, const QLocale &locale)
{
    if (amountMinor < 0) return {};
    currencyCode = currencyCode.trimmed().toUpper();
    static const QRegularExpression codePattern(QStringLiteral("^[A-Z]{3}$"));
    if (!codePattern.match(currencyCode).hasMatch()) return {};

    const int digits = currencyDigits(currencyCode);
    qint64 scale = 1;
    for (int i = 0; i < digits; ++i) scale *= 10;
    const qint64 major = amountMinor / scale;
    const qint64 fractional = amountMinor % scale;
    QString result = locale.toString(major);
    if (digits > 0) {
        result += locale.decimalPoint();
        result += QStringLiteral("%1").arg(fractional, digits, 10, QLatin1Char('0'));
    }
    return result + QLatin1Char(' ') + currencyCode;
}

int effectiveSteamDiscount(qint64 regularMinor, qint64 currentMinor, int providerDiscount)
{
    if (regularMinor <= 0 || currentMinor < 0 || currentMinor >= regularMinor) return 0;
    if (providerDiscount > 0 && providerDiscount <= 100) return providerDiscount;
    return qBound(0, qRound((1.0 - static_cast<double>(currentMinor) / regularMinor) * 100.0), 100);
}

} // namespace something::domain

#include "favorite_rules.h"

#include <QRegularExpression>
#include <QUrlQuery>

namespace something::domain {
namespace {

bool asciiTelegramName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9_]{5,32}$"));
    return pattern.match(name).hasMatch();
}

bool channelId(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^UC[A-Za-z0-9_-]{22}$"));
    return pattern.match(value).hasMatch();
}

bool videoId(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
    return pattern.match(value).hasMatch();
}

bool validHandle(const QString &value)
{
    const qsizetype length = value.toUcs4().size();
    if (length < 3 || length > 30) return false;
    for (char32_t point : value.toUcs4()) {
        if (!QChar::isLetterOrNumber(point) && point != U'_' && point != U'-' && point != U'.') return false;
    }
    return true;
}

QUrl safeHttpsUrl(QString host, QString path)
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(std::move(host));
    url.setPath(std::move(path));
    return url;
}

} // namespace

QString normalizeFavoriteSource(QString source)
{
    source = source.trimmed().toLower();
    return source == QStringLiteral("telegram") || source == QStringLiteral("youtube")
        ? source : QString();
}

QString normalizeTelegramChannel(QString channel)
{
    channel = channel.trimmed();
    if (channel.startsWith(QLatin1Char('@'))) channel.remove(0, 1);
    channel = channel.toLower();
    return asciiTelegramName(channel) ? channel : QString();
}

QString normalizeYouTubeReference(QString reference, bool *isChannelId)
{
    reference = reference.trimmed();
    if (reference.startsWith(QLatin1Char('@'))) reference.remove(0, 1);
    if (channelId(reference)) {
        if (isChannelId) *isChannelId = true;
        return reference;
    }
    if (isChannelId) *isChannelId = false;
    return validHandle(reference) ? reference : QString();
}

QString normalizeFavoriteCategoryColor(QString color)
{
    color = color.trimmed().toLower();
    static const QRegularExpression six(QStringLiteral("^#[0-9a-f]{6}$"));
    static const QRegularExpression three(QStringLiteral("^#[0-9a-f]{3}$"));
    if (color.isEmpty()) return {};
    if (three.match(color).hasMatch()) {
        return QStringLiteral("#%1%1%2%2%3%3").arg(color.mid(1, 1), color.mid(2, 1), color.mid(3, 1));
    }
    return six.match(color).hasMatch() ? color : QString();
}

QUrl telegramPostsUrl(const QString &channel, quint64 before)
{
    const QString normalized = normalizeTelegramChannel(channel);
    if (normalized.isEmpty()) return {};
    QUrl url = safeHttpsUrl(QStringLiteral("t.me"), QStringLiteral("/s/") + normalized);
    if (before > 0) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("before"), QString::number(before));
        url.setQuery(query);
    }
    return url;
}

quint64 oldestTelegramPostId(const QStringList &postIds)
{
    quint64 oldest = 0;
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    for (const QString &postId : postIds) {
        const QString id = postId.mid(postId.lastIndexOf(QLatin1Char('/')) + 1);
        if (!digits.match(id).hasMatch()) continue;
        bool ok = false;
        const quint64 candidate = id.toULongLong(&ok);
        if (ok && candidate > 0 && (oldest == 0 || candidate < oldest)) oldest = candidate;
    }
    return oldest;
}

QUrl telegramPostUrl(const QString &channel, const QString &postId)
{
    const QString normalized = normalizeTelegramChannel(channel);
    QString id = postId.trimmed();
    const qsizetype slash = id.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) {
        const QString idChannel = normalizeTelegramChannel(id.left(slash));
        if (idChannel != normalized) return {};
        id = id.mid(slash + 1);
    }
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    if (normalized.isEmpty() || !digits.match(id).hasMatch()) return {};
    return safeHttpsUrl(QStringLiteral("t.me"), QStringLiteral("/") + normalized + QLatin1Char('/') + id);
}

QUrl youTubeChannelUrl(const QString &reference, bool isChannelId)
{
    const QString normalized = normalizeYouTubeReference(reference);
    if (normalized.isEmpty()) return {};
    return safeHttpsUrl(QStringLiteral("www.youtube.com"),
                        isChannelId ? QStringLiteral("/channel/") + normalized
                                    : QStringLiteral("/@") + normalized);
}

QUrl youTubeFeedUrl(const QString &channelIdValue)
{
    if (!channelId(channelIdValue)) return {};
    QUrl url = safeHttpsUrl(QStringLiteral("www.youtube.com"), QStringLiteral("/feeds/videos.xml"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("channel_id"), channelIdValue);
    url.setQuery(query);
    return url;
}

QUrl youTubeVideoUrl(const QString &videoIdValue)
{
    if (!videoId(videoIdValue)) return {};
    QUrl url = safeHttpsUrl(QStringLiteral("www.youtube.com"), QStringLiteral("/watch"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("v"), videoIdValue);
    url.setQuery(query);
    return url;
}

bool isAllowedExternalFavoriteUrl(const QString &source, const QUrl &url)
{
    const QString normalizedSource = normalizeFavoriteSource(source);
    if (!url.isValid() || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 ||
        !url.userInfo().isEmpty() || (url.port(-1) != -1 && url.port(-1) != 443) ||
        !url.fragment().isEmpty()) return false;
    const QString host = url.host().toLower();
    const QStringList segments = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (normalizedSource == QStringLiteral("telegram")) {
        static const QRegularExpression postId(QStringLiteral("^[0-9]+$"));
        return host == QStringLiteral("t.me") && segments.size() == 2 &&
            !normalizeTelegramChannel(segments.at(0)).isEmpty() && postId.match(segments.at(1)).hasMatch();
    }
    if (normalizedSource == QStringLiteral("youtube")) {
        if (host != QStringLiteral("www.youtube.com")) return false;
        if (url.path() == QStringLiteral("/watch"))
            return videoId(QUrlQuery(url).queryItemValue(QStringLiteral("v")));
        if (segments.size() == 2 && segments.first() == QStringLiteral("channel"))
            return channelId(segments.at(1));
        if (segments.size() == 1 && segments.first().startsWith(QLatin1Char('@')))
            return !normalizeYouTubeReference(segments.first()).isEmpty();
    }
    return false;
}

} // namespace something::domain

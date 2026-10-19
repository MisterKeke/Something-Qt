#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

namespace something::domain {

QString normalizeFavoriteSource(QString source);
QString normalizeTelegramChannel(QString channel);
QString normalizeYouTubeReference(QString reference, bool *isChannelId = nullptr);
QString normalizeFavoriteCategoryColor(QString color);
QUrl telegramPostsUrl(const QString &channel, quint64 before = 0);
quint64 oldestTelegramPostId(const QStringList &postIds);
QUrl telegramPostUrl(const QString &channel, const QString &postId);
QUrl youTubeChannelUrl(const QString &reference, bool isChannelId);
QUrl youTubeFeedUrl(const QString &channelId);
QUrl youTubeVideoUrl(const QString &videoId);
bool isAllowedExternalFavoriteUrl(const QString &source, const QUrl &url);

} // namespace something::domain

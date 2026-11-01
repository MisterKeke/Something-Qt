#pragma once

#include <QRegularExpression>
#include <QString>

namespace something::domain::NotificationPlaceholders {

inline bool supported(const QString &placeholder, const QString &trigger)
{
    if (placeholder == QStringLiteral("task.id")
        || placeholder == QStringLiteral("task.title")
        || placeholder == QStringLiteral("task.priority"))
        return trigger == QStringLiteral("task.completed")
            || trigger == QStringLiteral("task.overdue");
    if (placeholder == QStringLiteral("task.completedAt"))
        return trigger == QStringLiteral("task.completed");
    if (placeholder == QStringLiteral("application.id")
        || placeholder == QStringLiteral("application.name")
        || placeholder == QStringLiteral("application.sessionId"))
        return trigger == QStringLiteral("application.session_started")
            || trigger == QStringLiteral("application.foreground_entered");
    if (placeholder == QStringLiteral("steam.appId")
        || placeholder == QStringLiteral("steam.gameName"))
        return trigger.startsWith(QStringLiteral("steam."));
    if (placeholder == QStringLiteral("steam.currency")
        || placeholder == QStringLiteral("steam.country")
        || placeholder == QStringLiteral("steam.priceMinor")
        || placeholder == QStringLiteral("steam.previousPriceMinor"))
        return trigger == QStringLiteral("steam.price_target_reached");
    if (placeholder == QStringLiteral("steam.newsTitle")
        || placeholder == QStringLiteral("steam.newsId"))
        return trigger == QStringLiteral("steam.game_news_available");
    if (placeholder == QStringLiteral("steam.releaseDate")
        || placeholder == QStringLiteral("steam.datePrecision"))
        return trigger == QStringLiteral("steam.release_date_changed");
    if (placeholder == QStringLiteral("schedule.id")
        || placeholder == QStringLiteral("schedule.localTime"))
        return trigger == QStringLiteral("schedule.time_reached");
    return false;
}

inline bool templateSupported(const QString &pattern, const QString &trigger)
{
    static const QRegularExpression token(QStringLiteral("\\{([^{}]+)\\}"));
    auto matches = token.globalMatch(pattern);
    while (matches.hasNext()) {
        if (!supported(matches.next().captured(1), trigger)) return false;
    }
    return true;
}

} // namespace something::domain::NotificationPlaceholders

#include "steam_release.h"

#include "steam_game.h"

namespace something::domain {

QString steamReleaseDatePrecisionName(SteamReleaseDatePrecision precision)
{
    switch (precision) {
    case SteamReleaseDatePrecision::ExactDay: return QStringLiteral("exact_day");
    case SteamReleaseDatePrecision::Month: return QStringLiteral("month");
    case SteamReleaseDatePrecision::Year: return QStringLiteral("year");
    case SteamReleaseDatePrecision::Quarter: return QStringLiteral("quarter");
    case SteamReleaseDatePrecision::Season: return QStringLiteral("season");
    case SteamReleaseDatePrecision::Range: return QStringLiteral("range");
    case SteamReleaseDatePrecision::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QString steamReleaseStatusName(SteamReleaseStatus status)
{
    switch (status) {
    case SteamReleaseStatus::ExactDateAnnounced:
        return QStringLiteral("exact_date_announced");
    case SteamReleaseStatus::ApproximateDateAnnounced:
        return QStringLiteral("approximate_date_announced");
    case SteamReleaseStatus::ComingSoon: return QStringLiteral("coming_soon");
    case SteamReleaseStatus::Released: return QStringLiteral("released");
    case SteamReleaseStatus::InformationUnavailable:
        return QStringLiteral("information_unavailable");
    }
    return QStringLiteral("information_unavailable");
}

QString steamReleaseRefreshStateName(SteamReleaseRefreshState state)
{
    switch (state) {
    case SteamReleaseRefreshState::NeverAttempted:
        return QStringLiteral("never_attempted");
    case SteamReleaseRefreshState::Succeeded: return QStringLiteral("succeeded");
    case SteamReleaseRefreshState::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("never_attempted");
}

std::optional<SteamReleaseDatePrecision> steamReleaseDatePrecisionFromName(
    const QString &name)
{
    if (name == QStringLiteral("unknown")) return SteamReleaseDatePrecision::Unknown;
    if (name == QStringLiteral("exact_day")) return SteamReleaseDatePrecision::ExactDay;
    if (name == QStringLiteral("month")) return SteamReleaseDatePrecision::Month;
    if (name == QStringLiteral("year")) return SteamReleaseDatePrecision::Year;
    if (name == QStringLiteral("quarter")) return SteamReleaseDatePrecision::Quarter;
    if (name == QStringLiteral("season")) return SteamReleaseDatePrecision::Season;
    if (name == QStringLiteral("range")) return SteamReleaseDatePrecision::Range;
    return std::nullopt;
}

std::optional<SteamReleaseStatus> steamReleaseStatusFromName(const QString &name)
{
    if (name == QStringLiteral("exact_date_announced"))
        return SteamReleaseStatus::ExactDateAnnounced;
    if (name == QStringLiteral("approximate_date_announced"))
        return SteamReleaseStatus::ApproximateDateAnnounced;
    if (name == QStringLiteral("coming_soon")) return SteamReleaseStatus::ComingSoon;
    if (name == QStringLiteral("released")) return SteamReleaseStatus::Released;
    if (name == QStringLiteral("information_unavailable"))
        return SteamReleaseStatus::InformationUnavailable;
    return std::nullopt;
}

std::optional<SteamReleaseRefreshState> steamReleaseRefreshStateFromName(
    const QString &name)
{
    if (name == QStringLiteral("never_attempted"))
        return SteamReleaseRefreshState::NeverAttempted;
    if (name == QStringLiteral("succeeded")) return SteamReleaseRefreshState::Succeeded;
    if (name == QStringLiteral("failed")) return SteamReleaseRefreshState::Failed;
    return std::nullopt;
}

bool isValidSteamReleaseDateInfo(const SteamReleaseDateInfo &date)
{
    if (date.announcedText.size() > 200) return false;
    const bool hasWindowStart = date.windowStart.has_value();
    const bool hasWindowEnd = date.windowEnd.has_value();
    if (hasWindowStart != hasWindowEnd) return false;
    if (hasWindowStart && (!date.windowStart->isValid() || !date.windowEnd->isValid()
                           || *date.windowStart > *date.windowEnd)) {
        return false;
    }

    if (date.precision == SteamReleaseDatePrecision::ExactDay) {
        return date.hasExactDate() && !hasWindowStart;
    }
    if (date.exactDate.has_value()) return false;
    if (date.precision == SteamReleaseDatePrecision::Unknown && hasWindowStart) return false;
    if ((date.precision == SteamReleaseDatePrecision::Month
         || date.precision == SteamReleaseDatePrecision::Year
         || date.precision == SteamReleaseDatePrecision::Quarter)
        && !hasWindowStart) {
        return false;
    }
    return true;
}

bool isValidSteamReleaseDetails(const SteamReleaseDetails &details)
{
    if (details.appId == 0 || details.title.trimmed().isEmpty()
        || details.title.trimmed().toUcs4().size() > 300
        || (!details.artworkUrl.isEmpty()
            && normalizeSteamArtworkUrl(details.artworkUrl) != details.artworkUrl)
        || !isValidSteamReleaseDateInfo(details.releaseDate)) {
        return false;
    }

    switch (details.status) {
    case SteamReleaseStatus::ExactDateAnnounced:
        return details.releaseDate.hasExactDate();
    case SteamReleaseStatus::ApproximateDateAnnounced:
        return details.releaseDate.precision != SteamReleaseDatePrecision::Unknown
            && details.releaseDate.precision != SteamReleaseDatePrecision::ExactDay
            && !details.releaseDate.announcedText.trimmed().isEmpty();
    case SteamReleaseStatus::ComingSoon:
        return details.releaseDate.precision == SteamReleaseDatePrecision::Unknown
            && !details.releaseDate.exactDate.has_value();
    case SteamReleaseStatus::Released:
        return true;
    case SteamReleaseStatus::InformationUnavailable:
        return details.releaseDate.precision == SteamReleaseDatePrecision::Unknown
            && !details.releaseDate.exactDate.has_value();
    }
    return false;
}

} // namespace something::domain

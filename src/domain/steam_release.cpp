#include "steam_release.h"

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

} // namespace something::domain

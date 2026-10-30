#pragma once

#include <QDate>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace something::domain {

// Precision describes what Steam announced. An exact day is represented
// separately from approximate bounds so a year, season, or window cannot be
// mistaken for a fabricated exact date.
enum class SteamReleaseDatePrecision {
    Unknown,
    ExactDay,
    Month,
    Year,
    Quarter,
    Season,
    Range,
};

enum class SteamReleaseStatus {
    ExactDateAnnounced,
    ApproximateDateAnnounced,
    ComingSoon,
    Released,
    InformationUnavailable,
};

// This records the most recent refresh attempt. Failed refreshes can retain
// the last successful release metadata and its timestamp.
enum class SteamReleaseRefreshState {
    NeverAttempted,
    Succeeded,
    Failed,
};

struct SteamReleaseDateInfo final {
    // Preserve Steam's announcement for display, including approximate text.
    QString announcedText;
    SteamReleaseDatePrecision precision{SteamReleaseDatePrecision::Unknown};

    // Set exactDate only when Steam announced a specific calendar day.
    std::optional<QDate> exactDate;

    // Optional inclusive bounds for a known approximate period. These bounds
    // describe a window and must never be presented as an exact release date.
    std::optional<QDate> windowStart;
    std::optional<QDate> windowEnd;

    friend bool operator==(const SteamReleaseDateInfo &,
                           const SteamReleaseDateInfo &) = default;

    bool hasExactDate() const noexcept
    {
        return precision == SteamReleaseDatePrecision::ExactDay
            && exactDate.has_value() && exactDate->isValid();
    }
};

struct SteamReleaseArtworkMetadata final {
    QString sourceUrl;
    QString cachedFilename;
    QString mimeType;
    qint64 byteSize{0};
};

// Detached Steam Store fields used when adding or refreshing a release. The
// tracked record adds local membership and refresh timestamps around these.
struct SteamReleaseDetails final {
    quint32 appId{0};
    QString title;
    QString artworkUrl;
    SteamReleaseDateInfo releaseDate;
    SteamReleaseStatus status{SteamReleaseStatus::InformationUnavailable};
};

// Persistence-neutral membership and latest-known release metadata. App ID
// identifies this record independently of price-tracker membership.
struct SteamTrackedRelease final {
    quint32 appId{0};
    QString canonicalStoreUrl;
    QString title;
    SteamReleaseArtworkMetadata artwork;
    SteamReleaseDateInfo releaseDate;
    SteamReleaseStatus status{SteamReleaseStatus::InformationUnavailable};

    QString followedAtUtc;
    QString lastSuccessfulUpdateAtUtc;
    QString lastAttemptedUpdateAtUtc;
    SteamReleaseRefreshState refreshState{SteamReleaseRefreshState::NeverAttempted};
    QString refreshError;
    bool dateChangeUnacknowledged{false};
};

QString steamReleaseDatePrecisionName(SteamReleaseDatePrecision precision);
QString steamReleaseStatusName(SteamReleaseStatus status);
QString steamReleaseRefreshStateName(SteamReleaseRefreshState state);
std::optional<SteamReleaseDatePrecision> steamReleaseDatePrecisionFromName(
    const QString &name);
std::optional<SteamReleaseStatus> steamReleaseStatusFromName(const QString &name);
std::optional<SteamReleaseRefreshState> steamReleaseRefreshStateFromName(
    const QString &name);
bool isValidSteamReleaseDateInfo(const SteamReleaseDateInfo &date);
bool isValidSteamReleaseDetails(const SteamReleaseDetails &details);

} // namespace something::domain

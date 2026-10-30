#pragma once

#include "../domain/app_error.h"
#include "../domain/task.h"
#include "../domain/task_history.h"
#include "../domain/task_scheduling.h"
#include "../domain/weather_currency.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace something::storage {

template<typename T>
struct RepositoryResult {
    T value{};
    domain::AppError error{};
    bool succeeded() const noexcept { return !error.isError() && !error.isCancelled(); }
};

template<>
struct RepositoryResult<void> {
    domain::AppError error{};
    bool succeeded() const noexcept { return !error.isError() && !error.isCancelled(); }
};

using domain::Subtask;
using domain::SubtaskInput;
using domain::Task;
using domain::TaskFilter;
using domain::TaskInput;
using domain::TaskSchedule;
using domain::TaskScheduleProposal;
using domain::TaskScheduleSnapshot;
using domain::SchedulingPreferences;

struct TaskScheduleBatchResult final {
    QVector<TaskSchedule> schedules;
    int changedCount{0};
};

struct TaskHistoryCursor final {
    QString occurredAtUtc;
    qint64 eventId{0};

    bool isValid() const noexcept
    {
        return (occurredAtUtc.isEmpty() && eventId == 0)
            || (!occurredAtUtc.isEmpty() && eventId > 0);
    }
    bool hasValue() const noexcept { return !occurredAtUtc.isEmpty(); }
};

struct TaskHistoryPage final {
    QVector<domain::TaskHistoryEvent> events;
    TaskHistoryCursor nextCursor;
    bool hasMore{false};
    int invalidEventsSkipped{0};
};

struct FavoriteCategory {
    qint64 id{0};
    QString name;
    QString source;
    QString color;
    QString createdAt;
    int displayOrder{0};
    QString updatedAt;
};

struct FavoriteCategoryDeleteResult {
    qint64 deletedId{0};
    QString source;
    int affectedFavorites{0};
};

struct Favorite {
    QString sourceId;
    QString source;
    QString username;
    QString addedAt;
    std::optional<qint64> categoryId;
};

struct FavoriteCheckpoint {
    QString source;
    QString sourceId;
    QString checkedThrough;
    QString lastSuccessAt;
    QString lastAttemptedAt;
    QString lastError;
};

struct FavoriteNewsItem {
    QString source;
    QString sourceId;
    QString itemId;
    QString publishedAt;
    QString discoveredAt;
    QString payloadJson;
};

struct FavoriteNewsState {
    QString scanStartedAt;
    QString errorsJson{QStringLiteral("[]")};
};

struct FavoriteNewsSourceScan final {
    QString source;
    QString sourceId;
    QString checkedThrough;
    QString updateWindowStart;
    bool sourceHasSeenItems{false};
    bool succeeded{false};
    QString error;
    QVector<FavoriteNewsItem> items;
};

struct FavoriteNewsScanCommit final {
    QString scanStartedAt;
    QString errorsJson{QStringLiteral("[]")};
    QVector<FavoriteNewsSourceScan> sources;
};

struct WeatherLocation {
    double latitude{0.0};
    double longitude{0.0};
    QString createdAt;
    QString updatedAt;
    QString forecastJson;
    QString forecastUpdatedAt;
};

struct WeatherComparisonLocation final {
    domain::WeatherCity city;
    int displayOrder{0};
    QString forecastJson;
    QString forecastUpdatedAt;
};

struct CurrencyPair {
    QString baseCode;
    QString quoteCode;
};

struct AssetMetadata {
    QString filename;
    QString mimeType;
    qint64 byteSize{0};
};

struct DesktopApplicationCategory {
    qint64 id{0};
    QString name;
    std::optional<QString> description;
    std::optional<QString> color;
    std::optional<QString> iconIdentifier;
    int displayOrder{0};
    QString createdAt;
    QString updatedAt;
    int applicationCount{0};
};

struct DesktopApplicationCategoryDeleteResult {
    qint64 deletedId{0};
    int affectedApplications{0};
};

struct DesktopApplication {
    qint64 id{0};
    QString displayName;
    QString executablePath;
    QString createdAt;
    QString updatedAt;
    std::optional<AssetMetadata> icon;
    std::optional<qint64> categoryId;
};

struct Setup {
    qint64 id{0};
    QString name;
    QString description;
    QString createdAt;
    QString updatedAt;
    QVector<qint64> applicationIds; // stored launch order
    std::optional<AssetMetadata> icon;
};

struct Wallpaper {
    QString id;
    QString displayName;
    AssetMetadata asset;
    QString createdAt;
};

struct SteamGame {
    qint64 id{0};
    qint64 steamAppId{0};
    QString storeUrl;
    QString name;
    QString imageSourceUrl;
    QString priceStatus;
    QString currency;
    std::optional<qint64> regularPriceMinor;
    std::optional<qint64> currentPriceMinor;
    int discountPercent{0};
    QString priceCountryCode;
    QString lastCheckedAt;
    QString lastAttemptedAt;
    QString lastRefreshError;
    QString createdAt;
    QString updatedAt;
    std::optional<AssetMetadata> artwork;
};

struct SteamSettings {
    QString countryCode{QStringLiteral("TR")};
};

enum class SteamPriceHistoryRange { Days7, Days30, Days90, Year1, All };

struct SteamPriceHistoryStatistics final {
    QString countryCode;
    QString currencyCode;
    QString latestObservationState;
    QString latestObservationAtUtc;
    std::optional<qint64> latestPriceMinor;
    QString latestPriceState;
    QString latestPriceAtUtc;
    std::optional<qint64> lowestPriceMinor;
    std::optional<qint64> highestPriceMinor;
    QString firstObservationAtUtc;
    QString mostRecentObservationAtUtc;
    qint64 observationCount{0};
    qint64 comparablePriceCount{0};
    // True when at least two observations in this market contain a price.
    bool hasSufficientHistory{false};
    bool hasComparableInterval{false};
    QString intervalStartAtUtc;
    QString intervalEndAtUtc;
    std::optional<qint64> intervalStartPriceMinor;
    std::optional<qint64> intervalEndPriceMinor;
    std::optional<qint64> intervalAbsoluteChangeMinor; // signed end minus start
    std::optional<double> intervalPercentageChange;
};

struct SteamPriceHistoryPoint final {
    qint64 id{0};
    QString observedAtUtc;
    qint64 observedAtUtcMs{0};
    QString priceState;
    QString currencyCode;
    std::optional<qint64> priceMinor;
};

struct SteamPriceHistorySnapshot final {
    SteamPriceHistoryStatistics statistics;
    QVector<SteamPriceHistoryPoint> points;
};

struct SteamRefreshUpdate final {
    qint64 gameId{0};
    QString attemptedAt;
    QString safeError;
    std::optional<SteamGame> refreshedGame;
};

} // namespace something::storage

#pragma once

#include "../domain/app_error.h"
#include "../domain/task.h"

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

struct CurrencyPair {
    QString baseCode;
    QString quoteCode;
};

struct AssetMetadata {
    QString filename;
    QString mimeType;
    qint64 byteSize{0};
};

struct DesktopApplication {
    qint64 id{0};
    QString displayName;
    QString executablePath;
    QString createdAt;
    QString updatedAt;
    std::optional<AssetMetadata> icon;
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

struct SteamRefreshUpdate final {
    qint64 gameId{0};
    QString attemptedAt;
    QString safeError;
    std::optional<SteamGame> refreshedGame;
};

} // namespace something::storage

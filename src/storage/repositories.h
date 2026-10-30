#pragma once

#include "repository_types.h"
#include "data_import_repository.h"

#include <QDate>
#include <QSqlDatabase>

#include <functional>

namespace something::storage {

class TaskRepository final {
public:
    explicit TaskRepository(QSqlDatabase &database);
    RepositoryResult<QVector<Task>> list(const TaskFilter &filter = {}) const;
    RepositoryResult<Task> get(qint64 id) const;
    RepositoryResult<Task> create(const TaskInput &input);
    RepositoryResult<Task> update(qint64 id, qint64 expectedRevision, const TaskInput &input);
    RepositoryResult<Task> setCompleted(qint64 id, qint64 expectedRevision, bool completed);
    RepositoryResult<Task> setSubtaskCompleted(qint64 id, qint64 subtaskId,
                                               qint64 expectedRevision, bool completed);
    RepositoryResult<void> remove(qint64 id, qint64 expectedRevision);

private:
    RepositoryResult<Task> load(qint64 id) const;
    bool replaceTags(qint64 taskId, const QStringList &tags, domain::AppError *error);
    bool replaceSubtasks(qint64 taskId, const QVector<SubtaskInput> &subtasks,
                         domain::AppError *error);
    QSqlDatabase &m_database;
};

class TaskHistoryRepository final {
public:
    explicit TaskHistoryRepository(QSqlDatabase &database);
    RepositoryResult<TaskHistoryPage> forTask(
        qint64 taskId, std::optional<domain::TaskHistoryEventType> eventType,
        int pageSize, const TaskHistoryCursor &before = {}) const;
    RepositoryResult<TaskHistoryPage> recent(
        QString taskTitleQuery, std::optional<domain::TaskHistoryEventType> eventType,
        int pageSize, const TaskHistoryCursor &before = {}) const;

private:
    RepositoryResult<TaskHistoryPage> list(
        std::optional<qint64> taskId, QString taskTitleQuery,
        std::optional<domain::TaskHistoryEventType> eventType,
        int pageSize, const TaskHistoryCursor &before) const;
    QSqlDatabase &m_database;
};

class SchedulingRepository final {
public:
    explicit SchedulingRepository(QSqlDatabase &database);
    RepositoryResult<TaskSchedule> taskSchedule(qint64 taskId) const;
    RepositoryResult<QVector<TaskSchedule>> taskSchedules() const;
    RepositoryResult<TaskSchedule> setTaskSchedule(const TaskSchedule &schedule,
                                                   qint64 expectedRevision);
    RepositoryResult<TaskScheduleBatchResult> applyTaskSchedules(
        const QVector<TaskScheduleProposal> &proposals,
        const QVector<TaskScheduleSnapshot> &taskSnapshot,
        qint64 expectedPreferencesRevision,
        const std::function<bool()> &cancelled = {});
    RepositoryResult<SchedulingPreferences> schedulingPreferences() const;
    RepositoryResult<SchedulingPreferences> setSchedulingPreferences(
        const SchedulingPreferences &preferences, qint64 expectedRevision);
private:
    RepositoryResult<TaskSchedule> loadTaskSchedule(qint64 taskId) const;
    QSqlDatabase &m_database;
};

class CurrencyFavoritesRepository final {
public:
    explicit CurrencyFavoritesRepository(QSqlDatabase &database);
    RepositoryResult<QVector<CurrencyPair>> list() const;
    RepositoryResult<void> add(QString baseCode, QString quoteCode);
    RepositoryResult<void> remove(QString baseCode, QString quoteCode);
private:
    QSqlDatabase &m_database;
};

class FavoritesRepository final {
public:
    explicit FavoritesRepository(QSqlDatabase &database);
    RepositoryResult<QVector<FavoriteCategory>> categories(QString source) const;
    RepositoryResult<FavoriteCategory> createCategory(QString source, QString name, QString color);
    RepositoryResult<FavoriteCategory> updateCategory(qint64 id, QString source,
                                                       QString name, QString color);
    RepositoryResult<void> reorderCategories(QString source, const QVector<qint64> &orderedIds);
    RepositoryResult<FavoriteCategoryDeleteResult> removeCategory(
        qint64 id, QString mode = QStringLiteral("unassign"),
        std::optional<qint64> targetCategoryId = std::nullopt);
    RepositoryResult<QVector<Favorite>> favorites(QString source) const;
    RepositoryResult<void> addFavorite(QString source, QString sourceId, QString username);
    RepositoryResult<void> assignFavoriteCategory(QString source, QString sourceId,
                                                  std::optional<qint64> categoryId);
    RepositoryResult<void> removeFavorite(QString source, QString sourceId);
private:
    QSqlDatabase &m_database;
};

class FavoriteNewsRepository final {
public:
    explicit FavoriteNewsRepository(QSqlDatabase &database);
    RepositoryResult<void> recordAppOpen(QString now, QString retentionCutoff);
    RepositoryResult<std::optional<FavoriteCheckpoint>> checkpoint(QString source, QString sourceId) const;
    RepositoryResult<void> saveCheckpoint(const FavoriteCheckpoint &checkpoint);
    RepositoryResult<bool> hasSeen(QString source, QString sourceId, QString itemId) const;
    RepositoryResult<bool> sourceHasSeenItems(QString source, QString sourceId) const;
    RepositoryResult<void> saveSeen(const QVector<FavoriteNewsItem> &items);
    RepositoryResult<void> saveNews(const QVector<FavoriteNewsItem> &items);
    RepositoryResult<QVector<FavoriteNewsItem>> recentNews(int limit) const;
    RepositoryResult<QVector<FavoriteNewsItem>> newsForScan(QString scanStartedAt, int limit) const;
    RepositoryResult<void> saveScanState(const FavoriteNewsState &state);
    RepositoryResult<std::optional<FavoriteNewsState>> scanState() const;
    RepositoryResult<void> pruneBefore(QString discoveredBefore);
    RepositoryResult<QVector<FavoriteNewsItem>> commitScan(
        const FavoriteNewsScanCommit &scan,
        const std::function<bool()> &cancellationRequested = {});
private:
    QSqlDatabase &m_database;
};

class WeatherRepository final {
public:
    explicit WeatherRepository(QSqlDatabase &database);
    RepositoryResult<std::optional<WeatherLocation>> load() const;
    RepositoryResult<void> saveLocation(double latitude, double longitude);
    RepositoryResult<void> saveLocationAndForecast(double latitude, double longitude,
                                                   QString forecastJson, QString updatedAt);
    RepositoryResult<void> saveForecast(QString forecastJson, QString updatedAt);
    RepositoryResult<void> clear();
private:
    QSqlDatabase &m_database;
};

class WeatherComparisonRepository final {
public:
    explicit WeatherComparisonRepository(QSqlDatabase &database);
    RepositoryResult<QVector<WeatherComparisonLocation>> load() const;
    RepositoryResult<void> add(const domain::WeatherCity &city,
                               QString forecastJson, QString forecastUpdatedAt);
    RepositoryResult<void> replace(const QString &identity,
                                  const domain::WeatherCity &city,
                                  QString forecastJson, QString forecastUpdatedAt);
    RepositoryResult<void> remove(const QString &identity);
    RepositoryResult<void> reorder(const QVector<QString> &orderedIdentities);
    RepositoryResult<void> saveForecast(const QString &identity,
                                        QString forecastJson, QString forecastUpdatedAt);
private:
    QSqlDatabase &m_database;
};

class PreferencesRepository final {
public:
    explicit PreferencesRepository(QSqlDatabase &database);
    RepositoryResult<QString> get(QString key) const;
    RepositoryResult<void> set(QString key, QString value);
    RepositoryResult<int> weekStart() const;
    RepositoryResult<void> setWeekStart(int sundayZero);
    RepositoryResult<bool> gameReleasesVisibleInCalendar() const;
    RepositoryResult<void> setGameReleasesVisibleInCalendar(bool visible);
    RepositoryResult<QString> selectedWallpaper() const;
    RepositoryResult<void> setSelectedWallpaper(QString wallpaperId);
private:
    QSqlDatabase &m_database;
};

class DesktopApplicationsRepository final {
public:
    explicit DesktopApplicationsRepository(QSqlDatabase &database);
    RepositoryResult<QVector<DesktopApplication>> list() const;
    RepositoryResult<QVector<DesktopApplication>> listByCategory(qint64 categoryId) const;
    RepositoryResult<QVector<DesktopApplication>> listUncategorized() const;
    RepositoryResult<DesktopApplication> get(qint64 id) const;
    RepositoryResult<QVector<DesktopApplicationCategory>> listCategories() const;
    RepositoryResult<int> uncategorizedApplicationCount() const;
    RepositoryResult<DesktopApplicationCategory> createCategory(
        QString name, QString description, QString color, QString iconIdentifier);
    RepositoryResult<DesktopApplicationCategory> updateCategory(
        qint64 id, QString name, QString description, QString color, QString iconIdentifier);
    RepositoryResult<void> reorderCategories(const QVector<qint64> &orderedIds);
    RepositoryResult<void> assignCategory(qint64 applicationId,
                                          std::optional<qint64> categoryId);
    RepositoryResult<DesktopApplicationCategoryDeleteResult> deleteCategory(qint64 id);
    RepositoryResult<DesktopApplication> add(QString displayName, QString executablePath,
                                              std::optional<AssetMetadata> icon = std::nullopt);
    RepositoryResult<DesktopApplication> update(qint64 id, QString displayName,
                                                 QString executablePath);
    RepositoryResult<void> setIcon(qint64 appId, const AssetMetadata &asset);
    RepositoryResult<void> removeIcon(qint64 appId);
    RepositoryResult<void> remove(qint64 id);
private:
    RepositoryResult<DesktopApplication> load(qint64 id) const;
    RepositoryResult<QVector<DesktopApplication>> listFiltered(
        std::optional<qint64> categoryId, bool uncategorized) const;
    RepositoryResult<DesktopApplicationCategory> loadCategory(qint64 id) const;
    QSqlDatabase &m_database;
};

class SetupsRepository final {
public:
    explicit SetupsRepository(QSqlDatabase &database);
    RepositoryResult<QVector<Setup>> list() const;
    RepositoryResult<Setup> get(qint64 id) const;
    RepositoryResult<Setup> save(Setup setup, bool removeIcon = false);
    RepositoryResult<void> setIcon(qint64 setupId, const AssetMetadata &asset);
    RepositoryResult<void> removeIcon(qint64 setupId);
    RepositoryResult<void> remove(qint64 id);
private:
    RepositoryResult<Setup> load(qint64 id) const;
    QSqlDatabase &m_database;
};

class WallpapersRepository final {
public:
    explicit WallpapersRepository(QSqlDatabase &database);
    RepositoryResult<QVector<Wallpaper>> list() const;
    RepositoryResult<void> add(const Wallpaper &wallpaper);
    RepositoryResult<void> addAndSelect(
        const Wallpaper &wallpaper, QString selection,
        const std::function<bool()> &cancellationRequested = {});
    RepositoryResult<void> remove(QString id,
                                  const std::function<bool()> &cancellationRequested = {});
private:
    QSqlDatabase &m_database;
};

class SteamRepository final {
public:
    explicit SteamRepository(QSqlDatabase &database);
    RepositoryResult<SteamSettings> settings() const;
    RepositoryResult<void> setCountry(QString countryCode);
    RepositoryResult<QVector<SteamGame>> list() const;
    RepositoryResult<SteamGame> get(qint64 id) const;
    RepositoryResult<QVector<SteamPriceHistoryMarket>> priceHistoryMarkets(
        qint64 gameId,
        const std::function<bool()> &cancellationRequested = {}) const;
    RepositoryResult<SteamPriceHistorySnapshot> priceHistory(
        qint64 gameId, QString countryCode, QString currencyCode,
        SteamPriceHistoryRange range,
        const std::function<bool()> &cancellationRequested = {}) const;
    RepositoryResult<SteamGame> add(
        SteamGame game,
        const std::function<bool()> &cancellationRequested = {});
    RepositoryResult<SteamGame> updateRefresh(
        SteamGame game,
        const std::function<bool()> &cancellationRequested = {});
    RepositoryResult<void> applyRefreshBatch(
        const QVector<SteamRefreshUpdate> &updates,
        const std::function<bool()> &cancellationRequested = {});
    RepositoryResult<void> setArtwork(qint64 gameId, const AssetMetadata &asset);
    RepositoryResult<void> removeArtwork(qint64 gameId);
    RepositoryResult<bool> hasArtworkFilename(const QString &filename) const;
    RepositoryResult<void> remove(qint64 id);
private:
    RepositoryResult<SteamGame> load(qint64 id) const;
    QSqlDatabase &m_database;
};

class SteamReleaseRepository final {
public:
    explicit SteamReleaseRepository(QSqlDatabase &database);
    RepositoryResult<QVector<domain::SteamTrackedRelease>> list() const;
    RepositoryResult<QVector<domain::SteamTrackedRelease>> exactDateEvents(
        QDate startDate, QDate endDate,
        const std::function<bool()> &cancellationRequested = {}) const;
    RepositoryResult<domain::SteamTrackedRelease> get(quint32 appId) const;
    RepositoryResult<domain::SteamTrackedRelease> add(
        domain::SteamTrackedRelease release,
        const std::function<bool()> &cancellationRequested = {});
    RepositoryResult<void> applyRefreshBatch(
        const QVector<SteamReleaseRefreshUpdate> &updates,
        const std::function<bool()> &cancellationRequested = {});
    RepositoryResult<void> acknowledgeDateChange(quint32 appId);
    RepositoryResult<void> setArtwork(
        quint32 appId, const QString &expectedSourceUrl,
        const domain::SteamReleaseArtworkMetadata &artwork);
    RepositoryResult<bool> hasArtworkFilename(const QString &filename) const;
    RepositoryResult<void> remove(quint32 appId);

private:
    RepositoryResult<domain::SteamTrackedRelease> load(quint32 appId) const;
    QSqlDatabase &m_database;
};

class SteamGameUpdatesRepository final {
public:
    static constexpr int maximumArticlesPerGame = 250;
    static constexpr int maximumArticlesOverall = 3000;
    static constexpr int maximumArticleContentCharacters = 6000;
    static constexpr int maximumNewsPageSize = 100;

    explicit SteamGameUpdatesRepository(QSqlDatabase &database);
    RepositoryResult<QVector<domain::SteamGameUpdateFollow>> list(
        const std::function<bool()> &cancellationRequested = {}) const;
    RepositoryResult<domain::SteamGameUpdateFollow> get(quint32 appId) const;
    RepositoryResult<domain::SteamGameUpdateFollow> add(
        domain::SteamGameUpdateFollow follow,
        const std::function<bool()> &cancellationRequested = {});
    RepositoryResult<domain::SteamGameUpdateFollow> setOwnershipStatus(
        quint32 appId, domain::SteamGameUpdateOwnershipStatus status);
    RepositoryResult<domain::SteamGameUpdateFollow> recordNewsCheck(
        quint32 appId, QString attemptedAtUtc, bool succeeded, QString safeError);
    RepositoryResult<void> setArtwork(
        quint32 appId, const QString &expectedSourceUrl,
        const domain::SteamGameUpdateArtworkMetadata &artwork);
    RepositoryResult<bool> hasArtworkFilename(const QString &filename) const;
    RepositoryResult<SteamGameUpdateNewsPage> newsPage(
        std::optional<quint32> appId, bool unreadOnly, QString titleQuery, int pageSize,
        const SteamGameUpdateNewsCursor &before = {},
        const std::function<bool()> &cancellationRequested = {}) const;
    RepositoryResult<std::optional<qint64>> oldestNewsPublishedAt(quint32 appId) const;
    RepositoryResult<QHash<quint32, int>> unreadNewsCounts() const;
    RepositoryResult<SteamGameUpdateNewsUpsertResult> upsertNewsPage(
        quint32 appId, const QVector<domain::SteamNewsArticle> &articles,
        const QString &discoveredAtUtc,
        const std::function<bool()> &cancellationRequested = {});
    RepositoryResult<void> setNewsRead(quint32 appId, const QString &newsId, bool isRead);
    RepositoryResult<int> setAllNewsRead(quint32 appId, bool isRead);
    RepositoryResult<void> remove(quint32 appId);

private:
    RepositoryResult<domain::SteamGameUpdateFollow> load(quint32 appId) const;
    QSqlDatabase &m_database;
};

// Constructed only by StorageWorker after migration. It is never passed across
// threads; callers submit value-only commands to the worker.
class StorageRepositories final {
public:
    explicit StorageRepositories(QSqlDatabase &database);
    TaskRepository tasks;
    TaskHistoryRepository taskHistory;
    SchedulingRepository scheduling;
    CurrencyFavoritesRepository currencies;
    FavoritesRepository favorites;
    FavoriteNewsRepository news;
    WeatherRepository weather;
    WeatherComparisonRepository weatherComparison;
    PreferencesRepository preferences;
    DesktopApplicationsRepository applications;
    SetupsRepository setups;
    WallpapersRepository wallpapers;
    SteamRepository steam;
    SteamReleaseRepository steamReleases;
    SteamGameUpdatesRepository gameUpdates;
    DataImportRepository dataImport;
};

} // namespace something::storage

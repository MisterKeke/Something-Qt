#pragma once

#include <QSqlDatabase>
#include <QVariantMap>
#include <QVector>
#include <QString>

namespace something::storage {

struct ApplicationUsageMetadata final {
    QString applicationId;
    QString executablePath;
    QString executableName;
    QString displayName;
    QString firstSeenUtc;
    QString lastSeenUtc;
};

struct ApplicationUsageSegment final {
    QString segmentId;
    QString sessionId;
    QString applicationId;
    QString state;
    QString startedUtc;
    QString endedUtc;
    QString localDay;
    int localHour{0};
    qint64 durationMs{0};
};

struct ApplicationUsageSession final {
    QString sessionId;
    QString applicationId;
    QString startedUtc;
    QString endedUtc;
    QString lastCheckpointUtc;
    QString endReason;
    qint64 durationMs{0};
    bool active{false};
};

struct ApplicationUsageCheckpoint final {
    QVector<ApplicationUsageMetadata> applications;
    QVector<ApplicationUsageSession> sessions;
    QVector<ApplicationUsageSegment> segments;
};

struct ApplicationUsageExclusion final {
    QString applicationId;
    QString canonicalPath;
    QString executableName;
    bool matchByName{false};
    QString createdAtUtc;
};

class ApplicationUsageRepository final {
public:
    explicit ApplicationUsageRepository(QSqlDatabase &database);

    bool initialize(QString *diagnostic = nullptr);
    bool loadConfiguration(bool *enabled, int *retentionDays,
                           QVector<ApplicationUsageExclusion> *exclusions,
                           QString *diagnostic = nullptr) const;
    bool setEnabled(bool enabled, QString *diagnostic = nullptr);
    bool setRetentionDays(int days, QString *diagnostic = nullptr);
    bool addExclusion(const ApplicationUsageExclusion &exclusion,
                      QString *diagnostic = nullptr);
    bool removeExclusion(const QString &applicationId, QString *diagnostic = nullptr);
    bool recordCheckpoint(const ApplicationUsageCheckpoint &checkpoint,
                          int retentionDays, QString *diagnostic = nullptr);
    bool recoverInterruptedSessions(const QString &nowUtc, QString *diagnostic = nullptr);
    QVariantMap queryOverview(const QString &firstLocalDay, const QString &lastLocalDay,
                              const QString &startUtc, const QString &endUtc,
                              const QString &search, const QString &sort,
                              QString *diagnostic = nullptr) const;
    QVariantList applicationChoices(QString *diagnostic = nullptr) const;
    QVariantMap queryApplication(const QString &applicationId,
                                 const QString &firstLocalDay,
                                 const QString &lastLocalDay,
                                 const QString &startUtc,
                                 const QString &endUtc,
                                 QString *diagnostic = nullptr) const;
    bool deleteHistory(const QString &applicationId, QString *diagnostic = nullptr);
    QVariantMap storageInformation(QString *diagnostic = nullptr) const;

private:
    QSqlDatabase &m_database;
};

} // namespace something::storage

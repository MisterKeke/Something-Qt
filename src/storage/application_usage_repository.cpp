#include "application_usage_repository.h"

#include <QDate>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace something::storage {
namespace {

bool fail(QString *diagnostic, const QSqlQuery &query)
{
    if (diagnostic) *diagnostic = query.lastError().text();
    return false;
}

QString overviewOrder(const QString &sort)
{
    if (sort == QStringLiteral("foreground")) return QStringLiteral("foreground_ms DESC, total_ms DESC");
    if (sort == QStringLiteral("fullscreen")) return QStringLiteral("fullscreen_ms DESC, total_ms DESC");
    if (sort == QStringLiteral("name")) return QStringLiteral("display_name COLLATE NOCASE ASC");
    if (sort == QStringLiteral("lastActivity")) return QStringLiteral("last_seen_utc DESC");
    return QStringLiteral("total_ms DESC, display_name COLLATE NOCASE ASC");
}

void bindDayRange(QSqlQuery &query, const QString &firstDay, const QString &lastDay)
{
    query.addBindValue(firstDay);
    query.addBindValue(lastDay);
}

bool pruneExpiredData(QSqlDatabase &database, int retentionDays, QString *diagnostic)
{
    if (retentionDays <= 0) return true;
    const QDate cutoff = QDate::currentDate().addDays(1 - retentionDays);
    const QString cutoffDay = cutoff.toString(Qt::ISODate);
    const QString cutoffUtc = QDateTime(cutoff, QTime(0, 0), Qt::LocalTime)
                                  .toUTC().toString(Qt::ISODateWithMs);
    QSqlQuery segments(database);
    segments.prepare(QStringLiteral("DELETE FROM usage_segments WHERE local_day < ?"));
    segments.addBindValue(cutoffDay);
    if (!segments.exec()) return fail(diagnostic, segments);

    QSqlQuery sessions(database);
    sessions.prepare(QStringLiteral(
        "DELETE FROM usage_sessions WHERE ended_utc IS NOT NULL AND last_checkpoint_utc < ? "
        "AND NOT EXISTS(SELECT 1 FROM usage_segments s WHERE s.session_id=usage_sessions.session_id)"));
    sessions.addBindValue(cutoffUtc);
    if (!sessions.exec()) return fail(diagnostic, sessions);

    QSqlQuery applications(database);
    applications.prepare(QStringLiteral(
        "DELETE FROM usage_tracked_applications WHERE last_seen_utc < ? "
        "AND NOT EXISTS(SELECT 1 FROM usage_sessions u WHERE u.application_id=usage_tracked_applications.application_id) "
        "AND NOT EXISTS(SELECT 1 FROM usage_segments s WHERE s.application_id=usage_tracked_applications.application_id)"));
    applications.addBindValue(cutoffUtc);
    return applications.exec() || fail(diagnostic, applications);
}

} // namespace

ApplicationUsageRepository::ApplicationUsageRepository(QSqlDatabase &database)
    : m_database(database)
{
}

bool ApplicationUsageRepository::initialize(QString *diagnostic)
{
    Q_UNUSED(diagnostic);
    return m_database.isOpen();
}

bool ApplicationUsageRepository::loadConfiguration(
    bool *enabled, int *retentionDays,
    QVector<ApplicationUsageExclusion> *exclusions, QString *diagnostic) const
{
    if (!enabled || !retentionDays || !exclusions) return false;
    QSqlQuery settings(m_database);
    if (!settings.exec(QStringLiteral("SELECT key, value FROM usage_tracking_settings")))
        return fail(diagnostic, settings);
    *enabled = true;
    *retentionDays = 365;
    while (settings.next()) {
        const QString key = settings.value(0).toString();
        const QString value = settings.value(1).toString();
        if (key == QStringLiteral("enabled")) *enabled = value == QStringLiteral("1");
        else if (key == QStringLiteral("retention_days")) *retentionDays = value.toInt();
    }

    exclusions->clear();
    QSqlQuery rows(m_database);
    if (!rows.exec(QStringLiteral(
            "SELECT application_id, canonical_path, executable_name, match_by_name, created_at_utc "
            "FROM usage_excluded_applications ORDER BY executable_name COLLATE NOCASE")))
        return fail(diagnostic, rows);
    while (rows.next()) {
        exclusions->push_back({rows.value(0).toString(), rows.value(1).toString(),
                               rows.value(2).toString(), rows.value(3).toBool(),
                               rows.value(4).toString()});
    }
    return true;
}

bool ApplicationUsageRepository::setEnabled(bool enabled, QString *diagnostic)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO usage_tracking_settings(key, value) VALUES('enabled', ?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    query.addBindValue(enabled ? QStringLiteral("1") : QStringLiteral("0"));
    return query.exec() || fail(diagnostic, query);
}

bool ApplicationUsageRepository::setRetentionDays(int days, QString *diagnostic)
{
    if (days < 0 || days > 3650) return false;
    if (!m_database.transaction()) {
        if (diagnostic) *diagnostic = m_database.lastError().text();
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO usage_tracking_settings(key, value) VALUES('retention_days', ?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    query.addBindValue(QString::number(days));
    if (!query.exec()) {
        fail(diagnostic, query);
        m_database.rollback();
        return false;
    }
    if (!pruneExpiredData(m_database, days, diagnostic)) {
        m_database.rollback();
        return false;
    }
    if (!m_database.commit()) {
        if (diagnostic) *diagnostic = m_database.lastError().text();
        m_database.rollback();
        return false;
    }
    return true;
}

bool ApplicationUsageRepository::addExclusion(const ApplicationUsageExclusion &exclusion,
                                               QString *diagnostic)
{
    if (exclusion.applicationId.trimmed().isEmpty()
        || (exclusion.canonicalPath.isEmpty() && exclusion.executableName.isEmpty())) return false;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO usage_excluded_applications(application_id, canonical_path, executable_name, "
        "match_by_name, created_at_utc) VALUES(?, ?, ?, ?, ?) "
        "ON CONFLICT(application_id) DO UPDATE SET canonical_path=excluded.canonical_path, "
        "executable_name=excluded.executable_name, match_by_name=excluded.match_by_name"));
    query.addBindValue(exclusion.applicationId);
    query.addBindValue(exclusion.canonicalPath);
    query.addBindValue(exclusion.executableName);
    query.addBindValue(exclusion.matchByName ? 1 : 0);
    query.addBindValue(exclusion.createdAtUtc);
    return query.exec() || fail(diagnostic, query);
}

bool ApplicationUsageRepository::removeExclusion(const QString &applicationId,
                                                  QString *diagnostic)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM usage_excluded_applications WHERE application_id=?"));
    query.addBindValue(applicationId);
    return query.exec() || fail(diagnostic, query);
}

bool ApplicationUsageRepository::recordCheckpoint(const ApplicationUsageCheckpoint &checkpoint,
                                                   int retentionDays, QString *diagnostic)
{
    if (!m_database.transaction()) {
        if (diagnostic) *diagnostic = m_database.lastError().text();
        return false;
    }
    auto rollback = [this] { m_database.rollback(); };

    QSqlQuery app(m_database);
    app.prepare(QStringLiteral(
        "INSERT INTO usage_tracked_applications(application_id, canonical_path, executable_name, "
        "display_name, first_seen_utc, last_seen_utc, last_detection_status) VALUES(?, ?, ?, ?, ?, ?, 'eligible') "
        "ON CONFLICT(application_id) DO UPDATE SET "
        "canonical_path=CASE WHEN excluded.canonical_path<>'' THEN excluded.canonical_path ELSE canonical_path END, "
        "executable_name=CASE WHEN excluded.executable_name<>'' THEN excluded.executable_name ELSE executable_name END, "
        "display_name=CASE WHEN excluded.display_name<>'' THEN excluded.display_name ELSE display_name END, "
        "first_seen_utc=MIN(first_seen_utc, excluded.first_seen_utc), "
        "last_seen_utc=MAX(last_seen_utc, excluded.last_seen_utc), last_detection_status='eligible'"));
    for (const auto &record : checkpoint.applications) {
        app.bindValue(0, record.applicationId);
        app.bindValue(1, record.executablePath);
        app.bindValue(2, record.executableName);
        app.bindValue(3, record.displayName);
        app.bindValue(4, record.firstSeenUtc);
        app.bindValue(5, record.lastSeenUtc);
        if (!app.exec()) { fail(diagnostic, app); rollback(); return false; }
    }

    QSqlQuery session(m_database);
    session.prepare(QStringLiteral(
        "INSERT INTO usage_sessions(session_id, application_id, started_utc, ended_utc, duration_ms, "
        "end_reason, last_checkpoint_utc) VALUES(?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(session_id) DO UPDATE SET ended_utc=excluded.ended_utc, "
        "duration_ms=excluded.duration_ms, end_reason=excluded.end_reason, "
        "last_checkpoint_utc=excluded.last_checkpoint_utc"));
    for (const auto &record : checkpoint.sessions) {
        session.bindValue(0, record.sessionId);
        session.bindValue(1, record.applicationId);
        session.bindValue(2, record.startedUtc);
        session.bindValue(3, record.active ? QVariant() : QVariant(record.endedUtc));
        session.bindValue(4, record.durationMs);
        session.bindValue(5, record.active ? QVariant() : QVariant(record.endReason));
        session.bindValue(6, record.lastCheckpointUtc);
        if (!session.exec()) { fail(diagnostic, session); rollback(); return false; }
    }

    QSqlQuery segment(m_database);
    segment.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO usage_segments(segment_id, session_id, application_id, state, "
        "started_utc, ended_utc, local_day, local_hour, duration_ms) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    for (const auto &record : checkpoint.segments) {
        if (record.durationMs <= 0) continue;
        segment.bindValue(0, record.segmentId);
        segment.bindValue(1, record.sessionId);
        segment.bindValue(2, record.applicationId);
        segment.bindValue(3, record.state);
        segment.bindValue(4, record.startedUtc);
        segment.bindValue(5, record.endedUtc);
        segment.bindValue(6, record.localDay);
        segment.bindValue(7, record.localHour);
        segment.bindValue(8, record.durationMs);
        if (!segment.exec()) { fail(diagnostic, segment); rollback(); return false; }
    }

    if (!pruneExpiredData(m_database, retentionDays, diagnostic)) {
        rollback();
        return false;
    }
    if (!m_database.commit()) {
        if (diagnostic) *diagnostic = m_database.lastError().text();
        rollback();
        return false;
    }
    return true;
}

bool ApplicationUsageRepository::recoverInterruptedSessions(const QString &nowUtc,
                                                             QString *diagnostic)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE usage_sessions SET ended_utc=last_checkpoint_utc, end_reason='interrupted' "
        "WHERE ended_utc IS NULL AND last_checkpoint_utc <= ?"));
    query.addBindValue(nowUtc);
    return query.exec() || fail(diagnostic, query);
}

QVariantMap ApplicationUsageRepository::queryOverview(
    const QString &firstLocalDay, const QString &lastLocalDay,
    const QString &startUtc, const QString &endUtc, const QString &search,
    const QString &sort, QString *diagnostic) const
{
    Q_UNUSED(startUtc);
    Q_UNUSED(endUtc);
    const QString order = overviewOrder(sort);
    const QString aggregateRows = QStringLiteral(
        "SELECT a.application_id, a.display_name, "
        "COALESCE(SUM(s.duration_ms),0) AS total_ms, "
        "COALESCE(SUM(CASE WHEN s.state IN ('ForegroundFullscreen','ForegroundWindowed') THEN s.duration_ms ELSE 0 END),0) AS foreground_ms, "
        "COALESCE(SUM(CASE WHEN s.state='ForegroundFullscreen' THEN s.duration_ms ELSE 0 END),0) AS fullscreen_ms, "
        "EXISTS(SELECT 1 FROM usage_sessions live WHERE live.application_id=a.application_id AND live.ended_utc IS NULL) AS is_running "
        "FROM usage_tracked_applications a LEFT JOIN usage_segments s ON s.application_id=a.application_id "
        "AND s.local_day BETWEEN ? AND ? WHERE (?='' OR instr(lower(a.display_name), lower(?))>0 "
        "OR instr(lower(a.executable_name), lower(?))>0) GROUP BY a.application_id "
        "HAVING total_ms>0 OR is_running");
    const auto bindOverviewFilters = [&](QSqlQuery &query, const QString &nameFilter) {
        bindDayRange(query, firstLocalDay, lastLocalDay);
        query.addBindValue(nameFilter.trimmed());
        query.addBindValue(nameFilter.trimmed());
        query.addBindValue(nameFilter.trimmed());
    };
    QSqlQuery totalsQuery(m_database);
    totalsQuery.prepare(QStringLiteral(
        "SELECT COUNT(*), COALESCE(SUM(total_ms),0), COALESCE(SUM(foreground_ms),0), "
        "COALESCE(SUM(fullscreen_ms),0) FROM (%1) usage_rows").arg(aggregateRows));
    bindOverviewFilters(totalsQuery, {});
    if (!totalsQuery.exec() || !totalsQuery.next()) {
        fail(diagnostic, totalsQuery);
        return {};
    }
    const qint64 applicationCount = totalsQuery.value(0).toLongLong();
    const qint64 total = totalsQuery.value(1).toLongLong();
    const qint64 foreground = totalsQuery.value(2).toLongLong();
    const qint64 fullscreen = totalsQuery.value(3).toLongLong();

    QSqlQuery matchingTotalsQuery(m_database);
    matchingTotalsQuery.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM (%1) usage_rows").arg(aggregateRows));
    bindOverviewFilters(matchingTotalsQuery, search);
    if (!matchingTotalsQuery.exec() || !matchingTotalsQuery.next()) {
        fail(diagnostic, matchingTotalsQuery);
        return {};
    }
    const qint64 matchingApplicationCount = matchingTotalsQuery.value(0).toLongLong();

    QSqlQuery mostUsedQuery(m_database);
    mostUsedQuery.prepare(QStringLiteral(
        "SELECT display_name,total_ms FROM (%1) usage_rows WHERE total_ms>0 "
        "ORDER BY total_ms DESC,display_name COLLATE NOCASE ASC LIMIT 1").arg(aggregateRows));
    bindOverviewFilters(mostUsedQuery, {});
    if (!mostUsedQuery.exec()) { fail(diagnostic, mostUsedQuery); return {}; }
    QVariantMap mostUsed;
    if (mostUsedQuery.next()) {
        mostUsed.insert(QStringLiteral("name"), mostUsedQuery.value(0));
        mostUsed.insert(QStringLiteral("totalMs"), mostUsedQuery.value(1).toLongLong());
    }

    const QString sql = QStringLiteral(
        "SELECT a.application_id, a.display_name, a.canonical_path, a.executable_name, "
        "a.first_seen_utc, a.last_seen_utc, "
        "COALESCE(SUM(s.duration_ms),0) AS total_ms, "
        "COALESCE(SUM(CASE WHEN s.state IN ('ForegroundFullscreen','ForegroundWindowed') THEN s.duration_ms ELSE 0 END),0) AS foreground_ms, "
        "COALESCE(SUM(CASE WHEN s.state='ForegroundFullscreen' THEN s.duration_ms ELSE 0 END),0) AS fullscreen_ms, "
        "COALESCE(SUM(CASE WHEN s.state='ForegroundWindowed' THEN s.duration_ms ELSE 0 END),0) AS windowed_ms, "
        "COALESCE(SUM(CASE WHEN s.state='BackgroundVisible' THEN s.duration_ms ELSE 0 END),0) AS background_ms, "
        "COALESCE(SUM(CASE WHEN s.state='Minimized' THEN s.duration_ms ELSE 0 END),0) AS minimized_ms, "
        "COALESCE(SUM(CASE WHEN s.state='Unknown' THEN s.duration_ms ELSE 0 END),0) AS unknown_ms, "
        "COUNT(DISTINCT s.session_id) AS session_count, "
        "EXISTS(SELECT 1 FROM usage_sessions live WHERE live.application_id=a.application_id AND live.ended_utc IS NULL) AS is_running "
        "FROM usage_tracked_applications a LEFT JOIN usage_segments s ON s.application_id=a.application_id "
        "AND s.local_day BETWEEN ? AND ? WHERE (?='' OR instr(lower(a.display_name), lower(?))>0 "
        "OR instr(lower(a.executable_name), lower(?))>0) GROUP BY a.application_id "
        "HAVING total_ms>0 OR is_running ORDER BY %1 LIMIT 500").arg(order);
    QSqlQuery query(m_database);
    query.prepare(sql);
    bindOverviewFilters(query, search);
    if (!query.exec()) { fail(diagnostic, query); return {}; }

    QVariantList applications;
    while (query.next()) {
        QVariantMap row;
        row.insert(QStringLiteral("applicationId"), query.value(0));
        row.insert(QStringLiteral("name"), query.value(1));
        row.insert(QStringLiteral("executablePath"), query.value(2));
        row.insert(QStringLiteral("executableName"), query.value(3));
        row.insert(QStringLiteral("firstSeenUtc"), query.value(4));
        row.insert(QStringLiteral("lastSeenUtc"), query.value(5));
        row.insert(QStringLiteral("totalMs"), query.value(6).toLongLong());
        row.insert(QStringLiteral("foregroundMs"), query.value(7).toLongLong());
        row.insert(QStringLiteral("fullscreenMs"), query.value(8).toLongLong());
        row.insert(QStringLiteral("windowedMs"), query.value(9).toLongLong());
        row.insert(QStringLiteral("backgroundMs"), query.value(10).toLongLong());
        row.insert(QStringLiteral("minimizedMs"), query.value(11).toLongLong());
        row.insert(QStringLiteral("unknownMs"), query.value(12).toLongLong());
        row.insert(QStringLiteral("sessionCount"), query.value(13).toInt());
        row.insert(QStringLiteral("running"), query.value(14).toBool());
        applications.push_back(row);
    }

    QVariantMap result;
    result.insert(QStringLiteral("applications"), applications);
    result.insert(QStringLiteral("totalMs"), total);
    result.insert(QStringLiteral("foregroundMs"), foreground);
    result.insert(QStringLiteral("fullscreenMs"), fullscreen);
    result.insert(QStringLiteral("applicationCount"), applicationCount);
    result.insert(QStringLiteral("matchingApplicationCount"), matchingApplicationCount);
    result.insert(QStringLiteral("mostUsed"), mostUsed);
    if (diagnostic) diagnostic->clear();
    return result;
}

QVariantList ApplicationUsageRepository::applicationChoices(QString *diagnostic) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT application_id,display_name,canonical_path,executable_name "
            "FROM usage_tracked_applications ORDER BY display_name COLLATE NOCASE,application_id"))) {
        fail(diagnostic, query);
        return {};
    }
    QVariantList choices;
    while (query.next()) {
        choices.push_back(QVariantMap{
            {QStringLiteral("applicationId"), query.value(0)},
            {QStringLiteral("name"), query.value(1)},
            {QStringLiteral("executablePath"), query.value(2)},
            {QStringLiteral("executableName"), query.value(3)}});
    }
    if (diagnostic) diagnostic->clear();
    return choices;
}

QVariantMap ApplicationUsageRepository::queryApplication(
    const QString &applicationId, const QString &firstLocalDay,
    const QString &lastLocalDay, const QString &startUtc, const QString &endUtc,
    QString *diagnostic) const
{
    Q_UNUSED(startUtc);
    Q_UNUSED(endUtc);
    QSqlQuery app(m_database);
    app.prepare(QStringLiteral(
        "SELECT display_name, canonical_path, executable_name, first_seen_utc, last_seen_utc "
        "FROM usage_tracked_applications WHERE application_id=?"));
    app.addBindValue(applicationId);
    if (!app.exec()) { fail(diagnostic, app); return {}; }
    if (!app.next()) return {};

    QVariantMap result{{QStringLiteral("applicationId"), applicationId},
                       {QStringLiteral("name"), app.value(0)},
                       {QStringLiteral("executablePath"), app.value(1)},
                       {QStringLiteral("executableName"), app.value(2)},
                       {QStringLiteral("firstSeenUtc"), app.value(3)},
                       {QStringLiteral("lastSeenUtc"), app.value(4)}};
    QSqlQuery states(m_database);
    states.prepare(QStringLiteral(
        "SELECT state, COALESCE(SUM(duration_ms),0) FROM usage_segments "
        "WHERE application_id=? AND local_day BETWEEN ? AND ? GROUP BY state"));
    states.addBindValue(applicationId);
    bindDayRange(states, firstLocalDay, lastLocalDay);
    if (!states.exec()) { fail(diagnostic, states); return {}; }
    qint64 total = 0, foreground = 0;
    QVariantMap distribution;
    distribution.insert(QStringLiteral("ForegroundFullscreen"), 0LL);
    distribution.insert(QStringLiteral("ForegroundWindowed"), 0LL);
    distribution.insert(QStringLiteral("BackgroundVisible"), 0LL);
    distribution.insert(QStringLiteral("Minimized"), 0LL);
    distribution.insert(QStringLiteral("Unknown"), 0LL);
    while (states.next()) {
        const QString state = states.value(0).toString();
        const qint64 duration = states.value(1).toLongLong();
        distribution.insert(state, duration);
        total += duration;
        if (state == QStringLiteral("ForegroundFullscreen")
            || state == QStringLiteral("ForegroundWindowed")) foreground += duration;
    }
    result.insert(QStringLiteral("totalMs"), total);
    result.insert(QStringLiteral("foregroundMs"), foreground);
    result.insert(QStringLiteral("distribution"), distribution);

    QSqlQuery sessionCount(m_database);
    sessionCount.prepare(QStringLiteral(
        "SELECT COUNT(DISTINCT session_id) FROM usage_segments WHERE application_id=? "
        "AND local_day BETWEEN ? AND ?"));
    sessionCount.addBindValue(applicationId);
    bindDayRange(sessionCount, firstLocalDay, lastLocalDay);
    if (!sessionCount.exec() || !sessionCount.next()) { fail(diagnostic, sessionCount); return {}; }
    result.insert(QStringLiteral("sessionCount"), sessionCount.value(0).toInt());
    QSqlQuery live(m_database);
    live.prepare(QStringLiteral(
        "SELECT session_id, started_utc, duration_ms FROM usage_sessions "
        "WHERE application_id=? AND ended_utc IS NULL ORDER BY started_utc DESC LIMIT 1"));
    live.addBindValue(applicationId);
    if (!live.exec()) { fail(diagnostic, live); return {}; }
    if (live.next()) {
        result.insert(QStringLiteral("running"), true);
        result.insert(QStringLiteral("liveSessionId"), live.value(0));
    } else {
        result.insert(QStringLiteral("running"), false);
    }

    QSqlQuery daily(m_database);
    daily.prepare(QStringLiteral(
        "SELECT local_day, SUM(duration_ms) FROM usage_segments WHERE application_id=? "
        "AND local_day BETWEEN ? AND ? GROUP BY local_day ORDER BY local_day"));
    daily.addBindValue(applicationId);
    bindDayRange(daily, firstLocalDay, lastLocalDay);
    if (!daily.exec()) { fail(diagnostic, daily); return {}; }
    QVariantList dailyRows;
    while (daily.next()) dailyRows.push_back(QVariantMap{{QStringLiteral("day"), daily.value(0)},
                                                        {QStringLiteral("totalMs"), daily.value(1)}});
    result.insert(QStringLiteral("daily"), dailyRows);

    QSqlQuery hourly(m_database);
    hourly.prepare(QStringLiteral(
        "SELECT local_hour, SUM(duration_ms) FROM usage_segments WHERE application_id=? "
        "AND local_day BETWEEN ? AND ? GROUP BY local_hour ORDER BY local_hour"));
    hourly.addBindValue(applicationId);
    bindDayRange(hourly, firstLocalDay, lastLocalDay);
    if (!hourly.exec()) { fail(diagnostic, hourly); return {}; }
    QVariantList hourlyRows;
    while (hourly.next()) hourlyRows.push_back(QVariantMap{{QStringLiteral("hour"), hourly.value(0)},
                                                          {QStringLiteral("totalMs"), hourly.value(1)}});
    result.insert(QStringLiteral("hourly"), hourlyRows);

    QSqlQuery recent(m_database);
    recent.prepare(QStringLiteral(
        "SELECT u.session_id, u.started_utc, COALESCE(u.ended_utc,''), SUM(s.duration_ms), u.end_reason "
        "FROM usage_sessions u JOIN usage_segments s ON s.session_id=u.session_id "
        "WHERE u.application_id=? AND s.local_day BETWEEN ? AND ? "
        "GROUP BY u.session_id ORDER BY u.started_utc DESC LIMIT 100"));
    recent.addBindValue(applicationId);
    bindDayRange(recent, firstLocalDay, lastLocalDay);
    if (!recent.exec()) { fail(diagnostic, recent); return {}; }
    QVariantList sessions;
    while (recent.next()) sessions.push_back(QVariantMap{
        {QStringLiteral("sessionId"), recent.value(0)},
        {QStringLiteral("startedUtc"), recent.value(1)},
        {QStringLiteral("endedUtc"), recent.value(2)},
        {QStringLiteral("durationMs"), recent.value(3)},
        {QStringLiteral("endReason"), recent.value(4)}});
    result.insert(QStringLiteral("sessions"), sessions);
    if (diagnostic) diagnostic->clear();
    return result;
}

bool ApplicationUsageRepository::deleteHistory(const QString &applicationId,
                                                QString *diagnostic)
{
    if (!m_database.transaction()) {
        if (diagnostic) *diagnostic = m_database.lastError().text();
        return false;
    }
    QSqlQuery query(m_database);
    if (applicationId.isEmpty()) {
        if (!query.exec(QStringLiteral("DELETE FROM usage_segments"))
            || !query.exec(QStringLiteral("DELETE FROM usage_sessions"))
            || !query.exec(QStringLiteral("DELETE FROM usage_tracked_applications"))) {
            fail(diagnostic, query); m_database.rollback(); return false;
        }
    } else {
        query.prepare(QStringLiteral("DELETE FROM usage_segments WHERE application_id=?"));
        query.addBindValue(applicationId);
        if (!query.exec()) { fail(diagnostic, query); m_database.rollback(); return false; }
        query.prepare(QStringLiteral("DELETE FROM usage_sessions WHERE application_id=?"));
        query.addBindValue(applicationId);
        if (!query.exec()) { fail(diagnostic, query); m_database.rollback(); return false; }
        query.prepare(QStringLiteral("DELETE FROM usage_tracked_applications WHERE application_id=?"));
        query.addBindValue(applicationId);
        if (!query.exec()) { fail(diagnostic, query); m_database.rollback(); return false; }
    }
    if (!m_database.commit()) {
        if (diagnostic) *diagnostic = m_database.lastError().text();
        m_database.rollback();
        return false;
    }
    return true;
}

QVariantMap ApplicationUsageRepository::storageInformation(QString *diagnostic) const
{
    QVariantMap result;
    for (const auto &table : {QStringLiteral("usage_tracked_applications"),
                              QStringLiteral("usage_sessions"),
                              QStringLiteral("usage_segments"),
                              QStringLiteral("usage_excluded_applications")}) {
        QSqlQuery query(m_database);
        query.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table);
        if (!query.next()) { fail(diagnostic, query); return {}; }
        result.insert(table, query.value(0).toLongLong());
    }
    if (diagnostic) diagnostic->clear();
    return result;
}

} // namespace something::storage

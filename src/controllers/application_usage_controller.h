#pragma once

#include "../services/application_usage_service.h"

#include <QObject>
#include <QHash>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace something::controllers {

class ApplicationUsageController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool trackingEnabled READ trackingEnabled NOTIFY stateChanged)
    Q_PROPERTY(bool trackingActive READ trackingActive NOTIFY stateChanged)
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(int retentionDays READ retentionDays NOTIFY stateChanged)
    Q_PROPERTY(QString trackingStatus READ trackingStatus NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QVariantList exclusions READ exclusions NOTIFY stateChanged)
    Q_PROPERTY(QVariantList applications READ applications NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap overview READ overview NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap detail READ detail NOTIFY stateChanged)
    Q_PROPERTY(QString selectedApplicationId READ selectedApplicationId NOTIFY stateChanged)
    Q_PROPERTY(QString rangePreset READ rangePreset NOTIFY stateChanged)
    Q_PROPERTY(QString customFirstDay READ customFirstDay NOTIFY stateChanged)
    Q_PROPERTY(QString customLastDay READ customLastDay NOTIFY stateChanged)
    Q_PROPERTY(QString searchText READ searchText NOTIFY stateChanged)
    Q_PROPERTY(QString sortKey READ sortKey NOTIFY stateChanged)

public:
    ApplicationUsageController(something::application::ApplicationServiceGraph *graph,
                               something::services::ApplicationUsageService *service,
                               QObject *parent = nullptr);

    bool ready() const noexcept;
    bool loading() const noexcept { return m_loading; }
    bool trackingEnabled() const noexcept;
    bool trackingActive() const noexcept;
    bool supported() const noexcept;
    int retentionDays() const noexcept;
    QString trackingStatus() const;
    QString errorMessage() const { return m_errorMessage; }
    QVariantList exclusions() const;
    QVariantList applications() const;
    QVariantMap overview() const { return m_overview; }
    QVariantMap detail() const { return m_detail; }
    QString selectedApplicationId() const { return m_selectedApplicationId; }
    QString rangePreset() const { return m_rangePreset; }
    QString customFirstDay() const { return m_customFirstDay; }
    QString customLastDay() const { return m_customLastDay; }
    QString searchText() const { return m_searchText; }
    QString sortKey() const { return m_sortKey; }

    Q_INVOKABLE void setPageVisible(bool visible);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void loadApplicationChoices();
    Q_INVOKABLE void setRangePreset(const QString &preset);
    Q_INVOKABLE void setCustomRange(const QString &firstDay, const QString &lastDay);
    Q_INVOKABLE void setSearchText(const QString &text);
    Q_INVOKABLE void setSortKey(const QString &sortKey);
    Q_INVOKABLE void selectApplication(const QString &applicationId);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void setTrackingEnabled(bool enabled);
    Q_INVOKABLE void setRetentionDays(int days);
    Q_INVOKABLE void addApplicationExclusion(const QString &applicationId,
                                              const QString &canonicalPath,
                                              const QString &executableName);
    Q_INVOKABLE void addFilenameExclusion(const QString &executableName);
    Q_INVOKABLE void removeExclusion(const QString &applicationId);
    Q_INVOKABLE void deleteHistory(const QString &applicationId = {});
    Q_INVOKABLE QString formatDuration(qint64 durationMs) const;
    Q_INVOKABLE QString formatTimestamp(const QString &timestampUtc) const;

signals:
    void stateChanged();

private:
    struct Request final {
        enum class Kind { Overview, Detail, ApplicationChoices } kind;
        quint64 generation{0};
    };

    void requestOverview(quint64 generation);
    void requestDetail(quint64 generation);
    void onStorageFinished(quint64 operationId, QVariantMap values,
                           something::domain::AppError error);
    void applyLiveStates();
    void updateDateRange();
    void updateLoading();

    something::application::ApplicationServiceGraph *m_graph{nullptr};
    something::services::ApplicationUsageService *m_service{nullptr};
    QTimer m_searchTimer;
    QTimer m_refreshTimer;
    QHash<quint64, Request> m_requests;
    QVariantMap m_overview;
    QVariantMap m_detail;
    QString m_selectedApplicationId;
    QString m_rangePreset{QStringLiteral("today")};
    QString m_searchText;
    QString m_sortKey{QStringLiteral("total")};
    QString m_customFirstDay;
    QString m_customLastDay;
    QString m_firstLocalDay;
    QString m_lastLocalDay;
    QString m_startUtc;
    QString m_endUtc;
    QString m_errorMessage;
    quint64 m_generation{0};
    bool m_pageVisible{false};
    bool m_loading{false};
};

} // namespace something::controllers

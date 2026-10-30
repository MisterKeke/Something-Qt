#pragma once

#include "../application/application_service_graph.h"
#include "../domain/clock.h"
#include "../models/scheduling_preview_model.h"

#include <QObject>
#include <QDateTime>
#include <QHash>
#include <QStringList>
#include <QTimeZone>
#include <QVariantList>

namespace something::controllers {

class SchedulingController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::SchedulingPreviewModel *recommendations
               READ recommendations CONSTANT)
    Q_PROPERTY(QVariantList daySummaries READ daySummaries NOTIFY stateChanged)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool previewVisible READ previewVisible NOTIFY stateChanged)
    Q_PROPERTY(bool previewAvailable READ previewAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool previewStale READ previewStale NOTIFY stateChanged)
    Q_PROPERTY(bool applying READ applying NOTIFY stateChanged)
    Q_PROPERTY(bool canStage READ canStage NOTIFY stateChanged)
    Q_PROPERTY(bool canApply READ canApply NOTIFY stateChanged)
    Q_PROPERTY(int acceptedCount READ acceptedCount NOTIFY stateChanged)
    Q_PROPERTY(int validCount READ validCount NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(QString todayDate READ todayDate NOTIFY stateChanged)

public:
    explicit SchedulingController(application::ApplicationServiceGraph *graph,
                                  const domain::Clock *clock,
                                  QObject *parent = nullptr);

    models::SchedulingPreviewModel *recommendations() noexcept { return &m_recommendations; }
    QVariantList daySummaries() const { return m_daySummaries; }
    QStringList warnings() const { return m_warnings; }
    bool loading() const noexcept { return m_loading; }
    bool previewVisible() const noexcept { return m_previewVisible; }
    bool previewAvailable() const noexcept { return m_previewAvailable; }
    bool previewStale() const noexcept { return m_previewStale; }
    bool applying() const noexcept { return m_applying; }
    bool canStage() const noexcept
    { return m_previewAvailable && !m_loading && !m_applying && !m_previewStale; }
    bool canApply() const noexcept { return canStage() && acceptedCount() > 0; }
    int acceptedCount() const noexcept { return m_recommendations.acceptedCount(); }
    int validCount() const noexcept { return m_recommendations.validCount(); }
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }
    QString todayDate() const { return m_todayDate; }

    Q_INVOKABLE void generatePreview();
    Q_INVOKABLE void setRecommendationAccepted(qint64 taskId, bool accepted);
    Q_INVOKABLE void acceptAllValid();
    Q_INVOKABLE void applyAccepted();
    Q_INVOKABLE void rejectProposal();

public slots:
    void onTaskMutationCommitted(qint64 taskId, const QVariantMap &currentTask, bool deleted);
    void onTaskMutationMayHaveCommitted();

signals:
    void stateChanged();
    void schedulesSaved();

private:
    enum class Action { LoadSnapshot, Calculate, ApplySchedules };

    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void onCalculationFinished(quint64 operationId, const QVariantMap &values,
                               domain::AppError error);
    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    bool submitCalculation(const QVariantMap &snapshot);
    void finishApply(const QVariantMap &values, domain::AppError error);
    void cancelCurrentOperation();
    void invalidatePreview();
    void finishWithError(domain::AppError error, const QString &fallback);

    application::ApplicationServiceGraph *m_graph{nullptr};
    const domain::Clock *m_clock{nullptr};
    models::SchedulingPreviewModel m_recommendations;
    QHash<quint64, Action> m_pending;
    QVariantList m_daySummaries;
    QStringList m_warnings;
    quint64 m_currentOperationId{0};
    QDateTime m_previewNowUtc;
    QTimeZone m_previewTimeZone;
    qint64 m_previewPreferencesRevision{0};
    QString m_todayDate;
    QString m_errorMessage;
    QString m_statusMessage;
    bool m_loading{false};
    bool m_applying{false};
    bool m_previewVisible{false};
    bool m_previewAvailable{false};
    bool m_previewStale{false};
};

} // namespace something::controllers

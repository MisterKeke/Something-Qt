#pragma once

#include "../application/application_service_graph.h"
#include "../models/running_windows_model.h"
#include "../models/universal_search_model.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QVariantList>
#include <QVariantMap>

namespace something::controllers {

class UniversalSearchController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::UniversalSearchModel *results READ results CONSTANT)
    Q_PROPERTY(QString query READ query NOTIFY queryChanged)
    Q_PROPERTY(QString categoryFilter READ categoryFilter NOTIFY categoryFilterChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)

public:
    explicit UniversalSearchController(application::ApplicationServiceGraph *graph,
                                       QObject *parent = nullptr);
    models::UniversalSearchModel *results() noexcept { return &m_results; }
    QString query() const { return m_query; }
    QString categoryFilter() const { return m_categoryFilter; }
    bool searching() const noexcept { return m_searching; }
    QString message() const { return m_message; }

    void setRunningWindowsModel(models::RunningWindowsModel *model);
    Q_INVOKABLE void setQuery(QString query);
    Q_INVOKABLE void setCategoryFilter(QString category);
    Q_INVOKABLE void reset();
    Q_INVOKABLE void activateResult(int row, QString action = {});

signals:
    void queryChanged();
    void categoryFilterChanged();
    void stateChanged();
    void resultActivated(const QVariantMap &result, const QString &action);

private:
    void beginSearch();
    void onStorageFinished(quint64 operationId, const QVariantMap &values,
                           domain::AppError error);
    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    QVector<QVariantMap> mergedResults(const QVariantList &stored) const;
    void setMessage(QString message);
    void cancelActive();

    application::ApplicationServiceGraph *m_graph{nullptr};
    models::RunningWindowsModel *m_runningWindows{nullptr};
    models::UniversalSearchModel m_results;
    QTimer m_debounce;
    QVariantList m_storedResults;
    QString m_query;
    QString m_categoryFilter{QStringLiteral("all")};
    QString m_message;
    quint64 m_activeOperation{0};
    quint64 m_generation{0};
    quint64 m_activeGeneration{0};
    bool m_searching{false};
};

} // namespace something::controllers

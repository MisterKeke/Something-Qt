#include "universal_search_controller.h"

#include "../application/route_catalog.h"
#include "../domain/universal_search.h"
#include "../services/universal_search_service.h"

#include <algorithm>
#include <utility>

namespace something::controllers {
namespace {

QVariantMap action(QString id, QString label)
{
    return {{QStringLiteral("id"), std::move(id)}, {QStringLiteral("label"), std::move(label)}};
}

QString providerTitle(const QVariantMap &route)
{
    const QString navigationTitle = route.value(QStringLiteral("navigationTitle")).toString();
    return navigationTitle.isEmpty() ? route.value(QStringLiteral("title")).toString()
                                     : navigationTitle;
}

} // namespace

UniversalSearchController::UniversalSearchController(
    application::ApplicationServiceGraph *graph, QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_results(this)
{
    Q_ASSERT(m_graph);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(160);
    connect(&m_debounce, &QTimer::timeout, this, &UniversalSearchController::beginSearch);
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &UniversalSearchController::onStorageFinished);
    connect(&m_graph->lifecycle(), &application::ApplicationLifecycle::stateChanged,
            this, &UniversalSearchController::onLifecycleChanged);
    onLifecycleChanged(m_graph->lifecycle().state());
}

void UniversalSearchController::setRunningWindowsModel(models::RunningWindowsModel *model)
{
    m_runningWindows = model;
    if (!m_runningWindows) return;
    const auto updateSnapshot = [this] {
        if (!m_query.trimmed().isEmpty() && !m_searching)
            m_results.replace(mergedResults(m_storedResults));
    };
    connect(m_runningWindows, &QAbstractItemModel::dataChanged, this,
            [updateSnapshot](const QModelIndex &, const QModelIndex &, const QList<int> &) {
        updateSnapshot();
    });
    connect(m_runningWindows, &QAbstractItemModel::rowsInserted, this,
            [updateSnapshot](const QModelIndex &, int, int) { updateSnapshot(); });
    connect(m_runningWindows, &QAbstractItemModel::rowsRemoved, this,
            [updateSnapshot](const QModelIndex &, int, int) { updateSnapshot(); });
    connect(m_runningWindows, &QAbstractItemModel::modelReset, this, updateSnapshot);
}

void UniversalSearchController::setQuery(QString query)
{
    if (query.size() > domain::UniversalSearch::maximumQueryLength)
        query.truncate(domain::UniversalSearch::maximumQueryLength);
    if (m_query == query) return;
    m_query = std::move(query);
    ++m_generation;
    m_debounce.stop();
    cancelActive();
    m_results.clear();
    m_storedResults.clear();
    m_message.clear();
    m_searching = false;
    emit queryChanged();
    emit stateChanged();
    if (!domain::UniversalSearch::terms(m_query).isEmpty()) {
        m_searching = true;
        emit stateChanged();
        m_debounce.start();
    }
}

void UniversalSearchController::setCategoryFilter(QString category)
{
    static const QStringList supported{QStringLiteral("all"), QStringLiteral("navigation"),
        QStringLiteral("tasks"), QStringLiteral("applications"), QStringLiteral("running"),
        QStringLiteral("games"), QStringLiteral("favorites"), QStringLiteral("setups")};
    if (!supported.contains(category)) category = QStringLiteral("all");
    if (m_categoryFilter == category) return;
    m_categoryFilter = std::move(category);
    emit categoryFilterChanged();
    m_results.replace(mergedResults(m_storedResults));
}

void UniversalSearchController::reset()
{
    ++m_generation;
    m_debounce.stop();
    cancelActive();
    m_results.clear();
    m_storedResults.clear();
    m_query.clear();
    m_message.clear();
    m_searching = false;
    emit queryChanged();
    emit stateChanged();
}

void UniversalSearchController::activateResult(int row, QString requestedAction)
{
    const QVariantMap result = m_results.rowAt(row);
    if (result.isEmpty()) return;
    const QVariantList available = result.value(QStringLiteral("actions")).toList();
    QString selected = std::move(requestedAction);
    if (selected.isEmpty() && !available.isEmpty())
        selected = available.first().toMap().value(QStringLiteral("id")).toString();
    bool allowed = false;
    for (const QVariant &item : available) {
        if (item.toMap().value(QStringLiteral("id")).toString() == selected) {
            allowed = true;
            break;
        }
    }
    if (allowed) emit resultActivated(result, selected);
}

void UniversalSearchController::beginSearch()
{
    const QStringList terms = domain::UniversalSearch::terms(m_query);
    if (terms.isEmpty()) {
        m_searching = false;
        emit stateChanged();
        return;
    }
    if (terms.size() > domain::UniversalSearch::maximumTerms) {
        m_searching = false;
        setMessage(QStringLiteral("Use eight words or fewer."));
        return;
    }
    const auto operation = m_graph->beginOperation(QStringLiteral("universal search"));
    if (!operation.isValid()) {
        m_searching = false;
        setMessage(QStringLiteral("Search is not ready yet."));
        m_results.replace(mergedResults({}));
        return;
    }
    m_activeOperation = operation.id.value;
    m_activeGeneration = m_generation;
    if (!m_graph->submitStorage(operation, services::UniversalSearchService::search(m_query))) {
        m_graph->completeOperation(operation.id,
            domain::AppError(domain::ErrorCategory::CapabilityUnavailable));
        m_activeOperation = 0;
        m_searching = false;
        setMessage(QStringLiteral("Search is temporarily unavailable."));
        m_results.replace(mergedResults({}));
    }
}

void UniversalSearchController::onStorageFinished(
    quint64 operationId, const QVariantMap &values, domain::AppError error)
{
    if (operationId != m_activeOperation) return;
    m_activeOperation = 0;
    m_searching = false;
    if (m_activeGeneration != m_generation) return;
    if (error.isError()) {
        if (!error.isCancelled()) {
            setMessage(QStringLiteral("Search is temporarily unavailable."));
            m_results.replace(mergedResults({}));
        } else {
            emit stateChanged();
        }
        return;
    }
    m_message.clear();
    m_storedResults = values.value(QStringLiteral("results")).toList();
    m_results.replace(mergedResults(m_storedResults));
    emit stateChanged();
}

void UniversalSearchController::onLifecycleChanged(application::ApplicationLifecycle::State state)
{
    if (state == application::ApplicationLifecycle::State::ShuttingDown
        || state == application::ApplicationLifecycle::State::Stopped
        || state == application::ApplicationLifecycle::State::Failed) {
        ++m_generation;
        m_debounce.stop();
        cancelActive();
        m_searching = false;
        emit stateChanged();
    }
}

QVector<QVariantMap> UniversalSearchController::mergedResults(const QVariantList &stored) const
{
    QVector<QVariantMap> results;
    const QString query = m_query.trimmed();
    if (query.isEmpty()) return results;
    results.reserve(stored.size() + 20);
    for (const QVariant &value : stored) {
        QVariantMap result = value.toMap();
        if (result.isEmpty()) continue;
        if (!domain::UniversalSearch::acceptsGroup(
                result.value(QStringLiteral("group")).toString(), m_categoryFilter)) continue;
        results.push_back(std::move(result));
    }

    for (const QVariant &routeValue : application::RouteCatalog::routes()) {
        const QVariantMap route = routeValue.toMap();
        const QString id = route.value(QStringLiteral("id")).toString();
        const QString title = route.value(QStringLiteral("title")).toString();
        QStringList routeTerms{title, route.value(QStringLiteral("navigationTitle")).toString(), id};
        const QVariantList aliases = route.value(QStringLiteral("searchAliases")).toList();
        for (const QVariant &alias : aliases) routeTerms.push_back(alias.toString());
        const int score = domain::UniversalSearch::score(query, routeTerms);
        if (score <= 0 || !domain::UniversalSearch::acceptsGroup(
                QStringLiteral("navigation"), m_categoryFilter)) continue;
        results.push_back({{QStringLiteral("id"), QStringLiteral("route:") + id},
            {QStringLiteral("type"), QStringLiteral("route")},
            {QStringLiteral("group"), QStringLiteral("navigation")},
            {QStringLiteral("title"), providerTitle(route)},
            {QStringLiteral("subtitle"), QStringLiteral("Application section")},
            {QStringLiteral("source"), QStringLiteral("Navigation")},
            {QStringLiteral("routeId"), id}, {QStringLiteral("targetId"), id},
            {QStringLiteral("secondaryId"), QVariant{}},
            {QStringLiteral("icon"), route.value(QStringLiteral("glyph"))},
            {QStringLiteral("actions"), QVariantList{action(QStringLiteral("navigate"),
                                                             QStringLiteral("Open section"))}},
            {QStringLiteral("payload"), route}, {QStringLiteral("score"), score}});
    }

    if (m_runningWindows && domain::UniversalSearch::acceptsGroup(
            QStringLiteral("running"), m_categoryFilter)) {
        for (int row = 0; row < m_runningWindows->rowCount(); ++row) {
            const QModelIndex index = m_runningWindows->index(row, 0);
            const QString windowId = m_runningWindows->data(index,
                models::RunningWindowsModel::WindowIdRole).toString();
            const QString process = m_runningWindows->data(index,
                models::RunningWindowsModel::ProcessNameRole).toString();
            const QString windowTitle = m_runningWindows->data(index,
                models::RunningWindowsModel::TitleRole).toString();
            const QString iconDataUrl = m_runningWindows->data(index,
                models::RunningWindowsModel::IconDataUrlRole).toString();
            const QString displayTitle = process.isEmpty() ? windowTitle : process;
            const int score = domain::UniversalSearch::score(query, {displayTitle, windowTitle});
            if (score <= 0) continue;
            results.push_back({{QStringLiteral("id"), QStringLiteral("runningWindow:") + windowId},
                {QStringLiteral("type"), QStringLiteral("runningApplication")},
                {QStringLiteral("group"), QStringLiteral("running")},
                {QStringLiteral("title"), displayTitle},
                {QStringLiteral("subtitle"), windowTitle.isEmpty()
                    ? QStringLiteral("Running application") : windowTitle},
                {QStringLiteral("source"), QStringLiteral("Running applications")},
                {QStringLiteral("routeId"), QStringLiteral("runningApplications")},
                {QStringLiteral("targetId"), windowId}, {QStringLiteral("secondaryId"), QVariant{}},
                {QStringLiteral("icon"), iconDataUrl.isEmpty() ? QStringLiteral("↗") : iconDataUrl},
                {QStringLiteral("actions"), QVariantList{
                    action(QStringLiteral("activate"), QStringLiteral("Activate window")),
                    action(QStringLiteral("open"), QStringLiteral("Open running apps"))}},
                {QStringLiteral("payload"), QVariantMap{{QStringLiteral("windowId"), windowId},
                    {QStringLiteral("processName"), process}, {QStringLiteral("title"), windowTitle}}},
                {QStringLiteral("score"), score}});
        }
    }

    std::sort(results.begin(), results.end(), domain::UniversalSearch::compare);
    if (results.size() > domain::UniversalSearch::maximumResults)
        results.resize(domain::UniversalSearch::maximumResults);
    return results;
}

void UniversalSearchController::setMessage(QString message)
{
    m_message = std::move(message);
    emit stateChanged();
}

void UniversalSearchController::cancelActive()
{
    if (!m_activeOperation) return;
    const application::OperationId operation{m_activeOperation};
    m_activeOperation = 0;
    m_graph->cancelOperation(operation);
}

} // namespace something::controllers

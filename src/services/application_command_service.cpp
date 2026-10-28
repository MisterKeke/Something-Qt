#include "application_command_service.h"

#include "task_service.h"
#include "../application/application_lifecycle.h"
#include "../application/operation_coordinator.h"

#include <QSet>
#include <QUuid>
#include <limits>
#include <utility>

namespace something::services {
namespace {

QString requireNoArguments(const QStringList &arguments)
{
    return arguments.isEmpty() ? QString() : QStringLiteral("This command takes no arguments.");
}

QString positiveInteger(const QString &value, qint64 maximum, qint64 *result)
{
    bool ok = false;
    const qint64 parsed = value.toLongLong(&ok, 10);
    if (!ok || parsed < 1 || parsed > maximum)
        return QStringLiteral("Expected a positive decimal integer.");
    *result = parsed;
    return {};
}

} // namespace

ApplicationCommandService::ApplicationCommandService(
    application::ApplicationServiceGraph *graph, QObject *parent)
    : QObject(parent), m_graph(graph)
{
    Q_ASSERT(m_graph);
    qRegisterMetaType<ConsoleRequestResult>();
    connect(m_graph, &application::ApplicationServiceGraph::storageOperationFinished,
            this, &ApplicationCommandService::onStorageFinished);
    connect(&m_graph->operations(), &application::OperationCoordinator::operationFinished,
            this, &ApplicationCommandService::onOperationFinished);
    registerBuiltins();
}

void ApplicationCommandService::registerBuiltins()
{
    using application::CommandDefinition;
    auto registerOne = [this](CommandDefinition command) {
        QString error;
        const bool registered = m_registry.registerCommand(std::move(command), &error);
        Q_ASSERT_X(registered, "ApplicationCommandService::registerBuiltins", qPrintable(error));
        Q_UNUSED(registered);
    };

    registerOne({QStringLiteral("help"), QStringLiteral("List commands or show command help."),
        QStringLiteral("help [command]"),
        [this](const QStringList &args) {
            if (args.size() > 1) return QStringLiteral("Usage: help [command]");
            if (!args.isEmpty() && !m_registry.find(args.constFirst()))
                return QStringLiteral("No such registered command.");
            return QString();
        },
        [this](const QStringList &args) {
            application::CommandExecution execution;
            if (args.isEmpty()) {
                execution.data.insert(QStringLiteral("commands"), m_registry.metadata());
                execution.data.insert(QStringLiteral("escapeSyntax"),
                    QStringLiteral("Backslash escapes a quote, backslash, or whitespace. Quotes group one argument; empty quotes make an empty argument."));
            } else {
                const auto *definition = m_registry.find(args.constFirst());
                execution.data.insert(QStringLiteral("command"), definition->name);
                execution.data.insert(QStringLiteral("description"), definition->description);
                execution.data.insert(QStringLiteral("usage"), definition->usage);
            }
            return execution;
        }});

    registerOne({QStringLiteral("status"), QStringLiteral("Show application readiness and capabilities."),
        QStringLiteral("status"), requireNoArguments,
        [this](const QStringList &) {
            application::CommandExecution execution;
            QVariantMap status{
                {QStringLiteral("lifecycle"), m_graph->lifecycle().stateName()},
                {QStringLiteral("maintenance"), m_graph->isProfileMaintenance()},
                {QStringLiteral("pendingConsoleRequests"), m_pending.size()},
                {QStringLiteral("unavailableCapabilities"), m_graph->capabilities().unavailableCapabilities()},
            };
            execution.data.insert(QStringLiteral("status"), status);
            return execution;
        }});

    registerOne({QStringLiteral("task"), QStringLiteral("Read saved tasks."),
        QStringLiteral("task list [--query \"text\"] [--page n] | task show <id>"),
        [](const QStringList &args) {
            if (args.isEmpty()) return QStringLiteral("Usage: task list [--query \"text\"] [--page n] | task show <id>");
            if (args.constFirst() == QStringLiteral("show")) {
                qint64 id = 0;
                if (args.size() != 2) return QStringLiteral("Usage: task show <id>");
                return positiveInteger(args.at(1), std::numeric_limits<qint64>::max(), &id);
            }
            if (args.constFirst() != QStringLiteral("list"))
                return QStringLiteral("Expected 'list' or 'show'.");
            QSet<QString> seen;
            for (qsizetype i = 1; i < args.size(); ++i) {
                const QString option = args.at(i);
                if (option != QStringLiteral("--query") && option != QStringLiteral("--page"))
                    return QStringLiteral("Unknown task list option.");
                if (seen.contains(option))
                    return QStringLiteral("A task list option may be used only once.");
                if (option == QStringLiteral("--query")) {
                    if (i + 1 >= args.size()) return QStringLiteral("--query requires text.");
                    seen.insert(option);
                    ++i;
                } else {
                    if (i + 1 >= args.size()) return QStringLiteral("--page requires a page number.");
                    qint64 page = 0;
                    const QString error = positiveInteger(args.at(i + 1), 1000000, &page);
                    if (!error.isEmpty()) return QStringLiteral("--page requires a page number from 1 to 1000000.");
                    seen.insert(option);
                    ++i;
                }
            }
            return QString();
        },
        [](const QStringList &args) {
            application::CommandExecution execution;
            if (args.constFirst() == QStringLiteral("show")) {
                const qint64 id = args.at(1).toLongLong();
                execution.storageCommand = TaskService::get(id);
                execution.data.insert(QStringLiteral("resultShape"), QStringLiteral("task-show"));
                return execution;
            }
            storage::TaskFilter filter;
            int page = 1;
            for (qsizetype i = 1; i < args.size(); ++i) {
                if (args.at(i) == QStringLiteral("--query")) filter.query = args.at(++i);
                else if (args.at(i) == QStringLiteral("--page")) page = args.at(++i).toInt();
            }
            constexpr int pageSize = 50;
            filter.offset = (page - 1) * pageSize;
            execution.storageCommand = TaskService::list(filter, pageSize);
            execution.data.insert(QStringLiteral("resultShape"), QStringLiteral("task-list"));
            execution.data.insert(QStringLiteral("page"), page);
            execution.data.insert(QStringLiteral("pageSize"), pageSize);
            return execution;
        }});
}

ConsoleRequestResult ApplicationCommandService::makeResult(QString id, QString state,
                                                             QString command) const
{
    ConsoleRequestResult result;
    result.requestId = id;
    result.state = std::move(state);
    result.command = std::move(command);
    return result;
}

QString ApplicationCommandService::submit(const QString &input)
{
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto reject = [this, requestId](QString command, QString message) {
        auto result = makeResult(requestId, QStringLiteral("rejected"), std::move(command));
        result.message = std::move(message);
        emit requestRejected(result);
        return requestId;
    };
    const auto parsed = application::CommandParser::parse(input);
    if (!parsed.succeeded()) return reject({}, parsed.error);
    if (parsed.tokens.isEmpty()) return reject({}, QStringLiteral("Enter a command."));
    const QString commandName = parsed.tokens.constFirst();
    const auto *definition = m_registry.find(commandName);
    if (!definition) return reject(commandName, QStringLiteral("Unknown command. Use 'help' to list registered commands."));
    const QStringList arguments = parsed.tokens.mid(1);
    const QString validationError = m_registry.validate(*definition, arguments);
    if (!validationError.isEmpty()) return reject(commandName, validationError);

    const bool taskRead = commandName == QStringLiteral("task");
    if (taskRead) {
        const QString unavailable = availabilityError();
        if (!unavailable.isEmpty()) return reject(commandName, unavailable);
    } else if (m_graph->lifecycle().state() == application::ApplicationLifecycle::State::ShuttingDown
               || m_graph->lifecycle().state() == application::ApplicationLifecycle::State::Stopped) {
        return reject(commandName, QStringLiteral("The application is shutting down."));
    }

    const application::CommandExecution execution = definition->handler(arguments);
    ConsoleRequestResult accepted = makeResult(requestId,
        execution.storageCommand ? QStringLiteral("pending") : QStringLiteral("succeeded"), commandName);
    accepted.accepted = true;
    accepted.data = execution.data;
    if (!execution.storageCommand) {
        emit requestAccepted(accepted);
        emit requestFinished(accepted);
        return requestId;
    }
    if (m_pending.size() >= MaximumPendingRequests)
        return reject(commandName, QStringLiteral("Console is busy; try again when a request finishes."));

    const auto operation = m_graph->beginOperation(QStringLiteral("console: ") + commandName);
    if (!operation.isValid()) return reject(commandName, availabilityError());
    PendingRequest pending;
    pending.result = accepted;
    pending.operationId = operation.id.value;
    m_pending.insert(requestId, pending);
    m_requestByOperation.insert(operation.id.value, requestId);
    emit requestAccepted(accepted);
    emit requestPending(accepted);
    m_graph->submitStorage(operation, execution.storageCommand);
    return requestId;
}

QString ApplicationCommandService::availabilityError() const
{
    using State = application::ApplicationLifecycle::State;
    const State state = m_graph->lifecycle().state();
    if (state == State::Starting) return QStringLiteral("Application startup is still in progress.");
    if (state == State::Failed) return QStringLiteral("Application startup failed; task storage is unavailable.");
    if (state == State::ShuttingDown || state == State::Stopped)
        return QStringLiteral("The application is shutting down.");
    if (m_graph->isProfileMaintenance()) return QStringLiteral("Task commands are unavailable during profile maintenance.");
    if (!m_graph->capabilities().isAvailable(QStringLiteral("local-storage")))
        return QStringLiteral("Local task storage is unavailable.");
    return {};
}

bool ApplicationCommandService::cancel(const QString &requestId)
{
    const auto it = m_pending.constFind(requestId);
    if (it == m_pending.cend() || it->finalEmitted) return false;
    return m_graph->cancelOperation({it->operationId});
}

void ApplicationCommandService::finish(const QString &id, QString state, QString message,
                                       QVariantMap data)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end() || it->finalEmitted) return;
    it->finalEmitted = true;
    ConsoleRequestResult result = it->result;
    result.state = std::move(state);
    result.message = std::move(message);
    result.data = std::move(data);
    m_requestByOperation.remove(it->operationId);
    m_pending.erase(it);
    if (result.state == QStringLiteral("cancelled")) emit requestCancelled(result);
    else emit requestFinished(result);
}

void ApplicationCommandService::onStorageFinished(quint64 operationId,
                                                   const QVariantMap &values,
                                                   domain::AppError error)
{
    const auto requestIt = m_requestByOperation.constFind(operationId);
    if (requestIt == m_requestByOperation.cend()) return;
    const QString requestId = requestIt.value();
    const auto pending = m_pending.constFind(requestId);
    if (pending == m_pending.cend()) return;
    if (error.isCancelled()) {
        finish(requestId, QStringLiteral("cancelled"), QStringLiteral("Request cancelled."));
        return;
    }
    if (error.isError()) {
        finish(requestId, QStringLiteral("failed"), error.userMessage());
        return;
    }
    QVariantMap data = pending->result.data;
    const QString resultShape = data.take(QStringLiteral("resultShape")).toString();
    if (resultShape == QStringLiteral("task-show")) {
        data.insert(QStringLiteral("task"), values.value(QStringLiteral("task")));
    } else if (resultShape == QStringLiteral("task-list")) {
        data.insert(QStringLiteral("tasks"), values.value(QStringLiteral("tasks")));
        data.insert(QStringLiteral("hasMore"), values.value(QStringLiteral("hasMore")));
    }
    finish(requestId, QStringLiteral("succeeded"), {}, std::move(data));
}

void ApplicationCommandService::onOperationFinished(
    quint64 operationId, application::OperationCoordinator::State state,
    domain::AppError error)
{
    Q_UNUSED(error);
    if (state != application::OperationCoordinator::State::Cancelled) return;
    const auto requestIt = m_requestByOperation.constFind(operationId);
    if (requestIt != m_requestByOperation.cend())
        finish(requestIt.value(), QStringLiteral("cancelled"), QStringLiteral("Request cancelled."));
}

} // namespace something::services

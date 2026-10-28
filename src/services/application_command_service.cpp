#include "application_command_service.h"

#include "task_service.h"
#include "wallpaper_service.h"
#include "favorites_service.h"
#include "desktop_applications_service.h"
#include "setups_service.h"
#include "../application/route_catalog.h"
#include "../domain/clock.h"
#include "../application/application_lifecycle.h"
#include "../application/operation_coordinator.h"

#include <QSet>
#include <QDate>
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

QString listPage(const QStringList &args, qsizetype firstOption, int *page)
{
    *page = 1;
    if (firstOption == args.size()) return {};
    if (firstOption + 2 != args.size() || args.at(firstOption) != QStringLiteral("--page"))
        return QStringLiteral("Expected optional --page <n>.");
    qint64 parsed = 0;
    if (positiveInteger(args.at(firstOption + 1), 1000000, &parsed).isEmpty()) {
        *page = static_cast<int>(parsed);
        return {};
    }
    return QStringLiteral("--page requires a number from 1 to 1000000.");
}

QStringList pageArguments(const QStringList &args)
{
    return args.mid(1);
}

application::CommandExecution pagedStorage(storage::StorageCommand command,
                                           QString resultShape, int page)
{
    application::CommandExecution execution;
    execution.storageCommand = std::move(command);
    execution.data.insert(QStringLiteral("resultShape"), std::move(resultShape));
    execution.data.insert(QStringLiteral("page"), page);
    execution.data.insert(QStringLiteral("pageSize"), 50);
    return execution;
}

QString errorCodeFor(domain::ErrorCategory category)
{
    using domain::ErrorCategory;
    switch (category) {
    case ErrorCategory::Validation: return QStringLiteral("validation");
    case ErrorCategory::NotFound: return QStringLiteral("not-found");
    case ErrorCategory::Conflict: return QStringLiteral("conflict");
    case ErrorCategory::Cancelled: return QStringLiteral("cancelled");
    case ErrorCategory::Busy: return QStringLiteral("busy");
    case ErrorCategory::CapabilityUnavailable: return QStringLiteral("unavailable");
    case ErrorCategory::Startup: return QStringLiteral("starting");
    case ErrorCategory::Storage: return QStringLiteral("storage");
    case ErrorCategory::None: return {};
    default: return QStringLiteral("operation-failed");
    }
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
                {QStringLiteral("pendingConsoleRequests"), m_pending.size() + m_mutationRequests.size()},
                {QStringLiteral("unavailableCapabilities"), m_graph->capabilities().unavailableCapabilities()},
            };
            execution.data.insert(QStringLiteral("status"), status);
            return execution;
        }});

    registerOne({QStringLiteral("task"), QStringLiteral("Read, add, or complete saved tasks."),
        QStringLiteral("task list [--query \"text\"] [--page n] | task show <id> | task add \"title\" [--due YYYY-MM-DD] [--priority low|medium|high] | task complete <id> --revision <revision>"),
        [](const QStringList &args) {
            if (args.isEmpty()) return QStringLiteral("Usage: task list [--query \"text\"] [--page n] | task show <id> | task add \"title\" [--due YYYY-MM-DD] [--priority low|medium|high] | task complete <id> --revision <revision>");
            if (args.constFirst() == QStringLiteral("add")) {
                if (args.size() < 2) return QStringLiteral("Usage: task add \"title\" [--due YYYY-MM-DD] [--priority low|medium|high]");
                QSet<QString> seen;
                for (qsizetype i = 2; i < args.size(); ++i) {
                    const QString option = args.at(i);
                    if ((option != QStringLiteral("--due") && option != QStringLiteral("--priority"))
                        || seen.contains(option) || i + 1 >= args.size())
                        return QStringLiteral("Expected one optional --due <date> and --priority <low|medium|high>.");
                    const QString value = args.at(++i);
                    if (option == QStringLiteral("--due")) {
                        const QDate date = QDate::fromString(value, Qt::ISODate);
                        if (value.isEmpty() || !date.isValid() || date.toString(Qt::ISODate) != value)
                            return QStringLiteral("--due must be a valid YYYY-MM-DD date.");
                    } else if (value != QStringLiteral("low") && value != QStringLiteral("medium")
                               && value != QStringLiteral("high")) {
                        return QStringLiteral("--priority must be low, medium, or high.");
                    }
                    seen.insert(option);
                }
                return QString();
            }
            if (args.constFirst() == QStringLiteral("complete")) {
                qint64 id = 0, revision = 0;
                if (args.size() != 4 || args.at(2) != QStringLiteral("--revision"))
                    return QStringLiteral("Usage: task complete <id> --revision <revision>");
                const QString idError = positiveInteger(args.at(1), std::numeric_limits<qint64>::max(), &id);
                if (!idError.isEmpty()) return idError;
                return positiveInteger(args.at(3), std::numeric_limits<qint64>::max(), &revision);
            }
            if (args.constFirst() == QStringLiteral("show")) {
                qint64 id = 0;
                if (args.size() != 2) return QStringLiteral("Usage: task show <id>");
                return positiveInteger(args.at(1), std::numeric_limits<qint64>::max(), &id);
            }
            if (args.constFirst() != QStringLiteral("list"))
                return QStringLiteral("Expected 'list', 'show', 'add', or 'complete'.");
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
            if (args.constFirst() == QStringLiteral("add")) {
                storage::TaskInput input;
                input.title = args.at(1);
                input.priority = QStringLiteral("medium");
                for (qsizetype i = 2; i < args.size(); ++i) {
                    if (args.at(i) == QStringLiteral("--due")) input.dueDate = args.at(++i);
                    else if (args.at(i) == QStringLiteral("--priority")) input.priority = args.at(++i);
                }
                execution.data.insert(QStringLiteral("mutationKind"), QStringLiteral("task-add"));
                execution.data.insert(QStringLiteral("title"), input.title);
                execution.data.insert(QStringLiteral("dueDate"), input.dueDate);
                execution.data.insert(QStringLiteral("priority"), input.priority);
                return execution;
            }
            if (args.constFirst() == QStringLiteral("complete")) {
                execution.data.insert(QStringLiteral("mutationKind"), QStringLiteral("task-complete"));
                execution.data.insert(QStringLiteral("taskId"), args.at(1).toLongLong());
                execution.data.insert(QStringLiteral("revision"), args.at(3).toLongLong());
                return execution;
            }
            if (args.constFirst() == QStringLiteral("show")) {
                const qint64 id = args.at(1).toLongLong();
                execution.storageCommand = TaskService::get(id);
                execution.data.insert(QStringLiteral("resultShape"), QStringLiteral("task-show"));
                execution.data.insert(QStringLiteral("source"), QStringLiteral("fresh-query"));
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
            execution.data.insert(QStringLiteral("source"), QStringLiteral("fresh-query"));
            return execution;
        }, [](const QStringList &complete, const QString &partial) {
            QStringList values;
            if (complete.isEmpty()) values = {QStringLiteral("list"), QStringLiteral("show"),
                                              QStringLiteral("add"), QStringLiteral("complete")};
            else if (complete.first() == QStringLiteral("add") && complete.size() >= 2) {
                // The title occupies argument 1. Only later pairs are options;
                // a title equal to an option name must remain ordinary text.
                QSet<QString> usedOptions;
                for (qsizetype i = 2; i < complete.size(); i += 2) {
                    const QString option = complete.at(i);
                    if ((option != QStringLiteral("--priority") && option != QStringLiteral("--due"))
                        || usedOptions.contains(option)) return QStringList{};
                    usedOptions.insert(option);
                    if (i + 1 == complete.size()) {
                        return option == QStringLiteral("--priority")
                            ? QStringList{QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")}
                            : QStringList{};
                    }
                }
                if (!usedOptions.contains(QStringLiteral("--priority"))) values << QStringLiteral("--priority");
                if (!usedOptions.contains(QStringLiteral("--due"))) values << QStringLiteral("--due");
            }
            values.removeAll(partial);
            return values;
        }});

    registerPhaseThreeCommands();
}

void ApplicationCommandService::setSnapshotProvider(QString name,
                                                    std::function<QVariantMap()> provider)
{
    if (provider) m_snapshotProviders.insert(std::move(name), std::move(provider));
}

void ApplicationCommandService::setMutationWorkflow(ApplicationMutationWorkflow *workflow)
{
    m_mutations = workflow;
    if (!m_mutations) return;
    connect(m_mutations, &ApplicationMutationWorkflow::requestFinished,
            this, &ApplicationCommandService::onMutationFinished);
}

void ApplicationCommandService::onMutationFinished(const MutationResult &mutation)
{
    if (!m_mutationRequests.remove(mutation.requestId)) return;
    ConsoleRequestResult result = makeResult(mutation.requestId, mutation.state,
                                              m_mutationCommands.take(mutation.requestId));
    result.accepted = true;
    result.message = mutation.message;
    result.data = mutation.data;
    if (!mutation.errorCode.isEmpty()) result.data.insert(QStringLiteral("errorCode"), mutation.errorCode);
    if (mutation.state == QStringLiteral("cancelled")) emit requestCancelled(result);
    else emit requestFinished(result);
}

void ApplicationCommandService::registerPhaseThreeCommands()
{
    using application::CommandDefinition;
    auto reg = [this](CommandDefinition definition) {
        QString error;
        const bool ok = m_registry.registerCommand(std::move(definition), &error);
        Q_ASSERT_X(ok, "ApplicationCommandService::registerPhaseThreeCommands", qPrintable(error));
        Q_UNUSED(ok);
    };
    const auto listExecution = [](storage::StorageCommand command, const QString &shape,
                                  const QStringList &args, qsizetype firstOption) {
        int page = 1;
        listPage(args, firstOption, &page);
        return pagedStorage(std::move(command), shape, page);
    };

    reg({QStringLiteral("page"), QStringLiteral("List routes or select a route."),
         QStringLiteral("page list | page open <route-id>"),
         [](const QStringList &args) {
             if (args == QStringList{QStringLiteral("list")}) return QString();
             if (args.size() == 2 && args.first() == QStringLiteral("open"))
                 return application::RouteCatalog::contains(args.at(1))
                     ? QString() : QStringLiteral("Unknown route id.");
             return QStringLiteral("Usage: page list | page open <route-id>");
         },
         [](const QStringList &args) {
             application::CommandExecution execution;
             if (args.first() == QStringLiteral("list"))
                 execution.data.insert(QStringLiteral("routes"), application::RouteCatalog::routes());
             else {
                 execution.data.insert(QStringLiteral("routeId"), args.at(1));
                 execution.data.insert(QStringLiteral("title"), application::RouteCatalog::titleFor(args.at(1)));
                 execution.data.insert(QStringLiteral("openRoute"), args.at(1));
                 execution.data.insert(QStringLiteral("routeSelected"), true);
                 execution.data.insert(QStringLiteral("pageLoadConfirmed"), false);
             }
             return execution;
         }, [](const QStringList &complete, const QString &) {
             if (complete.isEmpty()) return QStringList{QStringLiteral("list"), QStringLiteral("open")};
             if (complete == QStringList{QStringLiteral("open")}) {
                 QStringList routes;
                 for (const QVariant &row : application::RouteCatalog::routes())
                     routes.push_back(row.toMap().value(QStringLiteral("id")).toString());
                 return routes;
             }
             return QStringList{};
         }});

    reg({QStringLiteral("settings"), QStringLiteral("Show supported retained settings or update the week start."),
         QStringLiteral("settings show | settings week-start <0..6>"), [](const QStringList &args) {
             if (args == QStringList{QStringLiteral("show")}) return QString();
             if (args.size() == 2 && args.first() == QStringLiteral("week-start")) {
                 bool ok = false;
                 const int value = args.at(1).toInt(&ok, 10);
                 return ok && value >= 0 && value <= 6 ? QString()
                     : QStringLiteral("Week start must be a decimal integer from 0 to 6.");
             }
             return QStringLiteral("Usage: settings show | settings week-start <0..6>");
         }, [this](const QStringList &args) {
             application::CommandExecution execution;
             if (args.first() == QStringLiteral("week-start")) {
                 execution.data.insert(QStringLiteral("mutationKind"), QStringLiteral("week-start"));
                 execution.data.insert(QStringLiteral("weekStart"), args.at(1).toInt());
                 return execution;
             }
             const auto it = m_snapshotProviders.constFind(QStringLiteral("settings"));
             execution.data = it == m_snapshotProviders.cend()
                 ? QVariantMap{{QStringLiteral("available"), false},
                               {QStringLiteral("message"), QStringLiteral("Settings snapshot is unavailable.")}}
                 : it.value()();
             return execution;
         }, [](const QStringList &complete, const QString &partial) {
             if (complete.isEmpty()) return QStringList{QStringLiteral("show"), QStringLiteral("week-start")};
             if (complete == QStringList{QStringLiteral("week-start")})
                 return QStringList{QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2"),
                                    QStringLiteral("3"), QStringLiteral("4"), QStringLiteral("5"), QStringLiteral("6")};
             Q_UNUSED(partial);
             return QStringList{};
         }});

    reg({QStringLiteral("wallpaper"), QStringLiteral("List built-in and imported wallpaper catalog entries."),
         QStringLiteral("wallpaper list [--page n]"), [](const QStringList &args) {
             if (args.isEmpty() || args.first() != QStringLiteral("list"))
                 return QStringLiteral("Usage: wallpaper list [--page n]");
             int page = 1;
             return listPage(args, 1, &page);
         }, [listExecution](const QStringList &args) {
             return listExecution(WallpaperService::loadCatalog(), QStringLiteral("wallpapers"),
                                  pageArguments(args), 0);
         }});

    reg({QStringLiteral("calendar"), QStringLiteral("Read tasks due on a calendar date."),
         QStringLiteral("calendar tasks <YYYY-MM-DD> [--page n]"), [](const QStringList &args) {
             if (args.size() < 2 || args.first() != QStringLiteral("tasks"))
                 return QStringLiteral("Usage: calendar tasks <YYYY-MM-DD> [--page n]");
             const QDate date = QDate::fromString(args.at(1), Qt::ISODate);
             if (!date.isValid() || date.toString(Qt::ISODate) != args.at(1))
                 return QStringLiteral("Date must be a valid YYYY-MM-DD date.");
             int page = 1;
             return listPage(args.mid(2), 0, &page);
         }, [](const QStringList &args) {
             const QDate date = QDate::fromString(args.at(1), Qt::ISODate);
             int page = 1;
             listPage(args, 2, &page);
             auto execution = pagedStorage(TaskService::calendarDatePage(date, (page - 1) * 50),
                                           QStringLiteral("calendar-tasks"), page);
             execution.data.insert(QStringLiteral("date"), date.toString(Qt::ISODate));
             execution.data.insert(QStringLiteral("source"), QStringLiteral("fresh-query"));
             return execution;
         }});

    reg({QStringLiteral("dashboard"), QStringLiteral("Read today's or this week's task summaries."),
         QStringLiteral("dashboard tasks today|week"), [](const QStringList &args) {
             if (args.size() < 2 || args.first() != QStringLiteral("tasks")
                 || (args.at(1) != QStringLiteral("today") && args.at(1) != QStringLiteral("week")))
                 return QStringLiteral("Usage: dashboard tasks today|week");
             return args.size() == 2 ? QString()
                 : QStringLiteral("Usage: dashboard tasks today|week");
         }, [this](const QStringList &args) {
             const QDate today = m_clock ? m_clock->localDate() : QDate::currentDate();
             const bool week = args.at(1) == QStringLiteral("week");
             auto command = week ? TaskService::dashboardWeek(today)
                                 : TaskService::dashboardToday(today);
             auto execution = pagedStorage(std::move(command), week ? QStringLiteral("dashboard-week")
                                                                   : QStringLiteral("dashboard-today"), 1);
             execution.data.insert(QStringLiteral("pageSize"), 50);
             return execution;
         }});

    reg({QStringLiteral("favorites"), QStringLiteral("List saved Telegram or YouTube favorites."),
         QStringLiteral("favorites list <telegram|youtube> [--page n]"), [](const QStringList &args) {
             if (args.size() < 2 || args.first() != QStringLiteral("list")
                 || (args.at(1) != QStringLiteral("telegram") && args.at(1) != QStringLiteral("youtube")))
                 return QStringLiteral("Usage: favorites list <telegram|youtube> [--page n]");
             int page = 1;
             return listPage(args.mid(2), 0, &page);
         }, [listExecution](const QStringList &args) {
             return listExecution(FavoritesService::loadSourceSnapshot(args.at(1)),
                                  QStringLiteral("favorites"), args, 2);
         }, [](const QStringList &complete, const QString &) {
             if (complete.isEmpty()) return QStringList{QStringLiteral("list")};
             if (complete == QStringList{QStringLiteral("list")})
                 return QStringList{QStringLiteral("telegram"), QStringLiteral("youtube")};
             return QStringList{};
         }});

    reg({QStringLiteral("news"), QStringLiteral("List cached favorite-provider news."),
         QStringLiteral("news list [--page n]"), [](const QStringList &args) {
             if (args.isEmpty() || args.first() != QStringLiteral("list"))
                 return QStringLiteral("Usage: news list [--page n]");
             int page = 1;
             return listPage(args, 1, &page);
         }, [listExecution](const QStringList &args) {
             return listExecution(FavoritesService::recentNews(500), QStringLiteral("items"),
                                  pageArguments(args), 0);
         }});

    reg({QStringLiteral("apps"), QStringLiteral("List saved application records (without launching them)."),
         QStringLiteral("apps list [--page n]"), [](const QStringList &args) {
             if (args.isEmpty() || args.first() != QStringLiteral("list"))
                 return QStringLiteral("Usage: apps list [--page n]");
             int page = 1;
             return listPage(args, 1, &page);
         }, [listExecution](const QStringList &args) {
             return listExecution(DesktopApplicationsService::list(), QStringLiteral("applications"),
                                  pageArguments(args), 0);
         }});

    reg({QStringLiteral("setups"), QStringLiteral("List saved setup records."),
         QStringLiteral("setups list [--page n]"), [](const QStringList &args) {
             if (args.isEmpty() || args.first() != QStringLiteral("list"))
                 return QStringLiteral("Usage: setups list [--page n]");
             int page = 1;
             return listPage(args, 1, &page);
         }, [listExecution](const QStringList &args) {
             return listExecution(SetupsService::list(), QStringLiteral("setups"),
                                  pageArguments(args), 0);
         }});

    reg({QStringLiteral("steam"), QStringLiteral("List games from the already-loaded Steam snapshot."),
         QStringLiteral("steam list [--page n]"), [](const QStringList &args) {
             if (args.isEmpty() || args.first() != QStringLiteral("list"))
                 return QStringLiteral("Usage: steam list [--page n]");
             int page = 1;
             return listPage(args, 1, &page);
         }, [this](const QStringList &args) {
             application::CommandExecution execution;
             const auto it = m_snapshotProviders.constFind(QStringLiteral("steam"));
             QVariantMap snapshot = it == m_snapshotProviders.cend()
                 ? QVariantMap{{QStringLiteral("available"), false},
                               {QStringLiteral("message"), QStringLiteral("Steam snapshot is not loaded.")}}
                 : it.value()();
             if (snapshot.value(QStringLiteral("available")).toBool()) {
                 int page = 1;
                 listPage(args, 1, &page);
                 const QVariantList games = snapshot.take(QStringLiteral("games")).toList();
                 const int start = (page - 1) * 50;
                 QVariantList displayed;
                 for (int i = start; i < games.size() && displayed.size() < 50; ++i)
                     displayed.push_back(games.at(i));
                 const bool more = start + displayed.size() < games.size();
                 snapshot.insert(QStringLiteral("games"), displayed);
                 snapshot.insert(QStringLiteral("page"), page);
                 snapshot.insert(QStringLiteral("pageSize"), 50);
                 snapshot.insert(QStringLiteral("truncated"), more);
                 snapshot.insert(QStringLiteral("hasMore"), more);
             }
             execution.data = snapshot;
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
    auto reject = [this, requestId](QString command, QString message, QString errorCode = QString()) {
        auto result = makeResult(requestId, QStringLiteral("rejected"), std::move(command));
        result.message = std::move(message);
        if (!errorCode.isEmpty()) result.data.insert(QStringLiteral("errorCode"), std::move(errorCode));
        emit requestRejected(result);
        return requestId;
    };
    const auto parsed = application::CommandParser::parse(input);
    if (!parsed.succeeded()) return reject({}, parsed.error, QStringLiteral("syntax"));
    if (parsed.tokens.isEmpty()) return reject({}, QStringLiteral("Enter a command."),
                                               QStringLiteral("validation"));
    const QString commandName = parsed.tokens.constFirst();
    const auto *definition = m_registry.find(commandName);
    if (!definition) return reject(commandName,
        QStringLiteral("Unknown command. Use 'help' to list registered commands."),
        QStringLiteral("unknown-command"));
    const QStringList arguments = parsed.tokens.mid(1);
    const QString validationError = m_registry.validate(*definition, arguments);
    if (!validationError.isEmpty()) return reject(commandName, validationError, QStringLiteral("validation"));

    const bool queryCommand = commandName != QStringLiteral("help")
        && commandName != QStringLiteral("status") && commandName != QStringLiteral("page");
    const bool mutationCommand = (commandName == QStringLiteral("task")
                                  && !arguments.isEmpty()
                                  && (arguments.first() == QStringLiteral("add")
                                      || arguments.first() == QStringLiteral("complete")))
        || (commandName == QStringLiteral("settings") && arguments.size() == 2
            && arguments.first() == QStringLiteral("week-start"));
    if (queryCommand && !mutationCommand) {
        const QString unavailable = queryAvailabilityError();
        if (!unavailable.isEmpty()) {
            QString code = QStringLiteral("unavailable");
            if (m_graph->lifecycle().state() == application::ApplicationLifecycle::State::Starting)
                code = QStringLiteral("starting");
            else if (m_graph->isProfileMaintenance()) code = QStringLiteral("maintenance");
            else if (m_graph->lifecycle().state() == application::ApplicationLifecycle::State::ShuttingDown
                     || m_graph->lifecycle().state() == application::ApplicationLifecycle::State::Stopped)
                code = QStringLiteral("shutdown");
            return reject(commandName, unavailable, std::move(code));
        }
    } else if (!mutationCommand && (m_graph->lifecycle().state() == application::ApplicationLifecycle::State::ShuttingDown
               || m_graph->lifecycle().state() == application::ApplicationLifecycle::State::Stopped)) {
        return reject(commandName, QStringLiteral("The application is shutting down."),
                      QStringLiteral("shutdown"));
    }

    const application::CommandExecution execution = definition->handler(arguments);
    const QString mutationKind = execution.data.value(QStringLiteral("mutationKind")).toString();
    if (!mutationKind.isEmpty()) {
        if (!m_mutations) return reject(commandName, QStringLiteral("Application mutations are unavailable."),
                                        QStringLiteral("unavailable"));
        if (m_pending.size() + m_mutationRequests.size() >= MaximumPendingRequests)
            return reject(commandName, QStringLiteral("Console is busy; try again when a request finishes."),
                          QStringLiteral("busy"));
        m_mutationRequests.insert(requestId);
        m_mutationCommands.insert(requestId, commandName);
        const QVariantMap pendingData = execution.data;
        const auto admitted = [this, requestId, commandName, pendingData] {
            ConsoleRequestResult pending = makeResult(requestId, QStringLiteral("pending"), commandName);
            pending.accepted = true;
            pending.data = pendingData;
            emit requestAccepted(pending);
            emit requestPending(pending);
        };
        MutationAdmission admission;
        if (mutationKind == QStringLiteral("task-add")) {
            storage::TaskInput input;
            input.title = execution.data.value(QStringLiteral("title")).toString();
            input.dueDate = execution.data.value(QStringLiteral("dueDate")).toString();
            input.priority = execution.data.value(QStringLiteral("priority")).toString();
            admission = m_mutations->addTask(requestId, std::move(input), admitted);
        } else if (mutationKind == QStringLiteral("task-complete")) {
            admission = m_mutations->completeTask(requestId,
                execution.data.value(QStringLiteral("taskId")).toLongLong(),
                execution.data.value(QStringLiteral("revision")).toLongLong(), true, admitted);
        } else if (mutationKind == QStringLiteral("week-start")) {
            admission = m_mutations->setWeekStart(requestId,
                execution.data.value(QStringLiteral("weekStart")).toInt(), admitted);
        } else {
            admission = {false, QStringLiteral("unavailable"), QStringLiteral("Mutation is not supported.")};
        }
        if (!admission.accepted) {
            m_mutationRequests.remove(requestId);
            m_mutationCommands.remove(requestId);
            return reject(commandName, admission.message, admission.errorCode);
        }
        return requestId;
    }
    if (!execution.storageCommand && execution.data.contains(QStringLiteral("openRoute"))) {
        ConsoleRequestResult accepted = makeResult(requestId, QStringLiteral("succeeded"), commandName);
        accepted.accepted = true;
        emit requestAccepted(accepted);
        emit routeRequested(execution.data.value(QStringLiteral("openRoute")).toString());
        accepted.data = execution.data;
        emit requestFinished(accepted);
        return requestId;
    }
    ConsoleRequestResult accepted = makeResult(requestId,
        execution.storageCommand ? QStringLiteral("pending") : QStringLiteral("succeeded"), commandName);
    accepted.accepted = true;
    accepted.data = execution.data;
    if (!execution.storageCommand) {
        emit requestAccepted(accepted);
        emit requestFinished(accepted);
        return requestId;
    }
    if (m_pending.size() + m_mutationRequests.size() >= MaximumPendingRequests)
        return reject(commandName, QStringLiteral("Console is busy; try again when a request finishes."),
                      QStringLiteral("busy"));

    const auto operation = m_graph->beginOperation(QStringLiteral("console: ") + commandName);
    if (!operation.isValid()) {
        QString reason = availabilityError();
        if (reason.isEmpty()) reason = QStringLiteral("The application could not accept this request.");
        QString code = QStringLiteral("busy");
        if (m_graph->lifecycle().state() == application::ApplicationLifecycle::State::Starting)
            code = QStringLiteral("starting");
        else if (m_graph->isProfileMaintenance()) code = QStringLiteral("maintenance");
        else if (m_graph->lifecycle().state() == application::ApplicationLifecycle::State::ShuttingDown
                 || m_graph->lifecycle().state() == application::ApplicationLifecycle::State::Stopped)
            code = QStringLiteral("shutdown");
        else if (!m_graph->capabilities().isAvailable(QStringLiteral("local-storage")))
            code = QStringLiteral("unavailable");
        return reject(commandName, std::move(reason), std::move(code));
    }
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
    return queryAvailabilityError();
}

QString ApplicationCommandService::queryAvailabilityError() const
{
    using State = application::ApplicationLifecycle::State;
    const State state = m_graph->lifecycle().state();
    if (state == State::Starting) return QStringLiteral("Application startup is still in progress.");
    if (state == State::Failed) return QStringLiteral("Application startup failed; application data is unavailable.");
    if (state == State::ShuttingDown || state == State::Stopped)
        return QStringLiteral("The application is shutting down.");
    if (m_graph->isProfileMaintenance()) return QStringLiteral("Read commands are unavailable during profile maintenance.");
    if (!m_graph->capabilities().isAvailable(QStringLiteral("local-storage")))
        return QStringLiteral("Local application data is unavailable.");
    return {};
}

bool ApplicationCommandService::cancel(const QString &requestId)
{
    if (m_mutationRequests.contains(requestId) && m_mutations)
        return m_mutations->cancel(requestId);
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
        finish(requestId, QStringLiteral("cancelled"), QStringLiteral("Request cancelled."),
               {{QStringLiteral("errorCode"), QStringLiteral("cancelled")}});
        return;
    }
    if (error.isError()) {
        finish(requestId, QStringLiteral("failed"), error.userMessage(),
               {{QStringLiteral("errorCode"), errorCodeFor(error.category())}});
        return;
    }
    QVariantMap data = pending->result.data;
    const QString resultShape = data.take(QStringLiteral("resultShape")).toString();
    const int page = qMax(1, data.value(QStringLiteral("page")).toInt());
    const int pageSize = qMax(1, data.value(QStringLiteral("pageSize")).toInt());
    auto pageList = [page, pageSize](QVariantList rows, bool hasMore) {
        const int start = (page - 1) * pageSize;
        QVariantList selected;
        for (int i = start; i < rows.size() && selected.size() < pageSize; ++i)
            selected.push_back(rows.at(i));
        const bool more = hasMore || start + selected.size() < rows.size();
        return QVariantMap{{QStringLiteral("items"), selected},
                           {QStringLiteral("page"), page},
                           {QStringLiteral("pageSize"), pageSize},
                           {QStringLiteral("total"), rows.size()},
                           {QStringLiteral("truncated"), more},
                           {QStringLiteral("hasMore"), more}};
    };
    auto sanitizeApplications = [](QVariantList rows) {
        for (QVariant &value : rows) {
            QVariantMap row = value.toMap();
            row.remove(QStringLiteral("executablePath"));
            value = row;
        }
        return rows;
    };
    if (resultShape == QStringLiteral("task-show")) {
        data.insert(QStringLiteral("task"), values.value(QStringLiteral("task")));
        data.insert(QStringLiteral("source"), QStringLiteral("fresh-query"));
    } else if (resultShape == QStringLiteral("task-list")) {
        data.insert(QStringLiteral("tasks"), values.value(QStringLiteral("tasks")));
        data.insert(QStringLiteral("hasMore"), values.value(QStringLiteral("hasMore")));
        data.insert(QStringLiteral("source"), QStringLiteral("fresh-query"));
    } else if (resultShape == QStringLiteral("calendar-tasks")) {
        const QVariantList rows = values.value(QStringLiteral("tasks")).toList();
        const bool more = values.value(QStringLiteral("hasMore")).toBool();
        data.insert(QStringLiteral("tasks"), rows);
        data.insert(QStringLiteral("hasMore"), more);
        data.insert(QStringLiteral("truncated"), more);
        data.insert(QStringLiteral("totalKnown"), !more);
        data.insert(QStringLiteral("total"), (page - 1) * pageSize + rows.size());
    } else if (resultShape == QStringLiteral("dashboard-today")) {
        data.insert(QStringLiteral("source"), QStringLiteral("fresh-query"));
        for (const QString &key : {QStringLiteral("dueToday"), QStringLiteral("overdue"), QStringLiteral("undated")}) {
            const QVariantList rows = values.value(key).toList();
            QVariantMap pageData = pageList(rows, rows.size() >= 200);
            pageData.insert(QStringLiteral("sourceLimit"), 200);
            pageData.insert(QStringLiteral("totalKnown"), rows.size() < 200);
            data.insert(key, pageData);
        }
        data.insert(QStringLiteral("todayDate"), values.value(QStringLiteral("todayDate")));
    } else if (resultShape == QStringLiteral("dashboard-week")) {
        data.insert(QStringLiteral("source"), QStringLiteral("fresh-query"));
        for (const QString &key : {QStringLiteral("items"), QStringLiteral("overdue"), QStringLiteral("undated")}) {
            const QVariantList rows = values.value(key).toList();
            QVariantMap pageData = pageList(rows, rows.size() >= 200);
            pageData.insert(QStringLiteral("sourceLimit"), 200);
            pageData.insert(QStringLiteral("totalKnown"), rows.size() < 200);
            data.insert(key, pageData);
        }
        data.insert(QStringLiteral("fromDate"), values.value(QStringLiteral("fromDate")));
        data.insert(QStringLiteral("throughDate"), values.value(QStringLiteral("throughDate")));
        data.insert(QStringLiteral("weekStart"), values.value(QStringLiteral("weekStart")));
    } else {
        QString listKey = resultShape;
        if (resultShape == QStringLiteral("favorites")) listKey = QStringLiteral("favorites");
        QVariantList rows = values.value(listKey).toList();
        if (resultShape == QStringLiteral("wallpapers")) {
            QVariantList combined = WallpaperService::builtInCatalog();
            for (QVariant value : rows) {
                QVariantMap row = value.toMap();
                row.insert(QStringLiteral("builtIn"), false);
                row.insert(QStringLiteral("id"), QStringLiteral("custom:") + row.value(QStringLiteral("id")).toString());
                combined.push_back(row);
            }
            rows = combined;
        }
        if (resultShape == QStringLiteral("applications")) rows = sanitizeApplications(rows);
        QVariantMap paged = pageList(rows, false);
        data.insert(listKey, paged.value(QStringLiteral("items")));
        data.insert(QStringLiteral("page"), paged.value(QStringLiteral("page")));
        data.insert(QStringLiteral("pageSize"), paged.value(QStringLiteral("pageSize")));
        data.insert(QStringLiteral("total"), paged.value(QStringLiteral("total")));
        data.insert(QStringLiteral("truncated"), paged.value(QStringLiteral("truncated")));
        data.insert(QStringLiteral("hasMore"), paged.value(QStringLiteral("hasMore")));
        data.insert(QStringLiteral("source"), QStringLiteral("fresh-query"));
        if (resultShape == QStringLiteral("favorites")) {
            data.insert(QStringLiteral("sourceName"), values.value(QStringLiteral("source")));
        }
        if (resultShape == QStringLiteral("items")) {
            const bool available = values.value(QStringLiteral("available"), true).toBool();
            data.insert(QStringLiteral("available"), available);
            data.insert(QStringLiteral("source"), values.value(QStringLiteral("source"), QStringLiteral("fresh-query")));
            data.insert(QStringLiteral("sourceLimit"), 500);
            data.insert(QStringLiteral("totalKnown"), available && rows.size() < 500);
            if (rows.size() == 500) {
                data.insert(QStringLiteral("hasMore"), true);
                data.insert(QStringLiteral("truncated"), true);
            }
        }
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
        finish(requestIt.value(), QStringLiteral("cancelled"), QStringLiteral("Request cancelled."),
               {{QStringLiteral("errorCode"), QStringLiteral("cancelled")}});
}

} // namespace something::services

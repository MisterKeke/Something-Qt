#include "terminal_controller.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantList>
#include <QVariantMap>

namespace something::controllers {
namespace {

QString taskSummary(const QVariantMap &task)
{
    const QString state = task.value(QStringLiteral("completed")).toBool()
        ? QStringLiteral("completed") : QStringLiteral("open");
    QStringList lines;
    lines.push_back(QStringLiteral("Task #%1 — %2 (%3)")
        .arg(task.value(QStringLiteral("id")).toLongLong())
        .arg(task.value(QStringLiteral("title")).toString(), state));
    const QString description = task.value(QStringLiteral("description")).toString();
    if (!description.isEmpty()) lines.push_back(QStringLiteral("  %1").arg(description));
    lines.push_back(QStringLiteral("  Priority: %1").arg(task.value(QStringLiteral("priority")).toString()));
    const QString dueDate = task.value(QStringLiteral("dueDate")).toString();
    if (!dueDate.isEmpty()) lines.push_back(QStringLiteral("  Due: %1").arg(dueDate));
    const QStringList tags = task.value(QStringLiteral("tags")).toStringList();
    if (!tags.isEmpty()) lines.push_back(QStringLiteral("  Tags: %1").arg(tags.join(QStringLiteral(", "))));
    const QVariantList subtasks = task.value(QStringLiteral("subtasks")).toList();
    for (const QVariant &value : subtasks) {
        const QVariantMap subtask = value.toMap();
        lines.push_back(QStringLiteral("  [%1] %2")
            .arg(subtask.value(QStringLiteral("completed")).toBool()
                     ? QStringLiteral("✓") : QStringLiteral(" "),
                 subtask.value(QStringLiteral("title")).toString()));
    }
    return lines.join(QLatin1Char('\n'));
}

QString jsonText(const QVariantMap &data)
{
    return QString::fromUtf8(QJsonDocument::fromVariant(data).toJson(QJsonDocument::Indented)).trimmed();
}

} // namespace

TerminalController::TerminalController(services::ApplicationCommandService *commands,
                                       QObject *parent)
    : QObject(parent), m_commands(commands)
{
    Q_ASSERT(m_commands);
    connect(m_commands, &services::ApplicationCommandService::requestRejected,
            this, &TerminalController::onRejected);
    connect(m_commands, &services::ApplicationCommandService::requestPending,
            this, &TerminalController::onPending);
    connect(m_commands, &services::ApplicationCommandService::requestFinished,
            this, &TerminalController::onFinished);
    connect(m_commands, &services::ApplicationCommandService::requestCancelled,
            this, &TerminalController::onCancelled);
    connect(m_commands, &services::ApplicationCommandService::routeRequested,
            this, &TerminalController::routeRequested);
}

void TerminalController::submit(const QString &input)
{
    if (input.trimmed().isEmpty()) return;
    m_output.append(QStringLiteral("> %1").arg(input), QStringLiteral("normal"));
    addHistory(input);
    resetHistoryNavigation();
    m_commands->submit(input);
}

void TerminalController::clearOutput()
{
    m_output.clear();
}

QString TerminalController::historyUp(const QString &currentDraft)
{
    if (m_history.isEmpty()) return currentDraft;
    if (m_historyPosition >= m_history.size()) m_savedDraft = currentDraft;
    if (m_historyPosition > 0) --m_historyPosition;
    return m_history.at(m_historyPosition);
}

QString TerminalController::historyDown(const QString &currentDraft)
{
    if (m_historyPosition >= m_history.size()) return currentDraft;
    if (m_historyPosition < m_history.size() - 1) {
        ++m_historyPosition;
        return m_history.at(m_historyPosition);
    }
    if (m_historyPosition == m_history.size() - 1) {
        m_historyPosition = m_history.size();
        return m_savedDraft;
    }
    return m_savedDraft;
}

void TerminalController::addHistory(const QString &input)
{
    if (m_history.isEmpty() || m_history.constLast() != input)
        m_history.push_back(input);
    constexpr int maximumHistoryEntries = 100;
    while (m_history.size() > maximumHistoryEntries) m_history.removeFirst();
    emit stateChanged();
}

void TerminalController::resetHistoryNavigation()
{
    m_historyPosition = m_history.size();
    m_savedDraft.clear();
}

void TerminalController::onRejected(const services::ConsoleRequestResult &result)
{
    m_output.append(QStringLiteral("Rejected: %1").arg(result.message), QStringLiteral("error"));
}

void TerminalController::onPending(const services::ConsoleRequestResult &result)
{
    m_pendingCommands.insert(result.requestId, result.command);
    m_output.append(QStringLiteral("Pending · %1 · %2")
        .arg(result.command, result.requestId), QStringLiteral("pending"));
    emit stateChanged();
}

void TerminalController::onFinished(const services::ConsoleRequestResult &result)
{
    m_pendingCommands.remove(result.requestId);
    if (result.state == QStringLiteral("failed")) {
        m_output.append(QStringLiteral("Error: %1").arg(result.message), QStringLiteral("error"));
    } else {
        appendResultData(result);
    }
    emit stateChanged();
}

void TerminalController::onCancelled(const services::ConsoleRequestResult &result)
{
    m_pendingCommands.remove(result.requestId);
    m_output.append(QStringLiteral("Cancelled · %1").arg(result.command), QStringLiteral("warning"));
    emit stateChanged();
}

void TerminalController::appendResultData(const services::ConsoleRequestResult &result)
{
    const QString text = formatResultData(result);
    if (!text.isEmpty()) {
        const QString kind = result.data.contains(QStringLiteral("available"))
                && !result.data.value(QStringLiteral("available")).toBool()
            ? QStringLiteral("warning") : QStringLiteral("success");
        m_output.append(text, kind);
    }
}

QString TerminalController::formatResultData(const services::ConsoleRequestResult &result) const
{
    if (result.command == QStringLiteral("help")) {
        const QVariantList commands = result.data.value(QStringLiteral("commands")).toList();
        if (!commands.isEmpty()) {
            QStringList lines{QStringLiteral("Registered application commands:")};
            for (const QVariant &value : commands) {
                const QVariantMap entry = value.toMap();
                lines.push_back(QStringLiteral("  %1 — %2\n      %3")
                    .arg(entry.value(QStringLiteral("name")).toString(),
                         entry.value(QStringLiteral("description")).toString(),
                         entry.value(QStringLiteral("usage")).toString()));
            }
            const QString escape = result.data.value(QStringLiteral("escapeSyntax")).toString();
            if (!escape.isEmpty()) lines.push_back(escape);
            return lines.join(QLatin1Char('\n'));
        }
        if (result.data.contains(QStringLiteral("usage"))) {
            return QStringLiteral("%1\n%2\nUsage: %3")
                .arg(result.data.value(QStringLiteral("command")).toString(),
                     result.data.value(QStringLiteral("description")).toString(),
                     result.data.value(QStringLiteral("usage")).toString());
        }
    }
    if (result.command == QStringLiteral("task")) {
        if (result.data.contains(QStringLiteral("task")))
            return taskSummary(result.data.value(QStringLiteral("task")).toMap());
        if (result.data.contains(QStringLiteral("tasks"))) {
            const QVariantList tasks = result.data.value(QStringLiteral("tasks")).toList();
            QStringList lines{QStringLiteral("Page %1 · %2 task(s)%3")
                .arg(result.data.value(QStringLiteral("page")).toInt())
                .arg(tasks.size())
                .arg(result.data.value(QStringLiteral("hasMore")).toBool()
                         ? QStringLiteral(" · more results available") : QString())};
            for (const QVariant &value : tasks) lines.push_back(taskSummary(value.toMap()));
            if (tasks.isEmpty()) lines.push_back(QStringLiteral("No matching tasks."));
            return lines.join(QStringLiteral("\n\n"));
        }
    }
    return jsonText(result.data);
}

} // namespace something::controllers

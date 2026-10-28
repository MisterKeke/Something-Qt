#pragma once

#include "../models/terminal_output_model.h"
#include "../services/application_command_service.h"

#include <QObject>
#include <QHash>
#include <QStringList>

namespace something::controllers {

class TerminalController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::TerminalOutputModel *output READ output CONSTANT)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY stateChanged)
    Q_PROPERTY(int historyCount READ historyCount NOTIFY stateChanged)

public:
    explicit TerminalController(services::ApplicationCommandService *commands,
                                QObject *parent = nullptr);

    models::TerminalOutputModel *output() noexcept { return &m_output; }
    int pendingCount() const noexcept { return static_cast<int>(m_pendingCommands.size()); }
    int historyCount() const noexcept { return static_cast<int>(m_history.size()); }

    Q_INVOKABLE void submit(const QString &input);
    Q_INVOKABLE void clearOutput();
    Q_INVOKABLE QString historyUp(const QString &currentDraft);
    Q_INVOKABLE QString historyDown(const QString &currentDraft);

signals:
    void stateChanged();
    void routeRequested(const QString &routeId);

private:
    void onRejected(const services::ConsoleRequestResult &result);
    void onPending(const services::ConsoleRequestResult &result);
    void onFinished(const services::ConsoleRequestResult &result);
    void onCancelled(const services::ConsoleRequestResult &result);
    void appendResultData(const services::ConsoleRequestResult &result);
    QString formatResultData(const services::ConsoleRequestResult &result) const;
    void addHistory(const QString &input);
    void resetHistoryNavigation();

    services::ApplicationCommandService *m_commands{nullptr};
    models::TerminalOutputModel m_output;
    QHash<QString, QString> m_pendingCommands;
    QStringList m_history;
    int m_historyPosition{0};
    QString m_savedDraft;
};

} // namespace something::controllers

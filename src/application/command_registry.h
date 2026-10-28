#pragma once

#include "../storage/storage_worker.h"

#include <QVariantMap>
#include <QVariantList>
#include <QStringList>
#include <QHash>
#include <functional>

namespace something::application {

struct CommandExecution final {
    QVariantMap data;
    storage::StorageCommand storageCommand;
};

struct CommandDefinition final {
    QString name;
    QString description;
    QString usage;
    std::function<QString(const QStringList &arguments)> validateArguments;
    std::function<CommandExecution(const QStringList &arguments)> handler;
    // Explicit argument completion metadata. Arguments exclude the command
    // name; the final token may be partial or empty.
    std::function<QStringList(const QStringList &completeArguments,
                              const QString &partialArgument)> completeArguments;
};

struct CommandLookup final {
    const CommandDefinition *definition{nullptr};
    QString error;
};

// Only explicitly registered definitions are executable. The metadata view
// is also the source for help and future completion UI.
class CommandRegistry final {
public:
    bool registerCommand(CommandDefinition definition, QString *error = nullptr);
    const CommandDefinition *find(const QString &name) const;
    QStringList names() const;
    QVariantList metadata() const;
    QString validate(const CommandDefinition &definition,
                     const QStringList &arguments) const;

private:
    QHash<QString, CommandDefinition> m_commands;
};

} // namespace something::application

#include "command_registry.h"

#include <QRegularExpression>
#include <algorithm>
#include <utility>

namespace something::application {

bool CommandRegistry::registerCommand(CommandDefinition definition, QString *error)
{
    static const QRegularExpression validName(QStringLiteral("^[a-z][a-z0-9-]{0,31}$"));
    const auto reject = [error](QString message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (!validName.match(definition.name).hasMatch())
        return reject(QStringLiteral("Command names must be lowercase ASCII words or hyphenated words."));
    if (definition.description.trimmed().isEmpty() || definition.usage.trimmed().isEmpty()
        || !definition.validateArguments || !definition.handler)
        return reject(QStringLiteral("A command requires description, usage, validator, and handler."));
    if (m_commands.contains(definition.name))
        return reject(QStringLiteral("That command name is already registered."));
    m_commands.insert(definition.name, std::move(definition));
    if (error) error->clear();
    return true;
}

const CommandDefinition *CommandRegistry::find(const QString &name) const
{
    const auto it = m_commands.constFind(name);
    return it == m_commands.cend() ? nullptr : &it.value();
}

QStringList CommandRegistry::names() const
{
    QStringList result = m_commands.keys();
    std::sort(result.begin(), result.end());
    return result;
}

QVariantList CommandRegistry::metadata() const
{
    QVariantList result;
    for (const QString &name : names()) {
        const auto &definition = m_commands[name];
        result.push_back(QVariantMap{
            {QStringLiteral("name"), definition.name},
            {QStringLiteral("description"), definition.description},
            {QStringLiteral("usage"), definition.usage},
        });
    }
    return result;
}

QString CommandRegistry::validate(const CommandDefinition &definition,
                                 const QStringList &arguments) const
{
    const auto registered = m_commands.constFind(definition.name);
    if (registered == m_commands.cend() || &registered.value() != &definition)
        return QStringLiteral("Command is not registered.");
    return definition.validateArguments(arguments);
}

} // namespace something::application

#include "desktop_application_rules.h"

#include <QFileInfo>

namespace something::domain {

std::optional<QString> normalizeDesktopApplicationName(QString value)
{
    value = value.trimmed();
    const qsizetype length = value.toUcs4().size();
    if (value.isEmpty() || length > 120) return std::nullopt;
    return value;
}

QString defaultDesktopApplicationName(const QString &executablePath)
{
    return QFileInfo(executablePath).completeBaseName().trimmed();
}

} // namespace something::domain

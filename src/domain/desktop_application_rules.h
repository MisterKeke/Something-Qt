#pragma once

#include <QString>

#include <optional>

namespace something::domain {

std::optional<QString> normalizeDesktopApplicationName(QString value);
QString defaultDesktopApplicationName(const QString &executablePath);

} // namespace something::domain

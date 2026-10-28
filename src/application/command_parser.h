#pragma once

#include <QString>
#include <QStringList>

namespace something::application {

struct CommandParseResult final {
    QStringList tokens;
    QString error;
    int errorOffset{-1};
    bool succeeded() const noexcept { return error.isEmpty(); }
};

// Parses a deliberately small command language. Backslash escapes only a
// quote, backslash, or whitespace character. Outside quotes, shell operators
// and expansion markers are rejected; there is no expansion or evaluation.
class CommandParser final {
public:
    static constexpr qsizetype MaximumInputLength = 4096;
    static constexpr qsizetype MaximumTokenCount = 64;

    static CommandParseResult parse(const QString &input);
};

} // namespace something::application

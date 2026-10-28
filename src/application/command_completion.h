#pragma once

#include "command_registry.h"

namespace something::application {

// Completes only a token at the caret. It accepts unfinished quoted tokens
// so Tab remains useful while the user is entering an argument.
class CommandCompletion final {
public:
    static QVariantMap complete(const CommandRegistry &registry, const QString &input,
                                int cursorPosition);
    static QString apply(const QString &input, const QVariantMap &suggestion,
                         int *newCursorPosition = nullptr);
};

} // namespace something::application

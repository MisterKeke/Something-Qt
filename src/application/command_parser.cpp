#include "command_parser.h"

#include <QChar>

namespace something::application {

CommandParseResult CommandParser::parse(const QString &input)
{
    CommandParseResult result;
    if (input.size() > MaximumInputLength) {
        result.error = QStringLiteral("Command is too long (maximum 4096 characters).");
        return result;
    }

    enum class Quote { None, Single, Double };
    Quote quote = Quote::None;
    QString token;
    bool tokenStarted = false;
    for (qsizetype i = 0; i < input.size(); ++i) {
        const QChar ch = input.at(i);
        if (ch == u'\\') {
            if (i + 1 >= input.size()) {
                result.error = QStringLiteral("A trailing backslash is incomplete.");
                result.errorOffset = static_cast<int>(i);
                return result;
            }
            const QChar next = input.at(i + 1);
            if (!(next.isSpace() || next == u'\\' || next == u'\'' || next == u'"')) {
                result.error = QStringLiteral("Backslash may escape only whitespace, quotes, or backslash.");
                result.errorOffset = static_cast<int>(i);
                return result;
            }
            token += next;
            tokenStarted = true;
            ++i;
            continue;
        }

        if (quote == Quote::None && (ch == u'|' || ch == u';' || ch == u'&'
                                     || ch == u'<' || ch == u'>' || ch == u'$'
                                     || ch == u'`')) {
            result.error = QStringLiteral("Shell operators and expansion syntax are not supported.");
            result.errorOffset = static_cast<int>(i);
            return result;
        }

        if (quote == Quote::None && ch.isSpace()) {
            if (tokenStarted) {
                result.tokens.push_back(token);
                if (result.tokens.size() > MaximumTokenCount) {
                    result.error = QStringLiteral("Too many arguments (maximum 64 tokens).");
                    result.tokens.clear();
                    result.errorOffset = static_cast<int>(i);
                    return result;
                }
                token.clear();
                tokenStarted = false;
            }
            continue;
        }
        if (quote == Quote::None && ch == u'\'') {
            quote = Quote::Single;
            tokenStarted = true;
        } else if (quote == Quote::None && ch == u'"') {
            quote = Quote::Double;
            tokenStarted = true;
        } else if (quote == Quote::Single && ch == u'\'') {
            quote = Quote::None;
        } else if (quote == Quote::Double && ch == u'"') {
            quote = Quote::None;
        } else {
            token += ch;
            tokenStarted = true;
        }
    }

    if (quote != Quote::None) {
        result.error = quote == Quote::Single
            ? QStringLiteral("Unmatched single quote.")
            : QStringLiteral("Unmatched double quote.");
        result.errorOffset = static_cast<int>(input.size());
        result.tokens.clear();
        return result;
    }
    if (tokenStarted) result.tokens.push_back(token);
    if (result.tokens.size() > MaximumTokenCount) {
        result.error = QStringLiteral("Too many arguments (maximum 64 tokens).");
        result.errorOffset = static_cast<int>(input.size());
        result.tokens.clear();
    }
    return result;
}

} // namespace something::application

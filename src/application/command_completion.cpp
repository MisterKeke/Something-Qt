#include "command_completion.h"

#include "command_parser.h"

#include <QRegularExpression>
#include <QVector>
#include <algorithm>
#include <utility>

namespace something::application {
namespace {

int editDistanceAtMostTwo(const QString &a, const QString &b)
{
    if (qAbs(a.size() - b.size()) > 2) return 3;
    QVector<int> row(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j) row[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        int diagonal = row[0];
        row[0] = i;
        int minimum = row[0];
        for (int j = 1; j <= b.size(); ++j) {
            const int above = row[j];
            row[j] = qMin(qMin(row[j] + 1, row[j - 1] + 1),
                          diagonal + (a.at(i - 1).toLower() == b.at(j - 1).toLower() ? 0 : 1));
            diagonal = above;
            minimum = qMin(minimum, row[j]);
        }
        if (minimum > 2) return 3;
    }
    return row[b.size()];
}

QString encodeToken(const QString &value, QChar quote)
{
    if (quote == u'\'' || quote == u'"') {
        QString escaped;
        for (QChar ch : value) {
            if (ch == u'\\' || ch == quote) escaped += u'\\';
            escaped += ch;
        }
        return QString(quote) + escaped + quote;
    }
    if (value.contains(QRegularExpression(QStringLiteral("\\s")))
        || value.contains(u'\'') || value.contains(u'"')) {
        QString escaped = value;
        escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
        escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
        return QStringLiteral("\"") + escaped + QStringLiteral("\"");
    }
    return value;
}

} // namespace

QVariantMap CommandCompletion::complete(const CommandRegistry &registry,
                                        const QString &input, int cursorPosition)
{
    QVariantMap result{{QStringLiteral("start"), cursorPosition},
                       {QStringLiteral("length"), 0},
                       {QStringLiteral("suggestions"), QVariantList{}}};
    if (input.size() > CommandParser::MaximumInputLength || cursorPosition != input.size())
        return result;

    int tokenStart = 0;
    QChar quote;
    bool escaped = false;
    for (int i = 0; i < cursorPosition; ++i) {
        const QChar ch = input.at(i);
        if (escaped) { escaped = false; continue; }
        if (ch == u'\\') { escaped = true; continue; }
        if (!quote.isNull()) {
            if (ch == quote) quote = {};
            continue;
        }
        if (ch == u'\'' || ch == u'"') { quote = ch; continue; }
        if (ch.isSpace()) tokenStart = i + 1;
    }
    const QString preceding = input.left(tokenStart);
    const auto parsed = CommandParser::parse(preceding);
    if (!parsed.succeeded() || parsed.tokens.size() > CommandParser::MaximumTokenCount)
        return result;

    const QString raw = input.mid(tokenStart);
    QString normalizedPartial;
    QChar activeQuote;
    QChar currentQuote;
    bool currentEscape = false;
    for (QChar ch : raw) {
        if (currentEscape) {
            normalizedPartial += ch;
            currentEscape = false;
        } else if (ch == u'\\') {
            currentEscape = true;
        } else if (!currentQuote.isNull() && ch == currentQuote) {
            currentQuote = {};
        } else if (currentQuote.isNull() && (ch == u'\'' || ch == u'"')) {
            if (activeQuote.isNull()) activeQuote = ch;
            currentQuote = ch;
        } else {
            normalizedPartial += ch;
        }
    }
    if (currentEscape) normalizedPartial += u'\\';

    QStringList candidates;
    if (parsed.tokens.isEmpty()) {
        for (const QString &name : registry.names())
            if (name.startsWith(normalizedPartial, Qt::CaseInsensitive)) candidates.push_back(name);
        // Typo suggestions are suggestions only; selection is always explicit.
        if (candidates.isEmpty() && !normalizedPartial.isEmpty()) {
            for (const QString &name : registry.names())
                if (editDistanceAtMostTwo(normalizedPartial, name) <= 2) candidates.push_back(name);
        }
    } else {
        const auto *definition = registry.find(parsed.tokens.constFirst());
        if (!definition || !definition->completeArguments) return result;
        const QStringList completeArgs = parsed.tokens.mid(1);
        candidates = definition->completeArguments(completeArgs, normalizedPartial);
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
            [&normalizedPartial](const QString &candidate) {
                return !candidate.startsWith(normalizedPartial, Qt::CaseInsensitive);
            }), candidates.end());
    }

    QVariantList suggestions;
    for (const QString &candidate : std::as_const(candidates)) {
        suggestions.push_back(QVariantMap{
            {QStringLiteral("text"), candidate},
            {QStringLiteral("insert"), encodeToken(candidate, activeQuote)},
            {QStringLiteral("start"), tokenStart},
            {QStringLiteral("length"), raw.size()},
        });
    }
    result.insert(QStringLiteral("start"), tokenStart);
    result.insert(QStringLiteral("length"), raw.size());
    result.insert(QStringLiteral("suggestions"), suggestions);
    return result;
}

QString CommandCompletion::apply(const QString &input, const QVariantMap &suggestion,
                                 int *newCursorPosition)
{
    const int start = suggestion.value(QStringLiteral("start")).toInt();
    const int length = suggestion.value(QStringLiteral("length")).toInt();
    const QString inserted = suggestion.value(QStringLiteral("insert")).toString();
    if (start < 0 || length < 0 || start + length > input.size()) return input;
    const QString result = input.left(start) + inserted + input.mid(start + length);
    if (newCursorPosition) *newCursorPosition = start + inserted.size();
    return result;
}

} // namespace something::application

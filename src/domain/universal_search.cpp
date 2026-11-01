#include "universal_search.h"

#include <QRegularExpression>

namespace something::domain {
namespace {

bool allTermsFound(const QStringList &queryTerms, const QStringList &fields)
{
    for (const QString &term : queryTerms) {
        bool found = false;
        for (const QString &field : fields) {
            if (UniversalSearch::normalized(field).contains(term)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

QStringList combined(const QStringList &first, const QStringList &second)
{
    QStringList result = first;
    result.append(second);
    return result;
}

int titleScore(const QString &query, const QString &title)
{
    const QString foldedQuery = UniversalSearch::normalized(query);
    const QString foldedTitle = UniversalSearch::normalized(title);
    if (foldedQuery.isEmpty() || foldedTitle.isEmpty()) return 0;
    if (foldedTitle == foldedQuery) return 10000;
    if (foldedTitle.startsWith(foldedQuery)) return 8000;
    if (foldedTitle.contains(foldedQuery)) return 6500;
    const QStringList queryTerms = UniversalSearch::terms(foldedQuery);
    if (!queryTerms.isEmpty() && allTermsFound(queryTerms, {foldedTitle})) return 5200;
    return 0;
}

} // namespace

QString UniversalSearch::normalized(const QString &text)
{
    return text.normalized(QString::NormalizationForm_C).toCaseFolded()
        .normalized(QString::NormalizationForm_C).trimmed();
}

QStringList UniversalSearch::terms(const QString &query)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    QStringList result;
    const QString trimmed = normalized(query);
    if (trimmed.isEmpty()) return result;
    for (const QString &term : trimmed.split(whitespace, Qt::SkipEmptyParts)) {
        if (!term.isEmpty()) result.push_back(term);
    }
    return result;
}

bool UniversalSearch::matches(const QString &query, const QStringList &titleFields,
                              const QStringList &categoryFields,
                              const QStringList &descriptionFields)
{
    const QStringList queryTerms = terms(query);
    return !queryTerms.isEmpty()
        && allTermsFound(queryTerms, combined(combined(titleFields, categoryFields),
                                              descriptionFields));
}

bool UniversalSearch::acceptsGroup(const QString &resultGroup, const QString &selectedGroup)
{
    return selectedGroup.isEmpty() || selectedGroup == QStringLiteral("all")
        || resultGroup == selectedGroup;
}

int UniversalSearch::score(const QString &query, const QStringList &titleFields,
                           const QStringList &categoryFields,
                           const QStringList &descriptionFields)
{
    const QString foldedQuery = normalized(query);
    const QStringList queryTerms = terms(foldedQuery);
    if (queryTerms.isEmpty()) return 0;

    int bestTitleScore = 0;
    for (const QString &title : titleFields)
        bestTitleScore = qMax(bestTitleScore, titleScore(foldedQuery, title));
    if (bestTitleScore > 0) return bestTitleScore;
    if (allTermsFound(queryTerms, titleFields)) return 5200;
    if (allTermsFound(queryTerms, categoryFields)) return 3600;
    if (allTermsFound(queryTerms, combined(titleFields, categoryFields))) return 4200;
    if (allTermsFound(queryTerms, descriptionFields)) return 2200;
    if (allTermsFound(queryTerms, combined(titleFields, descriptionFields))) return 2800;
    return 0;
}

bool UniversalSearch::compare(const QVariantMap &left, const QVariantMap &right)
{
    const int leftScore = left.value(QStringLiteral("score")).toInt();
    const int rightScore = right.value(QStringLiteral("score")).toInt();
    if (leftScore != rightScore) return leftScore > rightScore;
    const int titleOrder = QString::compare(left.value(QStringLiteral("title")).toString(),
                                            right.value(QStringLiteral("title")).toString(),
                                            Qt::CaseInsensitive);
    if (titleOrder != 0) return titleOrder < 0;
    return left.value(QStringLiteral("id")).toString()
        < right.value(QStringLiteral("id")).toString();
}

} // namespace something::domain

#pragma once

#include <QVariantMap>
#include <QVector>
#include <QString>
#include <QStringList>

namespace something::domain {

struct UniversalSearchCandidate final {
    QVariantMap result;
    QStringList titleFields;
    QStringList categoryFields;
    QStringList descriptionFields;
};

class UniversalSearch final {
public:
    static constexpr int maximumQueryLength = 200;
    static constexpr int maximumTerms = 8;
    static constexpr int maximumResults = 50;

    static QString normalized(const QString &text);
    static QStringList terms(const QString &query);
    static int score(const QString &query, const QStringList &titleFields,
                     const QStringList &categoryFields = {},
                     const QStringList &descriptionFields = {});
    static bool matches(const QString &query, const QStringList &titleFields,
                        const QStringList &categoryFields = {},
                        const QStringList &descriptionFields = {});
    static bool acceptsGroup(const QString &resultGroup, const QString &selectedGroup);
    static bool compare(const QVariantMap &left, const QVariantMap &right);
};

} // namespace something::domain

#include "application/route_catalog.h"
#include "domain/universal_search.h"

#include <QtTest>

using something::domain::UniversalSearch;

class UniversalSearchDomainTest final : public QObject {
    Q_OBJECT

private slots:
    void ranksExactPrefixPartialMetadataInOrder();
    void matchesCaseInsensitiveUnicodeAndMultipleWords();
    void readsAliasesFromTheRouteCatalog();
    void filtersByResultGroup();
    void sortsEqualScoresDeterministically();
    void retainsAllWordsForValidation();
};

void UniversalSearchDomainTest::ranksExactPrefixPartialMetadataInOrder()
{
    const int exact = UniversalSearch::score(QStringLiteral("Discord"), {QStringLiteral("Discord")});
    const int prefix = UniversalSearch::score(QStringLiteral("disc"), {QStringLiteral("Discord Stable")});
    const int partial = UniversalSearch::score(QStringLiteral("cord"), {QStringLiteral("Discord")});
    const int category = UniversalSearch::score(QStringLiteral("design"), {QStringLiteral("Discord")},
                                                 {QStringLiteral("Design tools")});
    const int description = UniversalSearch::score(QStringLiteral("creative"), {QStringLiteral("Discord")},
                                                    {}, {QStringLiteral("Creative tools")});
    const int descriptionOnly = UniversalSearch::score(QStringLiteral("creative tools"),
        {QStringLiteral("Discord")}, {}, {QStringLiteral("Creative tools")});
    const int titleAndDescription = UniversalSearch::score(QStringLiteral("discord creative"),
        {QStringLiteral("Discord")}, {}, {QStringLiteral("Creative tools")});
    const int separateTitleFields = UniversalSearch::score(QStringLiteral("notepad document"),
        {QStringLiteral("Notepad.exe"), QStringLiteral("Document - Notes")});
    QVERIFY(exact > prefix);
    QVERIFY(prefix > partial);
    QVERIFY(partial > category);
    QVERIFY(category > description);
    QVERIFY(titleAndDescription > descriptionOnly);
    QVERIFY(separateTitleFields > category);
}

void UniversalSearchDomainTest::matchesCaseInsensitiveUnicodeAndMultipleWords()
{
    QVERIFY(UniversalSearch::matches(QStringLiteral("CAFÉ"), {QStringLiteral("Café launch") }));
    QVERIFY(UniversalSearch::matches(QStringLiteral("cafe\u0301"), {QStringLiteral("Café launch") }));
    QVERIFY(UniversalSearch::matches(QStringLiteral("café urgent"),
        {QStringLiteral("Café launch")}, {QStringLiteral("Urgent") }));
    QCOMPARE(UniversalSearch::score(QStringLiteral("café urgent"),
        {QStringLiteral("Café launch")}, {QStringLiteral("Urgent")}), 4200);
    QVERIFY(!UniversalSearch::matches(QStringLiteral("café missing"),
        {QStringLiteral("Café launch")}, {QStringLiteral("Urgent")}));
}

void UniversalSearchDomainTest::readsAliasesFromTheRouteCatalog()
{
    bool foundApps = false;
    bool foundGames = false;
    bool foundSettings = false;
    bool automationRulesIsInPrimaryNavigation = false;
    for (const QVariant &routeValue : something::application::RouteCatalog::routes()) {
        const QVariantMap route = routeValue.toMap();
        QStringList fields{route.value(QStringLiteral("title")).toString(),
            route.value(QStringLiteral("navigationTitle")).toString(),
            route.value(QStringLiteral("id")).toString()};
        for (const QVariant &alias : route.value(QStringLiteral("searchAliases")).toList())
            fields.push_back(alias.toString());
        if (route.value(QStringLiteral("id")).toString() == QStringLiteral("applications"))
            foundApps = UniversalSearch::score(QStringLiteral("apps"), fields) > 0;
        if (route.value(QStringLiteral("id")).toString() == QStringLiteral("steam"))
            foundGames = UniversalSearch::score(QStringLiteral("games"), fields) > 0;
        if (route.value(QStringLiteral("id")).toString() == QStringLiteral("settings"))
            foundSettings = UniversalSearch::score(QStringLiteral("preferences"), fields) > 0;
    }
    for (const QVariant &routeValue : something::application::RouteCatalog::navigationItems()) {
        if (routeValue.toMap().value(QStringLiteral("id")).toString()
            == QStringLiteral("automationRules"))
            automationRulesIsInPrimaryNavigation = true;
    }
    QVERIFY(foundApps);
    QVERIFY(foundGames);
    QVERIFY(foundSettings);
    QVERIFY(automationRulesIsInPrimaryNavigation);
    QVERIFY(something::application::RouteCatalog::contains(QStringLiteral("tasks")));
    QVERIFY(!something::application::RouteCatalog::contains(QStringLiteral("deleted-route")));
}

void UniversalSearchDomainTest::filtersByResultGroup()
{
    QVERIFY(UniversalSearch::acceptsGroup(QStringLiteral("games"), QStringLiteral("all")));
    QVERIFY(UniversalSearch::acceptsGroup(QStringLiteral("games"), QStringLiteral("games")));
    QVERIFY(!UniversalSearch::acceptsGroup(QStringLiteral("tasks"), QStringLiteral("games")));
}

void UniversalSearchDomainTest::sortsEqualScoresDeterministically()
{
    const QVariantMap a{{QStringLiteral("id"), QStringLiteral("task:2")},
                        {QStringLiteral("title"), QStringLiteral("Alpha")},
                        {QStringLiteral("score"), 100}};
    const QVariantMap b{{QStringLiteral("id"), QStringLiteral("task:1")},
                        {QStringLiteral("title"), QStringLiteral("alpha")},
                        {QStringLiteral("score"), 100}};
    QVERIFY(UniversalSearch::compare(b, a));
    QVERIFY(!UniversalSearch::compare(a, b));
}

void UniversalSearchDomainTest::retainsAllWordsForValidation()
{
    QCOMPARE(UniversalSearch::terms(QStringLiteral("one two three four five six seven eight nine")).size(), 9);
}

QTEST_GUILESS_MAIN(UniversalSearchDomainTest)
#include "universal_search_test.moc"

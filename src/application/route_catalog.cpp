#include "route_catalog.h"

#include <QCoreApplication>
#include <QStringList>
#include <utility>

namespace something::application {
namespace {
QVariantMap makeRoute(QString id, const char *title, QString glyph, QString asset, bool navigation,
                      const char *navigationTitle = nullptr, QStringList searchAliases = {})
{
    const QString translatedTitle = QCoreApplication::translate("RouteCatalog", title);
    return {{QStringLiteral("id"), std::move(id)},
            {QStringLiteral("title"), translatedTitle},
            {QStringLiteral("navigationTitle"), navigationTitle
                ? QCoreApplication::translate("RouteCatalog", navigationTitle) : translatedTitle},
            {QStringLiteral("glyph"), std::move(glyph)}, {QStringLiteral("asset"), std::move(asset)},
            {QStringLiteral("searchAliases"), std::move(searchAliases)},
            {QStringLiteral("navigation"), navigation}};
}

const QVariantList &routeTable()
{
    static const QVariantList value{
        makeRoute(QStringLiteral("dashboard"), "Main Page", QStringLiteral("⌂"), {}, false,
                  nullptr, {QStringLiteral("home"), QStringLiteral("dashboard")}),
        makeRoute(QStringLiteral("calendar"), "Calendar", QStringLiteral("▦"), {}, true,
                  nullptr, {QStringLiteral("schedule"), QStringLiteral("events")}),
        makeRoute(QStringLiteral("tasks"), "Tasks", QStringLiteral("✓"), QStringLiteral("qrc:/something/assets/navigation/todo-yelan.png"), true,
                  nullptr, {QStringLiteral("task"), QStringLiteral("todo"), QStringLiteral("todos")}),
        makeRoute(QStringLiteral("telegram"), "Telegram", QStringLiteral("➤"), QStringLiteral("qrc:/something/assets/navigation/telegram-varka.png"), true,
                  nullptr, {QStringLiteral("telegram favorites"), QStringLiteral("channels")}),
        makeRoute(QStringLiteral("youtube"), "YouTube", QStringLiteral("▶"), QStringLiteral("qrc:/something/assets/navigation/youtube-charlotte.png"), true,
                  nullptr, {QStringLiteral("youtube favorites"), QStringLiteral("videos")}),
        makeRoute(QStringLiteral("weather"), "Weather", QStringLiteral("☼"), QStringLiteral("qrc:/something/assets/navigation/weather-lisa.png"), true,
                  nullptr, {QStringLiteral("forecast")}),
        makeRoute(QStringLiteral("currencies"), "Currencies", QStringLiteral("¤"), QStringLiteral("qrc:/something/assets/navigation/currency-zhongli.png"), true,
                  nullptr, {QStringLiteral("currency"), QStringLiteral("exchange rates")}),
        makeRoute(QStringLiteral("applications"), "Saved applications", QStringLiteral("▣"), QStringLiteral("qrc:/something/assets/navigation/apps-furina.png"), true,
                  nullptr, {QStringLiteral("apps"), QStringLiteral("desktop apps"), QStringLiteral("applications")}),
        makeRoute(QStringLiteral("runningApplications"), "Running applications", QStringLiteral("↗"), {}, true, "Running apps",
                  {QStringLiteral("running"), QStringLiteral("processes"), QStringLiteral("open windows")}),
        makeRoute(QStringLiteral("usageStatistics"), "Application Usage Statistics", QStringLiteral("◷"), QStringLiteral("qrc:/something/assets/navigation/apps-furina.png"), true),
        makeRoute(QStringLiteral("setups"), "Setups", QStringLiteral("▱"), QStringLiteral("qrc:/something/assets/navigation/setups-layers.svg"), true,
                  nullptr, {QStringLiteral("setup"), QStringLiteral("launch groups")}),
        makeRoute(QStringLiteral("fileExplorer"), "File Explorer", QStringLiteral("▰"), QStringLiteral("qrc:/something/assets/navigation/file-explorer-aino.png"), true),
        makeRoute(QStringLiteral("gameUpdates"), "Game Updates", QStringLiteral("◉"), QStringLiteral("qrc:/something/assets/navigation/games-controller.svg"), true,
                  nullptr, {QStringLiteral("game updates"), QStringLiteral("game news"), QStringLiteral("updates")}),
        makeRoute(QStringLiteral("steam"), "Steam tracking", QStringLiteral("◉"), QStringLiteral("qrc:/something/assets/navigation/games-controller.svg"), true,
                  nullptr, {QStringLiteral("games"), QStringLiteral("steam"), QStringLiteral("price tracking"),
                            QStringLiteral("release radar"), QStringLiteral("collections")}),
        makeRoute(QStringLiteral("terminal"), "Console", QStringLiteral(">_"), {}, false,
                  nullptr, {QStringLiteral("console"), QStringLiteral("command line")}),
        makeRoute(QStringLiteral("settings"), "Settings", QStringLiteral("⚙"), QStringLiteral("qrc:/something/assets/navigation/settings-sandrone.png"), true,
                  nullptr, {QStringLiteral("preferences"), QStringLiteral("configuration")}),
        makeRoute(QStringLiteral("automationRules"), "Automation Rules", QStringLiteral("↻"), {}, false,
                  nullptr, {QStringLiteral("automations"), QStringLiteral("rules"), QStringLiteral("automation history")}),
    };
    return value;
}
}

QVariantList RouteCatalog::routes() { return routeTable(); }
QVariantList RouteCatalog::navigationItems()
{
    QVariantList result;
    for (const auto &item : routeTable()) {
        auto map = item.toMap();
        if (map.value(QStringLiteral("navigation")).toBool()) {
            map.insert(QStringLiteral("title"), map.value(QStringLiteral("navigationTitle")));
            result.push_back(map);
        }
    }
    return result;
}
QVariantMap RouteCatalog::titleMap()
{
    QVariantMap result;
    for (const auto &item : routeTable()) {
        const auto map = item.toMap();
        result.insert(map.value(QStringLiteral("id")).toString(), map.value(QStringLiteral("title")));
    }
    return result;
}
QString RouteCatalog::titleFor(const QString &routeId)
{
    for (const auto &item : routeTable()) {
        const auto map = item.toMap();
        if (map.value(QStringLiteral("id")).toString() == routeId)
            return map.value(QStringLiteral("title")).toString();
    }
    return {};
}
bool RouteCatalog::contains(const QString &routeId) { return !titleFor(routeId).isEmpty(); }

} // namespace something::application

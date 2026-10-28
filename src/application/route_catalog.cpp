#include "route_catalog.h"

#include <QCoreApplication>
#include <utility>

namespace something::application {
namespace {
QVariantMap makeRoute(QString id, const char *title, QString glyph, QString asset, bool navigation,
                      const char *navigationTitle = nullptr)
{
    const QString translatedTitle = QCoreApplication::translate("RouteCatalog", title);
    return {{QStringLiteral("id"), std::move(id)},
            {QStringLiteral("title"), translatedTitle},
            {QStringLiteral("navigationTitle"), navigationTitle
                ? QCoreApplication::translate("RouteCatalog", navigationTitle) : translatedTitle},
            {QStringLiteral("glyph"), std::move(glyph)}, {QStringLiteral("asset"), std::move(asset)},
            {QStringLiteral("navigation"), navigation}};
}

const QVariantList &routeTable()
{
    static const QVariantList value{
        makeRoute(QStringLiteral("dashboard"), "Main Page", QStringLiteral("⌂"), {}, false),
        makeRoute(QStringLiteral("calendar"), "Calendar", QStringLiteral("▦"), {}, true),
        makeRoute(QStringLiteral("tasks"), "Tasks", QStringLiteral("✓"), QStringLiteral("qrc:/something/assets/navigation/todo-yelan.png"), true),
        makeRoute(QStringLiteral("telegram"), "Telegram", QStringLiteral("➤"), QStringLiteral("qrc:/something/assets/navigation/telegram-varka.png"), true),
        makeRoute(QStringLiteral("youtube"), "YouTube", QStringLiteral("▶"), QStringLiteral("qrc:/something/assets/navigation/youtube-charlotte.png"), true),
        makeRoute(QStringLiteral("weather"), "Weather", QStringLiteral("☼"), QStringLiteral("qrc:/something/assets/navigation/weather-lisa.png"), true),
        makeRoute(QStringLiteral("currencies"), "Currencies", QStringLiteral("¤"), QStringLiteral("qrc:/something/assets/navigation/currency-zhongli.png"), true),
        makeRoute(QStringLiteral("applications"), "Saved applications", QStringLiteral("▣"), QStringLiteral("qrc:/something/assets/navigation/apps-furina.png"), true),
        makeRoute(QStringLiteral("runningApplications"), "Running applications", QStringLiteral("↗"), {}, true, "Running apps"),
        makeRoute(QStringLiteral("setups"), "Setups", QStringLiteral("▱"), QStringLiteral("qrc:/something/assets/navigation/setups-layers.svg"), true),
        makeRoute(QStringLiteral("fileExplorer"), "File Explorer", QStringLiteral("▰"), QStringLiteral("qrc:/something/assets/navigation/file-explorer-aino.png"), true),
        makeRoute(QStringLiteral("steam"), "Steam tracking", QStringLiteral("◉"), QStringLiteral("qrc:/something/assets/navigation/games-controller.svg"), true),
        makeRoute(QStringLiteral("terminal"), "Console", QStringLiteral(">_"), {}, true),
        makeRoute(QStringLiteral("settings"), "Settings", QStringLiteral("⚙"), QStringLiteral("qrc:/something/assets/navigation/settings-sandrone.png"), true),
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

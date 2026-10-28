#pragma once

#include <QVariantList>
#include <QVariantMap>
#include <QString>

namespace something::application {

// Single authoritative shell route table shared by navigation and console.
class RouteCatalog final {
public:
    static QVariantList routes();
    static QVariantList navigationItems();
    static QVariantMap titleMap();
    static QString titleFor(const QString &routeId);
    static bool contains(const QString &routeId);
};

} // namespace something::application

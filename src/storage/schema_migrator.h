#pragma once

#include <QSqlDatabase>
#include <QString>

namespace something::storage {

class SchemaMigrator final {
public:
    // Applies this application's independent, sequential schema history.
    // The caller must own and use database on its current thread.
    static bool apply(QSqlDatabase &database, QString *diagnostic = nullptr);
    static int latestVersion() noexcept;
};

} // namespace something::storage

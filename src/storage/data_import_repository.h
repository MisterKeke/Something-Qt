#pragma once

#include "../domain/app_error.h"

#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

#include <functional>

namespace something::storage {

struct DataImportStatus final {
    bool empty{false};
    bool completed{false};
    QString completedAt;
    QString reportJson;
    QString blockingReason;
};

class DataImportRepository final {
public:
    explicit DataImportRepository(QSqlDatabase &database) : m_database(database) {}

    domain::AppError status(DataImportStatus *result) const;
    domain::AppError hasCompletion(const QString &importId, bool *completed) const;
    domain::AppError importPlan(const QString &planPath, const QString &importId,
                                QVariantMap *report,
                                const std::function<bool()> &cancellationRequested = {});

private:
    bool targetIsEmpty(bool *empty, QString *blockingReason = nullptr) const;
    QSqlDatabase &m_database;
};

} // namespace something::storage

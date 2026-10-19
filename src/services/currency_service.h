#pragma once

#include "../storage/storage_worker.h"

namespace something::services {

class CurrencyService final {
public:
    static storage::StorageCommand listFavorites();
    static storage::StorageCommand addFavorite(QString baseCode, QString quoteCode);
    static storage::StorageCommand removeFavorite(QString baseCode, QString quoteCode);
};

} // namespace something::services

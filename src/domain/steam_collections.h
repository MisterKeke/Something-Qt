#pragma once

#include "app_error.h"

#include <QString>
#include <QtGlobal>

#include <optional>

namespace something::domain {

enum class SteamCollectionGameStatus {
    Backlog,
    Playing,
    Completed,
    Paused,
    Dropped,
};

struct SteamCollectionInput final {
    QString name;
    QString description;
    QString color; // Optional #rgb or #rrggbb value.
    QString icon;  // Optional short Unicode glyph.
};

struct SteamCollection final {
    qint64 id{0};
    QString name;
    QString description;
    QString color;
    QString icon;
    int displayOrder{0};
    QString createdAt;
    QString updatedAt;
};

struct SteamCollectionGameInput final {
    quint32 steamAppId{0};
    QString displayName;
    // A managed Steam artwork filename. Empty when no cached artwork is set.
    QString artworkReference;
    std::optional<SteamCollectionGameStatus> status;
    std::optional<int> personalRating; // Optional five-point rating (1–5).
    QString personalComment;
};

// A collection membership joined with common catalog display data. The
// (collectionId, steamAppId) pair is its stable membership identity.
struct SteamCollectionGameEntry final {
    qint64 collectionId{0};
    quint32 steamAppId{0};
    QString displayName;
    QString artworkReference;
    std::optional<SteamCollectionGameStatus> status;
    std::optional<int> personalRating;
    QString personalComment;
    QString createdAt;
    QString updatedAt;
};

QString steamCollectionGameStatusName(SteamCollectionGameStatus status);
std::optional<SteamCollectionGameStatus> steamCollectionGameStatusFromName(
    const QString &name);

// Stable comparison key for unique collection names. Display names retain
// their casing; the key is trimmed, canonically composed, and case-folded.
QString steamCollectionNameKey(const QString &name);

AppError normalizeSteamCollectionInput(SteamCollectionInput &input);
AppError normalizeSteamCollectionGameInput(SteamCollectionGameInput &input);

bool isValidSteamCollection(const SteamCollection &collection);
bool isValidSteamCollectionGameEntry(const SteamCollectionGameEntry &entry);

} // namespace something::domain

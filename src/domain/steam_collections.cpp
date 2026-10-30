#include "steam_collections.h"

#include <QRegularExpression>

#include <utility>

namespace something::domain {
namespace {

constexpr int maximumCollectionNameCharacters = 80;
constexpr int maximumCollectionDescriptionCharacters = 2000;
constexpr int maximumCollectionIconCharacters = 16;
constexpr int maximumPersonalCommentCharacters = 4000;
constexpr int minimumPersonalRating = 1;
constexpr int maximumPersonalRating = 5;

int characterCount(const QString &value)
{
    return value.toUcs4().size();
}

bool normalizeCollectionColor(QString value, QString *normalized)
{
    value = value.trimmed().toLower();
    if (value.isEmpty()) {
        *normalized = QStringLiteral("");
        return true;
    }

    static const QRegularExpression shortHex(QStringLiteral("^#[0-9a-f]{3}$"));
    static const QRegularExpression fullHex(QStringLiteral("^#[0-9a-f]{6}$"));
    if (shortHex.match(value).hasMatch()) {
        *normalized = QStringLiteral("#") + value[1] + value[1] + value[2] + value[2]
            + value[3] + value[3];
        return true;
    }
    if (!fullHex.match(value).hasMatch()) return false;
    *normalized = std::move(value);
    return true;
}

bool isSupportedStatus(SteamCollectionGameStatus status)
{
    switch (status) {
    case SteamCollectionGameStatus::Backlog:
    case SteamCollectionGameStatus::Playing:
    case SteamCollectionGameStatus::Completed:
    case SteamCollectionGameStatus::Paused:
    case SteamCollectionGameStatus::Dropped:
        return true;
    }
    return false;
}

bool validIconText(const QString &icon)
{
    for (const QChar character : icon) {
        if (character.isNull() || character.isSpace()
            || character.category() == QChar::Other_Control) {
            return false;
        }
    }
    return true;
}

} // namespace

QString steamCollectionGameStatusName(SteamCollectionGameStatus status)
{
    switch (status) {
    case SteamCollectionGameStatus::Backlog: return QStringLiteral("backlog");
    case SteamCollectionGameStatus::Playing: return QStringLiteral("playing");
    case SteamCollectionGameStatus::Completed: return QStringLiteral("completed");
    case SteamCollectionGameStatus::Paused: return QStringLiteral("paused");
    case SteamCollectionGameStatus::Dropped: return QStringLiteral("dropped");
    }
    return {};
}

std::optional<SteamCollectionGameStatus> steamCollectionGameStatusFromName(
    const QString &name)
{
    if (name == QStringLiteral("backlog")) return SteamCollectionGameStatus::Backlog;
    if (name == QStringLiteral("playing")) return SteamCollectionGameStatus::Playing;
    if (name == QStringLiteral("completed")) return SteamCollectionGameStatus::Completed;
    if (name == QStringLiteral("paused")) return SteamCollectionGameStatus::Paused;
    if (name == QStringLiteral("dropped")) return SteamCollectionGameStatus::Dropped;
    return std::nullopt;
}

QString steamCollectionNameKey(const QString &name)
{
    return name.trimmed().normalized(QString::NormalizationForm_C).toCaseFolded();
}

AppError normalizeSteamCollectionInput(SteamCollectionInput &input)
{
    input.name = input.name.trimmed().normalized(QString::NormalizationForm_C);
    input.description = input.description.trimmed().normalized(QString::NormalizationForm_C);
    input.icon = input.icon.trimmed().normalized(QString::NormalizationForm_C);
    if (input.description.isNull()) input.description = QStringLiteral("");
    if (input.icon.isNull()) input.icon = QStringLiteral("");

    QString normalizedColor;
    if (input.name.isEmpty() || characterCount(input.name) > maximumCollectionNameCharacters
        || characterCount(input.description) > maximumCollectionDescriptionCharacters
        || characterCount(input.icon) > maximumCollectionIconCharacters
        || !validIconText(input.icon)
        || !normalizeCollectionColor(input.color, &normalizedColor)) {
        return AppError(ErrorCategory::Validation);
    }
    input.color = std::move(normalizedColor);
    return AppError::none();
}

AppError normalizeSteamCollectionGameInput(SteamCollectionGameInput &input)
{
    input.personalComment = input.personalComment.trimmed()
        .normalized(QString::NormalizationForm_C);
    if (input.personalComment.isNull()) input.personalComment = QStringLiteral("");

    if (input.steamAppId == 0
        || characterCount(input.personalComment) > maximumPersonalCommentCharacters
        || (input.personalRating
            && (*input.personalRating < minimumPersonalRating
                || *input.personalRating > maximumPersonalRating))
        || (input.status && !isSupportedStatus(*input.status))) {
        return AppError(ErrorCategory::Validation);
    }
    return AppError::none();
}

bool isValidSteamCollection(const SteamCollection &collection)
{
    if (collection.id <= 0 || collection.displayOrder < 0
        || collection.createdAt.trimmed().isEmpty()
        || collection.updatedAt.trimmed().isEmpty()) {
        return false;
    }

    SteamCollectionInput input{collection.name, collection.description,
                               collection.color, collection.icon};
    if (normalizeSteamCollectionInput(input).isError()) return false;
    return input.name == collection.name && input.description == collection.description
        && input.color == collection.color && input.icon == collection.icon;
}

bool isValidSteamCollectionGameEntry(const SteamCollectionGameEntry &entry)
{
    if (entry.collectionId <= 0 || entry.createdAt.trimmed().isEmpty()
        || entry.updatedAt.trimmed().isEmpty() || !isValidSteamGameCatalogEntry(entry.game)) {
        return false;
    }

    SteamCollectionGameInput input{entry.game.appId, entry.status,
                                   entry.personalRating, entry.personalComment};
    if (normalizeSteamCollectionGameInput(input).isError()) return false;
    return input.personalComment == entry.personalComment;
}

} // namespace something::domain

#pragma once

#include "../domain/app_error.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QVector>

namespace something::providers {

enum class JsonFieldState { Missing, Present, WrongType };

template<typename T>
struct JsonField final {
    JsonFieldState state{JsonFieldState::Missing};
    T value{};

    bool isPresent() const noexcept { return state == JsonFieldState::Present; }
    bool isMissing() const noexcept { return state == JsonFieldState::Missing; }
    bool hasWrongType() const noexcept { return state == JsonFieldState::WrongType; }
};

// A JSON object boundary with typed field access. Missing optional fields stay
// distinguishable from fields that exist with an incompatible JSON type.
class ProviderJsonObject final {
public:
    static bool parse(const QByteArray &body,
                      ProviderJsonObject *result,
                      something::domain::AppError *error = nullptr);

    bool contains(const QString &key) const { return m_object.contains(key); }
    JsonField<QString> string(const QString &key) const;
    JsonField<double> number(const QString &key) const;
    JsonField<bool> boolean(const QString &key) const;
    JsonField<QJsonObject> object(const QString &key) const;
    JsonField<QJsonArray> array(const QString &key) const;

private:
    QJsonObject m_object;
};

struct TelegramPost final {
    QString text;
    QStringList images;
    QString date;
    QString views;
    QString postId;
};

struct YouTubeVideo final {
    QString videoId;
    QString title;
    QString description;
    QString thumbnail;
    QString publishedAt;
    QString channelId;
    QString channelTitle;
    QString videoUrl;
    QString views;
};

template<typename T>
struct ProviderParseResult final {
    QVector<T> items;
    something::domain::AppError error;

    bool succeeded() const noexcept { return error.category() == something::domain::ErrorCategory::None; }
    bool isLegitimateEmpty() const noexcept { return succeeded() && items.isEmpty(); }
};

ProviderParseResult<TelegramPost> parseTelegramPosts(const QByteArray &body);
ProviderParseResult<YouTubeVideo> parseYouTubeFeed(const QByteArray &body,
                                                   const QString &channelId);
QString parseYouTubeChannelIdPage(const QByteArray &body,
                                  something::domain::AppError *error = nullptr);

} // namespace something::providers

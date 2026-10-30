#pragma once

#include <QtGlobal>
#include <QString>

#include <optional>

namespace something::domain {

// Source metadata is retained verbatim. Callers must not infer that a news
// item is a patch, announcement, or event unless Steam supplies reliable data.
struct SteamNewsArticle final {
    quint32 appId{0};
    QString newsId;
    QString title;
    qint64 publishedAtUnixSeconds{0};
    QString sourceUrl;
    QString author;
    QString feedLabel;
    QString feedName;
    std::optional<int> feedType;
    QString contentText;
    bool externalUrl{false};
};

} // namespace something::domain

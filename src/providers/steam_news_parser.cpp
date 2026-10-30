#include "feature_parsers.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/html/html.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace something::providers {
namespace {

using something::domain::AppError;
using something::domain::ErrorCategory;
using something::domain::SteamNewsArticle;

constexpr qsizetype kMaximumProviderPayloadBytes = 2 * 1024 * 1024;
constexpr qsizetype kMaximumArticleHtmlBytes = 256 * 1024;
constexpr qsizetype kMaximumPlainTextCharacters = 12000;
constexpr qsizetype kMaximumNewsItemsPerPage = 20;
constexpr qsizetype kMaximumTitleCharacters = 500;
constexpr qsizetype kMaximumNewsIdCharacters = 32;

AppError invalidResponse()
{
    return AppError(ErrorCategory::InvalidResponse);
}

bool integerField(const QJsonObject &object, const QString &key, qint64 *value)
{
    const QJsonValue field = object.value(key);
    if (!field.isDouble()) return false;
    const double number = field.toDouble();
    // Steam app IDs, result counts, and Unix timestamps are all far below
    // this bound, which also keeps the double-to-integer conversion exact.
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < 0.0 || number > 253402300799.0) {
        return false;
    }
    if (value) *value = static_cast<qint64>(number);
    return true;
}

bool requiredString(const QJsonObject &object, const QString &key, QString *value)
{
    const QJsonValue field = object.value(key);
    if (!field.isString()) return false;
    if (value) *value = field.toString();
    return true;
}

bool optionalString(const QJsonObject &object, const QString &key, QString *value)
{
    const QJsonValue field = object.value(key);
    if (field.isUndefined() || field.isNull()) {
        if (value) value->clear();
        return true;
    }
    if (!field.isString()) return false;
    if (value) *value = field.toString();
    return true;
}

bool safeSourceUrl(const QString &raw, QString *normalized)
{
    if (raw.isEmpty() || raw.toUtf8().size() > 4096) return false;
    const QUrl url(raw, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QStringLiteral("https") &&
                           scheme != QStringLiteral("http")) ||
        url.host().isEmpty() || !url.userInfo().isEmpty() || !url.fragment().isEmpty() ||
        (url.port(-1) != -1 && url.port(-1) != (scheme == QStringLiteral("https") ? 443 : 80))) {
        return false;
    }
    if (normalized) *normalized = url.toString(QUrl::FullyEncoded);
    return true;
}

bool isSkippedElement(const QString &name)
{
    static const QSet<QString> skipped{
        QStringLiteral("script"), QStringLiteral("style"), QStringLiteral("template"),
        QStringLiteral("iframe"), QStringLiteral("object"), QStringLiteral("embed"),
        QStringLiteral("svg"), QStringLiteral("math"), QStringLiteral("noscript")};
    return skipped.contains(name.toLower());
}

bool isBlockElement(const QString &name)
{
    static const QSet<QString> blocks{
        QStringLiteral("address"), QStringLiteral("article"), QStringLiteral("blockquote"),
        QStringLiteral("br"), QStringLiteral("div"), QStringLiteral("dl"),
        QStringLiteral("footer"), QStringLiteral("h1"), QStringLiteral("h2"),
        QStringLiteral("h3"), QStringLiteral("h4"), QStringLiteral("h5"),
        QStringLiteral("h6"), QStringLiteral("header"), QStringLiteral("hr"),
        QStringLiteral("li"), QStringLiteral("ol"), QStringLiteral("p"),
        QStringLiteral("section"), QStringLiteral("table"), QStringLiteral("tr"),
        QStringLiteral("ul")};
    return blocks.contains(name.toLower());
}

QString elementName(lxb_dom_node_t *node)
{
    size_t length = 0;
    const lxb_char_t *name = lxb_dom_element_local_name(
        lxb_dom_interface_element(node), &length);
    return name && length > 0
        ? QString::fromUtf8(reinterpret_cast<const char *>(name), qsizetype(length))
        : QString();
}

struct PendingNode final {
    lxb_dom_node_t *node{nullptr};
    bool closeBlock{false};
};

void appendNodeText(lxb_dom_node_t *root, QString *output)
{
    if (!root || !output || output->size() >= kMaximumArticleHtmlBytes) return;

    QVector<PendingNode> pending{{root, false}};
    while (!pending.isEmpty() && output->size() < kMaximumArticleHtmlBytes) {
        const PendingNode pendingNode = pending.takeLast();
        lxb_dom_node_t *node = pendingNode.node;
        if (pendingNode.closeBlock) {
            output->append(QLatin1Char('\n'));
            continue;
        }
        if (node->next) pending.push_back({node->next, false});
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            const QString name = elementName(node);
            if (isSkippedElement(name)) continue;
            const bool block = isBlockElement(name);
            if (block) {
                output->append(QLatin1Char('\n'));
                pending.push_back({node, true});
            }
        } else if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            lxb_char_t *text = lxb_dom_node_text_content(node, &length);
            if (text && length > 0) {
                const qsizetype remaining = kMaximumArticleHtmlBytes - output->size();
                output->append(QString::fromUtf8(
                    reinterpret_cast<const char *>(text), qsizetype(std::min<size_t>(
                        length, size_t(remaining)))));
            }
        }
        if (node->first_child) pending.push_back({node->first_child, false});
    }
}

QString plainTextFromHtml(const QString &html, bool *valid)
{
    if (valid) *valid = false;
    const QByteArray bytes = html.toUtf8();
    if (bytes.size() > kMaximumArticleHtmlBytes) return {};

    lxb_html_parser_t *parser = lxb_html_parser_create();
    if (!parser || lxb_html_parser_init(parser) != LXB_STATUS_OK) {
        if (parser) lxb_html_parser_destroy(parser);
        return {};
    }
    lxb_html_document_t *document = lxb_html_parse(
        parser, reinterpret_cast<const lxb_char_t *>(bytes.constData()), size_t(bytes.size()));
    lxb_html_parser_destroy(parser);
    if (!document) return {};

    QString rawText;
    appendNodeText(lxb_dom_interface_node(document), &rawText);
    lxb_html_document_destroy(document);

    QStringList lines;
    const QStringList rawLines = rawText.split(QLatin1Char('\n'));
    for (QString line : rawLines) {
        line = line.simplified();
        if (line.isEmpty()) continue;
        for (qsizetype i = 0; i < line.size(); ++i) {
            const QChar ch = line.at(i);
            const ushort code = ch.unicode();
            if ((code >= 0x202a && code <= 0x202e) || (code >= 0x2066 && code <= 0x2069))
                line[i] = QChar(0xfffd);
        }
        lines.push_back(std::move(line));
    }
    QString result = lines.join(QStringLiteral("\n\n")).trimmed();
    if (result.size() > kMaximumPlainTextCharacters) {
        result.truncate(kMaximumPlainTextCharacters);
        result = result.trimmed();
    }
    if (valid) *valid = true;
    return result;
}

bool parseArticle(const QJsonObject &object, quint32 expectedAppId,
                  SteamNewsArticle *article)
{
    if (!article) return false;
    SteamNewsArticle parsed;
    qint64 value = 0;
    if (!requiredString(object, QStringLiteral("gid"), &parsed.newsId) ||
        parsed.newsId.isEmpty() || parsed.newsId.size() > kMaximumNewsIdCharacters) {
        return false;
    }
    for (const QChar ch : parsed.newsId) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
    }

    if (!integerField(object, QStringLiteral("appid"), &value) ||
        value != qint64(expectedAppId)) return false;
    parsed.appId = expectedAppId;

    if (!requiredString(object, QStringLiteral("title"), &parsed.title)) return false;
    parsed.title = parsed.title.trimmed();
    if (parsed.title.isEmpty() || parsed.title.size() > kMaximumTitleCharacters) return false;

    if (!integerField(object, QStringLiteral("date"), &parsed.publishedAtUnixSeconds) ||
        parsed.publishedAtUnixSeconds <= 0) return false;

    QString rawUrl;
    if (!requiredString(object, QStringLiteral("url"), &rawUrl) ||
        !safeSourceUrl(rawUrl, &parsed.sourceUrl)) return false;

    QString contents;
    if (!requiredString(object, QStringLiteral("contents"), &contents) ||
        contents.toUtf8().size() > kMaximumArticleHtmlBytes) return false;
    bool contentValid = false;
    parsed.contentText = plainTextFromHtml(contents, &contentValid);
    if (!contentValid) return false;

    if (!optionalString(object, QStringLiteral("author"), &parsed.author) ||
        !optionalString(object, QStringLiteral("feedlabel"), &parsed.feedLabel) ||
        !optionalString(object, QStringLiteral("feedname"), &parsed.feedName)) return false;
    if (parsed.author.size() > 500 || parsed.feedLabel.size() > 200 ||
        parsed.feedName.size() > 200) return false;

    const QJsonValue external = object.value(QStringLiteral("is_external_url"));
    if (!external.isUndefined() && !external.isNull()) {
        if (!external.isBool()) return false;
        parsed.externalUrl = external.toBool();
    }

    const QJsonValue feedType = object.value(QStringLiteral("feed_type"));
    if (!feedType.isUndefined() && !feedType.isNull()) {
        if (!feedType.isDouble() || std::floor(feedType.toDouble()) != feedType.toDouble() ||
            feedType.toDouble() < 0.0 || feedType.toDouble() > double(std::numeric_limits<int>::max())) {
            return false;
        }
        parsed.feedType = static_cast<int>(feedType.toDouble());
    }

    *article = std::move(parsed);
    return true;
}

} // namespace

FeatureParseResult<QVector<SteamNewsArticle>> parseSteamNewsArticles(
    const QByteArray &body, quint32 expectedAppId)
{
    FeatureParseResult<QVector<SteamNewsArticle>> result;
    if (expectedAppId == 0 || body.isEmpty() || body.size() > kMaximumProviderPayloadBytes) {
        result.error = invalidResponse();
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = invalidResponse();
        return result;
    }
    const QJsonObject appNews = document.object().value(QStringLiteral("appnews")).toObject();
    qint64 responseAppId = 0;
    qint64 count = 0;
    const QJsonValue itemsValue = appNews.value(QStringLiteral("newsitems"));
    if (appNews.isEmpty() || !integerField(appNews, QStringLiteral("appid"), &responseAppId) ||
        responseAppId != qint64(expectedAppId) ||
        !integerField(appNews, QStringLiteral("count"), &count) ||
        !itemsValue.isArray() || count < 0 ||
        itemsValue.toArray().size() > kMaximumNewsItemsPerPage ||
        count < itemsValue.toArray().size()) {
        result.error = invalidResponse();
        return result;
    }

    const QJsonArray items = itemsValue.toArray();
    QSet<QString> seenIds;
    result.value.reserve(items.size());
    for (const QJsonValue &itemValue : items) {
        if (!itemValue.isObject()) {
            result.value.clear();
            result.error = invalidResponse();
            return result;
        }
        SteamNewsArticle article;
        if (!parseArticle(itemValue.toObject(), expectedAppId, &article) ||
            seenIds.contains(article.newsId)) {
            result.value.clear();
            result.error = invalidResponse();
            return result;
        }
        seenIds.insert(article.newsId);
        result.value.push_back(std::move(article));
    }
    return result;
}

} // namespace something::providers

#include "provider_parsers.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QUrl>
#include <QXmlStreamReader>

#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/html/html.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace something::providers {
namespace {

using ErrorCategory = something::domain::ErrorCategory;
constexpr qsizetype kMaximumProviderPayloadBytes = 2 * 1024 * 1024;

something::domain::AppError invalidPayload()
{
    return something::domain::AppError(ErrorCategory::InvalidResponse);
}

bool validYouTubeVideoId(const QString &value)
{
    if (value.size() != 11) {
        return false;
    }
    for (const QChar ch : value) {
        const ushort code = ch.unicode();
        if (!((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z') ||
              (code >= '0' && code <= '9') || ch == QLatin1Char('_') || ch == QLatin1Char('-'))) {
            return false;
        }
    }
    return true;
}

bool validYouTubeChannelId(const QString &value)
{
    if (value.size() != 24 || !value.startsWith(QStringLiteral("UC"))) {
        return false;
    }
    for (const QChar ch : value) {
        const ushort code = ch.unicode();
        if (!((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z') ||
              (code >= '0' && code <= '9') || ch == QLatin1Char('_') || ch == QLatin1Char('-'))) {
            return false;
        }
    }
    return true;
}

QString lexborString(const lxb_char_t *data, size_t length)
{
    if (!data || length == 0) {
        return {};
    }
    return QString::fromUtf8(reinterpret_cast<const char *>(data), qsizetype(length));
}

QByteArray lexborAttribute(lxb_dom_element_t *element, const char *name)
{
    size_t length = 0;
    const auto *value = lxb_dom_element_get_attribute(
        element, reinterpret_cast<const lxb_char_t *>(name), std::char_traits<char>::length(name), &length);
    if (!value || length == 0) {
        return {};
    }
    return QByteArray(reinterpret_cast<const char *>(value), qsizetype(length));
}

QString lexborText(lxb_dom_node_t *node)
{
    size_t length = 0;
    lxb_char_t *value = lxb_dom_node_text_content(node, &length);
    return lexborString(value, length).trimmed();
}

bool hasClass(lxb_dom_element_t *element, const QByteArray &expected)
{
    size_t length = 0;
    const lxb_char_t *value = lxb_dom_element_class(element, &length);
    if (!value || length == 0) {
        return false;
    }
    const QByteArray classes(reinterpret_cast<const char *>(value), qsizetype(length));
    const auto tokens = classes.simplified().split(' ');
    return std::find(tokens.cbegin(), tokens.cend(), expected) != tokens.cend();
}

QVector<lxb_dom_element_t *> descendantsWithClass(lxb_dom_node_t *root,
                                                   const QByteArray &className)
{
    QVector<lxb_dom_element_t *> result;
    for (lxb_dom_node_t *node = root; node;) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            auto *element = lxb_dom_interface_element(node);
            if (hasClass(element, className)) {
                result.push_back(element);
            }
        }
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node && node != root && !node->next) {
            node = node->parent;
        }
        node = !node || node == root ? nullptr : node->next;
    }
    return result;
}

QVector<lxb_dom_element_t *> descendantsWithAttribute(lxb_dom_node_t *root,
                                                       const char *attributeName)
{
    QVector<lxb_dom_element_t *> result;
    const qsizetype nameLength = qsizetype(std::char_traits<char>::length(attributeName));
    for (lxb_dom_node_t *node = root; node;) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            auto *element = lxb_dom_interface_element(node);
            if (lxb_dom_element_has_attribute(element,
                    reinterpret_cast<const lxb_char_t *>(attributeName), size_t(nameLength))) {
                result.push_back(element);
            }
        }
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node && node != root && !node->next) {
            node = node->parent;
        }
        node = !node || node == root ? nullptr : node->next;
    }
    return result;
}

QVector<lxb_dom_element_t *> allElements(lxb_dom_node_t *root)
{
    QVector<lxb_dom_element_t *> result;
    for (lxb_dom_node_t *node = root; node;) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            result.push_back(lxb_dom_interface_element(node));
        }
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node && node != root && !node->next) {
            node = node->parent;
        }
        node = !node || node == root ? nullptr : node->next;
    }
    return result;
}

QString elementName(lxb_dom_element_t *element)
{
    size_t length = 0;
    const lxb_char_t *name = lxb_dom_element_local_name(element, &length);
    return lexborString(name, length);
}

bool isSafeHttpsUrl(const QString &raw, QString *normalized = nullptr)
{
    const QUrl url(raw, QUrl::StrictMode);
    if (!url.isValid() || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 ||
        url.host().isEmpty() || !url.userInfo().isEmpty()) {
        return false;
    }
    if (normalized) {
        *normalized = url.toString(QUrl::FullyEncoded);
    }
    return true;
}

QString cssUrl(const QByteArray &style)
{
    const QByteArray lowered = style.toLower();
    qsizetype position = lowered.indexOf("url(");
    while (position >= 0) {
        qsizetype start = position + 4;
        while (start < style.size() && (style.at(start) == ' ' || style.at(start) == '\t')) {
            ++start;
        }
        char quote = 0;
        if (start < style.size() && (style.at(start) == '\'' || style.at(start) == '"')) {
            quote = style.at(start++);
        }
        qsizetype end = start;
        while (end < style.size()) {
            const char current = style.at(end);
            if ((quote != 0 && current == quote) || (quote == 0 && current == ')')) {
                break;
            }
            if (current == '\\' && end + 1 < style.size()) {
                end += 2;
            } else {
                ++end;
            }
        }
        const QString candidate = QString::fromUtf8(style.mid(start, end - start)).trimmed();
        QString safe;
        if (isSafeHttpsUrl(candidate, &safe)) {
            return safe;
        }
        position = lowered.indexOf("url(", end);
    }
    return {};
}

QString postIdFromHref(const QString &href)
{
    const QUrl url(href, QUrl::StrictMode);
    if (!url.isValid() || (!url.host().isEmpty() &&
        url.host().compare(QStringLiteral("t.me"), Qt::CaseInsensitive) != 0)) {
        return {};
    }
    QStringList segments = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!segments.isEmpty() && segments.first() == QStringLiteral("s")) {
        segments.removeFirst();
    }
    if (segments.size() < 2) {
        return {};
    }
    const QString channel = segments.at(segments.size() - 2);
    const QString post = segments.last();
    if (channel.isEmpty() || post.isEmpty()) {
        return {};
    }
    for (const QChar ch : post) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) {
            return {};
        }
    }
    return channel + QLatin1Char('/') + post;
}

QString channelIdFromPageUrl(const QString &raw)
{
    const QUrl url(raw, QUrl::StrictMode);
    const QString host = url.host().toLower();
    if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
        (host != QStringLiteral("www.youtube.com") && host != QStringLiteral("youtube.com")) ||
        !url.userInfo().isEmpty()) {
        return {};
    }
    const QStringList segments = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype i = 0; i + 1 < segments.size(); ++i) {
        if (segments.at(i) == QStringLiteral("channel") && validYouTubeChannelId(segments.at(i + 1))) {
            return segments.at(i + 1);
        }
    }
    return {};
}

QString findChannelIdInJson(const QJsonValue &value)
{
    QVector<QJsonValue> pending{value};
    while (!pending.isEmpty()) {
        const QJsonValue current = pending.takeLast();
        if (!current.isObject() && !current.isArray()) {
            continue;
        }
        if (current.isObject()) {
            const QJsonObject object = current.toObject();
            for (const QString &key : {QStringLiteral("channelId"), QStringLiteral("channel_id"),
                                       QStringLiteral("externalChannelId")}) {
                const QJsonValue candidate = object.value(key);
                if (candidate.isString() && validYouTubeChannelId(candidate.toString())) {
                    return candidate.toString();
                }
            }
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                pending.push_back(it.value());
            }
        } else {
            for (const QJsonValue &item : current.toArray()) {
                pending.push_back(item);
            }
        }
    }
    return {};
}

QJsonObject firstJsonObjectInScript(const QString &script)
{
    for (qsizetype start = 0; start < script.size(); ++start) {
        if (script.at(start) != QLatin1Char('{')) {
            continue;
        }
        qsizetype depth = 0;
        bool inString = false;
        bool escaped = false;
        for (qsizetype end = start; end < script.size(); ++end) {
            const QChar ch = script.at(end);
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (ch == QLatin1Char('\\')) {
                    escaped = true;
                } else if (ch == QLatin1Char('"')) {
                    inString = false;
                }
                continue;
            }
            if (ch == QLatin1Char('"')) {
                inString = true;
            } else if (ch == QLatin1Char('{')) {
                ++depth;
            } else if (ch == QLatin1Char('}') && --depth == 0) {
                const QByteArray candidate = script.mid(start, end - start + 1).toUtf8();
                QJsonParseError parseError;
                const QJsonDocument document = QJsonDocument::fromJson(candidate, &parseError);
                if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                    return document.object();
                }
                break;
            }
        }
    }
    return {};
}

QString readXmlText(QXmlStreamReader &reader)
{
    return reader.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
}

void parseMediaGroup(QXmlStreamReader &reader,
                     QString *thumbnail,
                     QString *description,
                     QString *views)
{
    const QString mediaNamespace = QStringLiteral("http://search.yahoo.com/mrss/");
    while (reader.readNextStartElement()) {
        if (reader.namespaceUri() == mediaNamespace && reader.name() == QStringLiteral("thumbnail")) {
            *thumbnail = reader.attributes().value(QStringLiteral("url")).toString().trimmed();
            reader.skipCurrentElement();
        } else if (reader.namespaceUri() == mediaNamespace &&
                   reader.name() == QStringLiteral("description")) {
            *description = readXmlText(reader);
        } else if (reader.namespaceUri() == mediaNamespace &&
                   reader.name() == QStringLiteral("community")) {
            while (reader.readNextStartElement()) {
                if (reader.namespaceUri() == mediaNamespace &&
                    reader.name() == QStringLiteral("statistics")) {
                    *views = reader.attributes().value(QStringLiteral("views")).toString().trimmed();
                    reader.skipCurrentElement();
                } else {
                    reader.skipCurrentElement();
                }
            }
        } else {
            reader.skipCurrentElement();
        }
    }
}

YouTubeVideo parseFeedEntry(QXmlStreamReader &reader, const QString &channelId)
{
    const QString atomNamespace = QStringLiteral("http://www.w3.org/2005/Atom");
    const QString videoNamespace = QStringLiteral("http://www.youtube.com/xml/schemas/2015");
    const QString mediaNamespace = QStringLiteral("http://search.yahoo.com/mrss/");
    YouTubeVideo video;
    video.channelId = channelId;

    while (reader.readNextStartElement()) {
        const QString name = reader.name().toString();
        const QString nameSpace = reader.namespaceUri().toString();
        if (nameSpace == atomNamespace && name == QStringLiteral("title")) {
            video.title = readXmlText(reader);
        } else if (nameSpace == atomNamespace && name == QStringLiteral("published")) {
            video.publishedAt = readXmlText(reader);
        } else if (nameSpace == atomNamespace && name == QStringLiteral("author")) {
            while (reader.readNextStartElement()) {
                if (reader.namespaceUri() == atomNamespace && reader.name() == QStringLiteral("name")) {
                    video.channelTitle = readXmlText(reader);
                } else {
                    reader.skipCurrentElement();
                }
            }
        } else if (nameSpace == videoNamespace && name == QStringLiteral("videoId")) {
            video.videoId = readXmlText(reader);
        } else if (nameSpace == mediaNamespace && name == QStringLiteral("group")) {
            parseMediaGroup(reader, &video.thumbnail, &video.description, &video.views);
        } else {
            reader.skipCurrentElement();
        }
    }

    if (validYouTubeVideoId(video.videoId)) {
        video.videoUrl = QStringLiteral("https://www.youtube.com/watch?v=") + video.videoId;
    }
    QString safeThumbnail;
    if (isSafeHttpsUrl(video.thumbnail, &safeThumbnail)) {
        video.thumbnail = std::move(safeThumbnail);
    } else {
        video.thumbnail.clear();
    }
    return video;
}

} // namespace

bool ProviderJsonObject::parse(const QByteArray &body,
                               ProviderJsonObject *result,
                               something::domain::AppError *error)
{
    if (error) {
        *error = {};
    }
    if (!result || body.isEmpty() || body.size() > kMaximumProviderPayloadBytes) {
        if (error) {
            *error = invalidPayload();
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = invalidPayload();
        }
        return false;
    }
    result->m_object = document.object();
    return true;
}

JsonField<QString> ProviderJsonObject::string(const QString &key) const
{
    const QJsonValue value = m_object.value(key);
    if (!m_object.contains(key)) return {};
    if (!value.isString()) return {JsonFieldState::WrongType, {}};
    return {JsonFieldState::Present, value.toString()};
}

JsonField<double> ProviderJsonObject::number(const QString &key) const
{
    const QJsonValue value = m_object.value(key);
    if (!m_object.contains(key)) return {};
    if (!value.isDouble() || !std::isfinite(value.toDouble())) {
        return {JsonFieldState::WrongType, 0.0};
    }
    return {JsonFieldState::Present, value.toDouble()};
}

JsonField<bool> ProviderJsonObject::boolean(const QString &key) const
{
    const QJsonValue value = m_object.value(key);
    if (!m_object.contains(key)) return {};
    if (!value.isBool()) return {JsonFieldState::WrongType, false};
    return {JsonFieldState::Present, value.toBool()};
}

JsonField<QJsonObject> ProviderJsonObject::object(const QString &key) const
{
    const QJsonValue value = m_object.value(key);
    if (!m_object.contains(key)) return {};
    if (!value.isObject()) return {JsonFieldState::WrongType, {}};
    return {JsonFieldState::Present, value.toObject()};
}

JsonField<QJsonArray> ProviderJsonObject::array(const QString &key) const
{
    const QJsonValue value = m_object.value(key);
    if (!m_object.contains(key)) return {};
    if (!value.isArray()) return {JsonFieldState::WrongType, {}};
    return {JsonFieldState::Present, value.toArray()};
}

ProviderParseResult<TelegramPost> parseTelegramPosts(const QByteArray &body)
{
    ProviderParseResult<TelegramPost> result;
    if (body.trimmed().isEmpty() || body.size() > kMaximumProviderPayloadBytes) {
        result.error = invalidPayload();
        return result;
    }

    lxb_html_parser_t *parser = lxb_html_parser_create();
    if (!parser || lxb_html_parser_init(parser) != LXB_STATUS_OK) {
        if (parser) lxb_html_parser_destroy(parser);
        result.error = invalidPayload();
        return result;
    }
    lxb_html_document_t *document = lxb_html_parse(
        parser, reinterpret_cast<const lxb_char_t *>(body.constData()), size_t(body.size()));
    lxb_html_parser_destroy(parser);
    if (!document) {
        result.error = invalidPayload();
        return result;
    }

    lxb_dom_node_t *root = lxb_dom_interface_node(document);
    const auto wrappers = descendantsWithClass(root, QByteArrayLiteral("tgme_widget_message_wrap"));
    for (lxb_dom_element_t *wrapper : wrappers) {
        TelegramPost post;
        const auto textNodes = descendantsWithClass(
            lxb_dom_interface_node(wrapper), QByteArrayLiteral("tgme_widget_message_text"));
        if (!textNodes.isEmpty()) {
            post.text = lexborText(lxb_dom_interface_node(textNodes.first()));
        }

        const auto photoContainers = descendantsWithClass(
            lxb_dom_interface_node(wrapper), QByteArrayLiteral("tgme_widget_message_photo_wrap"));
        for (lxb_dom_element_t *container : photoContainers) {
            const QString image = cssUrl(lexborAttribute(container, "style"));
            if (!image.isEmpty()) {
                post.images.push_back(image);
            }
        }

        const auto imageContainers = descendantsWithClass(
            lxb_dom_interface_node(wrapper), QByteArrayLiteral("tgme_widget_message_photo"));
        for (lxb_dom_element_t *container : imageContainers) {
            const auto imageNodes = descendantsWithAttribute(lxb_dom_interface_node(container), "src");
            for (lxb_dom_element_t *imageNode : imageNodes) {
                QString image;
                if (isSafeHttpsUrl(QString::fromUtf8(lexborAttribute(imageNode, "src")), &image)) {
                    post.images.push_back(image);
                }
            }
        }

        const auto dateNodes = descendantsWithClass(
            lxb_dom_interface_node(wrapper), QByteArrayLiteral("tgme_widget_message_date"));
        if (!dateNodes.isEmpty()) {
            const auto dateElements = descendantsWithAttribute(lxb_dom_interface_node(dateNodes.first()),
                                                               "datetime");
            if (!dateElements.isEmpty()) {
                post.date = QString::fromUtf8(lexborAttribute(dateElements.first(), "datetime")).trimmed();
            }
        }

        const auto viewNodes = descendantsWithClass(
            lxb_dom_interface_node(wrapper), QByteArrayLiteral("tgme_widget_message_views"));
        if (!viewNodes.isEmpty()) {
            post.views = lexborText(lxb_dom_interface_node(viewNodes.first()));
        }

        const QByteArray directPostId = lexborAttribute(wrapper, "data-post");
        if (!directPostId.isEmpty()) {
            post.postId = QString::fromUtf8(directPostId).trimmed();
        } else {
            const auto messageNodes = descendantsWithClass(
                lxb_dom_interface_node(wrapper), QByteArrayLiteral("tgme_widget_message"));
            if (!messageNodes.isEmpty()) {
                post.postId = QString::fromUtf8(lexborAttribute(messageNodes.first(), "data-post")).trimmed();
            }
            if (post.postId.isEmpty() && !dateNodes.isEmpty()) {
                post.postId = postIdFromHref(QString::fromUtf8(
                    lexborAttribute(dateNodes.first(), "href")));
            }
        }

        if (!post.text.isEmpty() || !post.images.isEmpty() || !post.postId.isEmpty() ||
            !post.date.isEmpty()) {
            result.items.push_back(std::move(post));
        }
    }
    lxb_html_document_destroy(document);

    std::reverse(result.items.begin(), result.items.end());
    return result;
}

ProviderParseResult<YouTubeVideo> parseYouTubeFeed(const QByteArray &body,
                                                   const QString &channelId)
{
    ProviderParseResult<YouTubeVideo> result;
    if (body.isEmpty() || body.size() > kMaximumProviderPayloadBytes ||
        !validYouTubeChannelId(channelId)) {
        result.error = invalidPayload();
        return result;
    }

    QXmlStreamReader reader(body);
    const QString atomNamespace = QStringLiteral("http://www.w3.org/2005/Atom");
    bool foundFeed = false;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD) {
            result.error = invalidPayload();
            return result;
        }
        if (token == QXmlStreamReader::StartElement &&
            reader.name() == QStringLiteral("feed") && reader.namespaceUri() == atomNamespace) {
            foundFeed = true;
            while (reader.readNextStartElement()) {
                if (reader.name() == QStringLiteral("entry") && reader.namespaceUri() == atomNamespace) {
                    YouTubeVideo video = parseFeedEntry(reader, channelId);
                    if (validYouTubeVideoId(video.videoId)) {
                        result.items.push_back(std::move(video));
                    }
                } else {
                    reader.skipCurrentElement();
                }
            }
            break;
        }
    }
    if (!foundFeed || reader.hasError()) {
        result.items.clear();
        result.error = invalidPayload();
    }
    return result;
}

QString parseYouTubeChannelIdPage(const QByteArray &body, something::domain::AppError *error)
{
    if (error) {
        *error = {};
    }
    if (body.trimmed().isEmpty() || body.size() > kMaximumProviderPayloadBytes) {
        if (error) *error = invalidPayload();
        return {};
    }

    lxb_html_parser_t *parser = lxb_html_parser_create();
    if (!parser || lxb_html_parser_init(parser) != LXB_STATUS_OK) {
        if (parser) lxb_html_parser_destroy(parser);
        if (error) *error = invalidPayload();
        return {};
    }
    lxb_html_document_t *document = lxb_html_parse(
        parser, reinterpret_cast<const lxb_char_t *>(body.constData()), size_t(body.size()));
    lxb_html_parser_destroy(parser);
    if (!document) {
        if (error) *error = invalidPayload();
        return {};
    }

    QString channelId;
    const auto elements = allElements(lxb_dom_interface_node(document));
    for (lxb_dom_element_t *element : elements) {
        const QString itemprop = QString::fromUtf8(lexborAttribute(element, "itemprop"));
        if (itemprop == QStringLiteral("channelId")) {
            const QString candidate = QString::fromUtf8(lexborAttribute(element, "content")).trimmed();
            if (validYouTubeChannelId(candidate)) {
                channelId = candidate;
                break;
            }
        }
        const QString rel = QString::fromUtf8(lexborAttribute(element, "rel")).toLower();
        if (rel == QStringLiteral("canonical")) {
            channelId = channelIdFromPageUrl(QString::fromUtf8(lexborAttribute(element, "href")));
        }
        const QString property = QString::fromUtf8(lexborAttribute(element, "property"));
        if (property == QStringLiteral("og:url")) {
            const QString candidate = channelIdFromPageUrl(
                QString::fromUtf8(lexborAttribute(element, "content")));
            if (channelId.isEmpty()) {
                channelId = candidate;
            }
        }
        if (!channelId.isEmpty()) {
            break;
        }
        if (elementName(element) == QStringLiteral("script")) {
            const QString script = lexborText(lxb_dom_interface_node(element));
            const QString candidate = findChannelIdInJson(firstJsonObjectInScript(script));
            if (!candidate.isEmpty()) {
                channelId = candidate;
                break;
            }
        }
    }
    lxb_html_document_destroy(document);
    if (!validYouTubeChannelId(channelId)) {
        if (error) *error = invalidPayload();
        return {};
    }
    return channelId;
}

} // namespace something::providers

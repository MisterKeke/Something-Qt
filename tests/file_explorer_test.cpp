#include "../src/platform/file_explorer_platform.h"
#include "../src/services/file_explorer_service.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#if defined(Q_OS_WIN)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace something;

namespace {

class FakeFileExplorerShell final : public platform::FileExplorerShell {
public:
    bool supportsNativeActions() const noexcept override { return enabled; }
    domain::AppError openFile(const QString &path) override
    {
        opened.push_back(path);
        return openError;
    }
    domain::AppError recycleFile(const QString &path) override
    {
        recycled.push_back(path);
        return recycleError;
    }

    bool enabled{true};
    domain::AppError openError;
    domain::AppError recycleError;
    QStringList opened;
    QStringList recycled;
};

QString createFile(const QString &directory, const QString &name)
{
    const QString path = QDir(directory).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    file.write("fixture");
    file.close();
    return path;
}

} // namespace

class FileExplorerTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void rootsAreOpaqueSessionScopedAndDeduplicated();
    void listIsImmediateDeterministicAndFilterable();
    void listingIsPagedAndReportsTruncation();
    void rejectsTraversalAndDoesNotFollowLinks();
    void cancellationStopsTheCurrentListing();
    void nativeActionsReceiveOnlyValidatedRegularFiles();
    void vanishedRootsAndEntriesReturnSafeErrors();

private:
    QTemporaryDir m_root;
    QTemporaryDir m_outside;
    platform::NativeFileExplorerPlatform m_platform;
    FakeFileExplorerShell m_shell;
    services::FileExplorerService m_service{&m_platform, &m_shell};
    QString m_rootId;
};

void FileExplorerTest::init()
{
    QVERIFY(m_root.isValid());
    QVERIFY(m_outside.isValid());
    QVERIFY(QDir().mkpath(QDir(m_root.path()).filePath(QStringLiteral("Folder"))));
    QVERIFY(!createFile(m_root.path(), QStringLiteral("Alpha.txt")).isEmpty());
    QVERIFY(!createFile(QDir(m_root.path()).filePath(QStringLiteral("Folder")),
                        QStringLiteral("nested.txt")).isEmpty());
    const QVariantMap result = m_service.addSessionRoot(m_root.path(), {});
    QVERIFY(result.value(QStringLiteral("ok")).toBool());
    m_rootId = result.value(QStringLiteral("root")).toMap()
                   .value(QStringLiteral("rootId")).toString();
    QVERIFY(!m_rootId.isEmpty());
}

void FileExplorerTest::cleanup()
{
    m_shell.opened.clear();
    m_shell.recycled.clear();
    QDir(m_root.path()).removeRecursively();
    QDir(m_outside.path()).removeRecursively();
    QDir().mkpath(m_root.path());
    QDir().mkpath(m_outside.path());
}

void FileExplorerTest::rootsAreOpaqueSessionScopedAndDeduplicated()
{
    services::FileExplorerService nextSession(&m_platform, &m_shell);
    const QVariantMap first = nextSession.addSessionRoot(m_root.path(), {});
    const QVariantMap duplicate = nextSession.addSessionRoot(m_root.path(), {});
    QVERIFY(first.value(QStringLiteral("ok")).toBool());
    QVERIFY(duplicate.value(QStringLiteral("ok")).toBool());
    const QString firstId = first.value(QStringLiteral("root")).toMap()
                                .value(QStringLiteral("rootId")).toString();
    const QString duplicateId = duplicate.value(QStringLiteral("root")).toMap()
                                    .value(QStringLiteral("rootId")).toString();
    QCOMPARE(firstId, duplicateId);
    QVERIFY(firstId != m_rootId);
    QVERIFY(!firstId.contains(m_root.path()));
}

void FileExplorerTest::listIsImmediateDeterministicAndFilterable()
{
    const auto token = application::CancellationSource().token();
    const QVariantMap result = m_service.listDirectory(m_rootId, {}, {}, 0, 250, token);
    QVERIFY(result.value(QStringLiteral("ok")).toBool());
    const QVariantList rows = result.value(QStringLiteral("entries")).toList();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Folder"));
    QCOMPARE(rows.at(1).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Alpha.txt"));
    QVERIFY(rows.at(0).toMap().value(QStringLiteral("canNavigate")).toBool());

    const QVariantMap nested = m_service.listDirectory(
        m_rootId, {}, QStringLiteral("nested"), 0, 250, token);
    QVERIFY(nested.value(QStringLiteral("ok")).toBool());
    QCOMPARE(nested.value(QStringLiteral("entries")).toList().size(), 0);
    const QVariantMap currentOnly = m_service.listDirectory(
        m_rootId, QStringLiteral("Folder"), QStringLiteral("nested"), 0, 250, token);
    QVERIFY(currentOnly.value(QStringLiteral("ok")).toBool());
    QCOMPARE(currentOnly.value(QStringLiteral("entries")).toList().size(), 1);
}

void FileExplorerTest::listingIsPagedAndReportsTruncation()
{
    for (int i = 0; i < 260; ++i)
        QVERIFY(!createFile(m_root.path(), QStringLiteral("item-%1.txt").arg(i, 3, 10, QLatin1Char('0'))).isEmpty());
    const auto token = application::CancellationSource().token();
    const QVariantMap first = m_service.listDirectory(m_rootId, {}, {}, 0, 250, token);
    QVERIFY(first.value(QStringLiteral("ok")).toBool());
    QCOMPARE(first.value(QStringLiteral("entries")).toList().size(), 250);
    QVERIFY(first.value(QStringLiteral("hasMore")).toBool());
    QVERIFY(!first.value(QStringLiteral("truncated")).toBool());
    const QVariantMap second = m_service.listDirectory(m_rootId, {}, {}, 250, 250, token);
    QVERIFY(second.value(QStringLiteral("ok")).toBool());
    QCOMPARE(second.value(QStringLiteral("entries")).toList().size(), 12);
    QVERIFY(!second.value(QStringLiteral("hasMore")).toBool());

    for (int i = 260; i < 2001; ++i)
        QVERIFY(!createFile(m_root.path(), QStringLiteral("item-%1.txt").arg(i, 4, 10, QLatin1Char('0'))).isEmpty());
    const QVariantMap capped = m_service.listDirectory(m_rootId, {}, {}, 1750, 250, token);
    QVERIFY(capped.value(QStringLiteral("ok")).toBool());
    QCOMPARE(capped.value(QStringLiteral("entries")).toList().size(), 250);
    QVERIFY(capped.value(QStringLiteral("truncated")).toBool());
    QVERIFY(!capped.value(QStringLiteral("hasMore")).toBool());
}

void FileExplorerTest::rejectsTraversalAndDoesNotFollowLinks()
{
    const auto token = application::CancellationSource().token();
    const QVariantMap traversal = m_service.listDirectory(
        m_rootId, QStringLiteral("../outside"), {}, 0, 250, token);
    QVERIFY(!traversal.value(QStringLiteral("ok")).toBool());
    QCOMPARE(traversal.value(QStringLiteral("errorCode")).toString(), QStringLiteral("invalid-path"));

    QVERIFY(!createFile(m_outside.path(), QStringLiteral("secret.txt")).isEmpty());
    const QString linkPath = QDir(m_root.path()).filePath(QStringLiteral("outside-link"));
#if defined(Q_OS_WIN)
    const bool linked = CreateSymbolicLinkW(
        reinterpret_cast<LPCWSTR>(linkPath.utf16()),
        reinterpret_cast<LPCWSTR>(m_outside.path().utf16()),
        SYMBOLIC_LINK_FLAG_DIRECTORY) != 0;
#else
    const bool linked = QFile::link(m_outside.path(), linkPath);
#endif
    if (!linked) QSKIP("The current test platform does not permit creating a directory link.");
    const QVariantMap listed = m_service.listDirectory(m_rootId, {}, {}, 0, 250, token);
    QVERIFY(listed.value(QStringLiteral("ok")).toBool());
    bool foundBlockedLink = false;
    for (const QVariant &value : listed.value(QStringLiteral("entries")).toList()) {
        const QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("name")).toString() == QStringLiteral("outside-link")) {
            foundBlockedLink = true;
            QVERIFY(!row.value(QStringLiteral("canNavigate")).toBool());
            QCOMPARE(row.value(QStringLiteral("type")).toString(), QStringLiteral("link"));
        }
    }
    QVERIFY(foundBlockedLink);
    const QVariantMap followed = m_service.listDirectory(
        m_rootId, QStringLiteral("outside-link"), {}, 0, 250, token);
    QVERIFY(!followed.value(QStringLiteral("ok")).toBool());
    QCOMPARE(followed.value(QStringLiteral("errorCode")).toString(), QStringLiteral("unsafe-link"));
}

void FileExplorerTest::cancellationStopsTheCurrentListing()
{
    application::CancellationSource cancellation;
    cancellation.requestCancellation();
    const QVariantMap result = m_service.listDirectory(
        m_rootId, {}, {}, 0, 250, cancellation.token());
    QVERIFY(!result.value(QStringLiteral("ok")).toBool());
    QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("cancelled"));
}

void FileExplorerTest::nativeActionsReceiveOnlyValidatedRegularFiles()
{
    const auto token = application::CancellationSource().token();
    const QString filePath = createFile(m_root.path(), QStringLiteral("action.txt"));
    QVERIFY(!filePath.isEmpty());
    QVariantMap opened = m_service.openFile(m_rootId, QStringLiteral("action.txt"), token);
    QVERIFY(opened.value(QStringLiteral("ok")).toBool());
    QCOMPARE(m_shell.opened.size(), 1);
    QCOMPARE(QFileInfo(m_shell.opened.first()).canonicalFilePath(), QFileInfo(filePath).canonicalFilePath());

    const QVariantMap directory = m_service.recycleFile(m_rootId, QStringLiteral("Folder"), token);
    QVERIFY(!directory.value(QStringLiteral("ok")).toBool());
    QCOMPARE(directory.value(QStringLiteral("errorCode")).toString(), QStringLiteral("not-file"));
    QCOMPARE(m_shell.recycled.size(), 0);
    const QVariantMap escaped = m_service.recycleFile(m_rootId, QStringLiteral("..\\outside\\secret.txt"), token);
    QVERIFY(!escaped.value(QStringLiteral("ok")).toBool());
    QCOMPARE(m_shell.recycled.size(), 0);

    const QString unicodeName = QString::fromUtf8("東京-Δοκιμή-данные.txt");
    QVERIFY(!createFile(m_root.path(), unicodeName).isEmpty());
    const QVariantMap unicode = m_service.openFile(m_rootId, unicodeName, token);
    QVERIFY(unicode.value(QStringLiteral("ok")).toBool());
    QCOMPARE(m_shell.opened.size(), 2);

    const QString longName = QString(235, QLatin1Char('n')) + QStringLiteral(".txt");
    if (createFile(m_root.path(), longName).isEmpty())
        QSKIP("The host OS has not enabled the long-path policy for this temporary location.");
    const QVariantMap longPath = m_service.openFile(m_rootId, longName, token);
    QVERIFY(longPath.value(QStringLiteral("ok")).toBool());
    QCOMPARE(m_shell.opened.size(), 3);
}

void FileExplorerTest::vanishedRootsAndEntriesReturnSafeErrors()
{
    const auto token = application::CancellationSource().token();
    const QString disappeared = createFile(m_root.path(), QStringLiteral("disappears.txt"));
    QVERIFY(!disappeared.isEmpty());
    QVERIFY(QFile::remove(disappeared));
    const QVariantMap result = m_service.openFile(m_rootId, QStringLiteral("disappears.txt"), token);
    QVERIFY(!result.value(QStringLiteral("ok")).toBool());
    QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("not-found"));
    QCOMPARE(m_shell.opened.size(), 0);
}

QTEST_GUILESS_MAIN(FileExplorerTest)
#include "file_explorer_test.moc"

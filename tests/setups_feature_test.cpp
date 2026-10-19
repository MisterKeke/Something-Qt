#include "models/setups_model.h"
#include "services/desktop_applications_service.h"
#include "services/setup_icon_store.h"
#include "services/setups_service.h"

#include <QImage>
#include <QTemporaryDir>
#include <QVariantMap>
#include <QtTest>

using namespace something;

class SetupsFeatureTest final : public QObject {
    Q_OBJECT
private slots:
    void continuesAfterPerApplicationFailure();
    void userCancellationStopsBeforeTheNextMember();
    void cancellationBeforeNativeStartPreventsTheLaunch();
    void modelSummarizesApplicationIdsInStoredOrder();
    void setupIconStoreStagesAndValidatesImages();
};

class NoLaunchPlatform final : public platform::DesktopApplicationsPlatform {
public:
    bool supported() const noexcept override { return true; }
    domain::AppError resolveExecutable(const QString &selectedPath, QString *canonicalPath) const override
    {
        *canonicalPath = selectedPath;
        if (cancelDuringResolve) cancelDuringResolve->requestCancellation();
        return domain::AppError::none();
    }
    QImage extractExecutableIcon(const QString &, const application::CancellationToken &) const override
    {
        return {};
    }
    domain::AppError launchExecutable(const QString &, const application::CancellationToken &) const override
    {
        ++launchCalls;
        return domain::AppError::none();
    }
    mutable int launchCalls{0};
    application::CancellationSource *cancelDuringResolve{nullptr};
};

static QVariantList members()
{
    return {QVariantMap{{QStringLiteral("id"), 11}, {QStringLiteral("displayName"), QStringLiteral("One")}},
            QVariantMap{{QStringLiteral("id"), 12}, {QStringLiteral("displayName"), QStringLiteral("Two")}},
            QVariantMap{{QStringLiteral("id"), 13}, {QStringLiteral("displayName"), QStringLiteral("Three")}}};
}

void SetupsFeatureTest::continuesAfterPerApplicationFailure()
{
    application::CancellationSource applicationCancellation;
    application::CancellationSource userCancellation;
    QVector<qint64> called;
    const auto summary = services::SetupsService::launchMembers(members(),
        applicationCancellation.token(), userCancellation,
        [&called](const QVariantMap &app, const application::CancellationToken &,
                  const application::CancellationToken &) {
            const qint64 id = app.value(QStringLiteral("id")).toLongLong();
            called.push_back(id);
            return id == 12 ? domain::AppError(domain::ErrorCategory::Platform)
                            : domain::AppError::none();
        });
    QCOMPARE(called, QVector<qint64>({11, 12, 13}));
    QCOMPARE(summary.attempted, 3);
    QCOMPARE(summary.launched, 2);
    QCOMPARE(summary.failures.size(), 1);
    QCOMPARE(summary.failures.front().applicationId, 12);
    QVERIFY(!summary.failures.front().message.isEmpty());
    QVERIFY(!summary.failures.front().message.contains(QStringLiteral("C:/")));
    QCOMPARE(summary.toVariantMap().value(QStringLiteral("failures")).toList().size(), 1);
}

void SetupsFeatureTest::userCancellationStopsBeforeTheNextMember()
{
    application::CancellationSource applicationCancellation;
    application::CancellationSource userCancellation;
    QVector<qint64> called;
    const auto summary = services::SetupsService::launchMembers(members(),
        applicationCancellation.token(), userCancellation,
        [&called, userCancellation](const QVariantMap &app,
            const application::CancellationToken &, const application::CancellationToken &) {
            called.push_back(app.value(QStringLiteral("id")).toLongLong());
            userCancellation.requestCancellation();
            return domain::AppError::none();
        });
    QCOMPARE(called, QVector<qint64>({11}));
    QCOMPARE(summary.attempted, 1);
    QCOMPARE(summary.launched, 1);
    QVERIFY(summary.cancelled);
    QVERIFY(summary.failures.isEmpty());
}

void SetupsFeatureTest::cancellationBeforeNativeStartPreventsTheLaunch()
{
    NoLaunchPlatform platform;
    application::CancellationSource applicationCancellation;
    application::CancellationSource userCancellation;
    platform.cancelDuringResolve = &userCancellation;
    const auto userToken = userCancellation.token();
    const QVariantMap application{{QStringLiteral("id"), 21},
        {QStringLiteral("displayName"), QStringLiteral("Fixture")},
        {QStringLiteral("executablePath"), QStringLiteral("fixture.exe")}};
    const auto error = services::DesktopApplicationsService::launchSavedRecord(
        application, &platform, applicationCancellation.token(), &userToken);
    QVERIFY(error.isCancelled());
    QCOMPARE(platform.launchCalls, 0);
}

void SetupsFeatureTest::modelSummarizesApplicationIdsInStoredOrder()
{
    models::SetupsModel model;
    const QVariantList apps{
        QVariantMap{{QStringLiteral("id"), 11}, {QStringLiteral("displayName"), QStringLiteral("One")}},
        QVariantMap{{QStringLiteral("id"), 12}, {QStringLiteral("displayName"), QStringLiteral("Two")}}};
    const QVariantList rows{QVariantMap{{QStringLiteral("id"), 4},
        {QStringLiteral("name"), QStringLiteral("Desk" )},
        {QStringLiteral("description"), QString()},
        {QStringLiteral("applicationIds"), QVariantList{12, 11}},
        {QStringLiteral("iconFilename"), QString()}}};
    model.replace(rows, apps);
    QCOMPARE(model.count(), 1);
    QCOMPARE(model.data(model.index(0), models::SetupsModel::ApplicationSummaryRole).toString(),
             QStringLiteral("Two · One"));
    QCOMPARE(model.data(model.index(0), models::SetupsModel::ApplicationIdsRole).toList(),
             QVariantList({12, 11}));
}

void SetupsFeatureTest::setupIconStoreStagesAndValidatesImages()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString input = temporary.filePath(QStringLiteral("fixture.png"));
    QImage image(24, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::cyan);
    QVERIFY(image.save(input, "PNG"));
    application::CancellationSource cancellation;
    services::StagedSetupIcon staged;
    QCOMPARE(services::SetupIconStore::stageImport(input, temporary.path(),
        cancellation.token(), &staged).category(), domain::ErrorCategory::None);
    QCOMPARE(staged.asset.mimeType, QStringLiteral("image/png"));
    QVERIFY(services::SetupIconStore::install(temporary.path(), staged));
    QVERIFY(services::SetupIconStore::remove(temporary.path(), staged.asset.filename));
    QVERIFY(!services::SetupIconStore::remove(temporary.path(), QStringLiteral("../escape.png")));
}

QTEST_GUILESS_MAIN(SetupsFeatureTest)
#include "setups_feature_test.moc"

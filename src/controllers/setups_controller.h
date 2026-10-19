#pragma once

#include "../application/application_service_graph.h"
#include "../models/setups_model.h"
#include "../platform/desktop_applications_platform.h"
#include "../providers/setup_icon_image_provider.h"
#include "../services/setup_icon_store.h"
#include "../services/setups_service.h"

#include <QObject>
#include <QHash>
#include <QVariantList>
#include <QVariantMap>
#include <QUrl>

namespace something::controllers {

class SetupsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::SetupsModel *setups READ setups CONSTANT)
    Q_PROPERTY(QVariantList applications READ applications NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool launching READ launching NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap launchSummary READ launchSummary NOTIFY stateChanged)

public:
    SetupsController(application::ApplicationServiceGraph *graph,
        platform::DesktopApplicationsPlatform *platform,
        providers::SetupIconImageProvider *images, QObject *parent = nullptr);
    models::SetupsModel *setups() noexcept { return &m_setups; }
    QVariantList applications() const { return m_applications; }
    bool loading() const noexcept { return m_loading; }
    bool busy() const noexcept { return m_busy; }
    bool launching() const noexcept { return m_launching; }
    QString errorMessage() const { return m_errorMessage; }
    QString notification() const { return m_notification; }
    QVariantMap launchSummary() const { return m_launchSummary; }

    Q_INVOKABLE void setPageVisible(bool visible);
    Q_INVOKABLE void reload();
    Q_INVOKABLE void saveSetup(qint64 id, QString name, QString description,
                               QVariantList applicationIds, QString iconPath, bool removeIcon);
    Q_INVOKABLE void saveSetupFromFile(qint64 id, QString name, QString description,
        QVariantList applicationIds, const QUrl &iconFile, bool removeIcon);
    Q_INVOKABLE void removeSetup(qint64 id);
    Q_INVOKABLE void startSetup(qint64 id);
    Q_INVOKABLE void cancelLaunch();
    Q_INVOKABLE void clearMessages();

signals:
    void stateChanged();
    void mutationFinished(bool success);
    void launchFinished(QVariantMap summary);

private:
    enum class Kind { List, IconPrepare, Save, Remove, LaunchPrepare, LaunchRun };
    struct Pending final {
        Kind kind{Kind::List};
        application::OperationContext operation;
        storage::Setup draft;
        bool removeIcon{false};
        QString iconPath;
        QString oldIconFilename;
        std::optional<services::StagedSetupIcon> stagedIcon;
        application::CancellationSource userCancellation;
        bool mutation{false};
    };

    application::OperationContext begin(Kind kind, QString purpose, bool mutation = false);
    bool submitStorage(quint64 id, storage::StorageCommand command);
    bool submitValue(quint64 id, services::BoundedWorkerPool::ValueWork work);
    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    void onValueFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    void commitDraft(quint64 previousOperationId, Pending pending);
    void startLaunchWorker(quint64 previousOperationId, Pending pending,
                           QVariantList orderedApplications);
    void fail(quint64 id, domain::AppError error);
    void complete(quint64 id);
    void cancelAll();

    application::ApplicationServiceGraph *m_graph{nullptr};
    platform::DesktopApplicationsPlatform *m_platform{nullptr};
    providers::SetupIconImageProvider *m_images{nullptr};
    models::SetupsModel m_setups;
    QVariantList m_applications;
    QHash<quint64, Pending> m_pending;
    quint64 m_listOperation{0};
    quint64 m_activeLaunchOperation{0};
    bool m_stagingCleanupDone{false};
    bool m_ready{false};
    bool m_pageVisible{false};
    bool m_loading{false};
    bool m_busy{false};
    bool m_launching{false};
    QString m_errorMessage;
    QString m_notification;
    QVariantMap m_launchSummary;
};

} // namespace something::controllers

#pragma once

#include "../application/application_service_graph.h"
#include "../models/desktop_application_categories_model.h"
#include "../models/desktop_applications_model.h"
#include "../platform/desktop_applications_platform.h"
#include "../providers/application_icon_image_provider.h"
#include "../services/desktop_app_icon_store.h"

#include <QHash>
#include <QObject>
#include <QVariantList>
#include <QUrl>

namespace something::controllers {

class DesktopApplicationsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::DesktopApplicationsModel *applications READ applications CONSTANT)
    Q_PROPERTY(something::models::DesktopApplicationCategoriesModel *categories READ categories CONSTANT)
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool categoriesLoading READ categoriesLoading NOTIFY stateChanged)
    Q_PROPERTY(int uncategorizedCount READ uncategorizedCount NOTIFY stateChanged)
    Q_PROPERTY(int totalApplicationsCount READ totalApplicationsCount NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY stateChanged)

public:
    DesktopApplicationsController(application::ApplicationServiceGraph *graph,
        platform::DesktopApplicationsPlatform *platform,
        providers::ApplicationIconImageProvider *images,
        QObject *parent = nullptr);

    models::DesktopApplicationsModel *applications() noexcept { return &m_applications; }
    models::DesktopApplicationCategoriesModel *categories() noexcept { return &m_categories; }
    bool supported() const noexcept { return m_platform && m_platform->supported(); }
    bool loading() const noexcept { return m_loading; }
    bool categoriesLoading() const noexcept { return m_categoriesLoading; }
    int uncategorizedCount() const noexcept { return m_uncategorizedCount; }
    int totalApplicationsCount() const;
    bool busy() const noexcept { return m_busy; }
    QString errorMessage() const { return m_errorMessage; }
    QString notification() const { return m_notification; }

    Q_INVOKABLE void setPageVisible(bool visible);
    Q_INVOKABLE void reload();
    Q_INVOKABLE void loadAllApplications();
    Q_INVOKABLE void loadApplicationTargets();
    Q_INVOKABLE void loadCategories();
    Q_INVOKABLE void loadApplicationsByCategory(qint64 categoryId);
    Q_INVOKABLE void loadUncategorizedApplications();
    Q_INVOKABLE void createCategory(QString name, QString description = {},
                                    QString color = {}, QString iconIdentifier = {});
    Q_INVOKABLE void updateCategory(qint64 id, QString name, QString description = {},
                                    QString color = {}, QString iconIdentifier = {});
    Q_INVOKABLE void reorderCategories(QVariantList orderedIds);
    Q_INVOKABLE void assignApplicationCategory(qint64 applicationId, qint64 categoryId);
    Q_INVOKABLE void removeApplicationCategory(qint64 applicationId);
    Q_INVOKABLE void deleteCategory(qint64 categoryId);
    Q_INVOKABLE void addFromPath(QString selectedPath);
    Q_INVOKABLE void addFromFile(const QUrl &selectedFile);
    Q_INVOKABLE void renameApplication(qint64 id, QString name);
    Q_INVOKABLE void relocateApplication(qint64 id, QString selectedPath);
    Q_INVOKABLE void relocateFromFile(qint64 id, const QUrl &selectedFile);
    Q_INVOKABLE void launchApplication(qint64 id);
    Q_INVOKABLE void importIcon(qint64 id, QString selectedPath);
    Q_INVOKABLE void importIconFile(qint64 id, const QUrl &selectedFile);
    Q_INVOKABLE void resetIcon(qint64 id);
    Q_INVOKABLE void removeApplication(qint64 id);
    Q_INVOKABLE void clearMessages();

signals:
    void stateChanged();
    void mutationFinished(bool success);

private:
    enum class Kind { List, ListAvailability, AddPrepare, AddCommit, Rename,
        RelocatePrepare, RelocateCommit, LaunchGet, LaunchRun, IconGet,
        IconPrepare, IconCommit, IconRemoveGet, IconRemoveCommit, DeleteGet, DeleteCommit,
        CategoriesList, CategoryCreate, CategoryUpdate, CategoryReorder,
        CategoryAssign, CategoryClear, CategoryDelete };
    enum class ApplicationFilter { All, Category, Uncategorized };
    struct Pending final {
        Kind kind{Kind::List};
        application::OperationContext operation;
        qint64 applicationId{0};
        qint64 categoryId{0};
        QString selectedPath;
        QString failureFallback;
        QString oldIconFilename;
        bool mutation{false};
        bool installedIcon{false};
        services::StagedDesktopAppIcon stagedIcon;
        services::StagedDesktopAppIconDeletion stagedDeletion;
    };

    application::OperationContext begin(Kind kind, QString purpose, qint64 applicationId = 0,
                                        bool mutation = false, QString fallback = {});
    bool submitStorage(quint64 id, storage::StorageCommand command);
    bool submitValue(quint64 id, services::BoundedWorkerPool::ValueWork work);
    quint64 continuePending(quint64 previousId);
    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    void onValueFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    void fail(quint64 id, domain::AppError error, QString message = {});
    void succeed(quint64 id, QString notification = {});
    void cancelAll();
    void updateFromRecord(const QVariantMap &record);
    void startLaunch(quint64 id, QVariantMap record);
    void startIconImport(quint64 id, QVariantMap record);
    void stageIconDeletion(quint64 id, QVariantMap record, bool deleteApplication);
    void setError(QString message);

    application::ApplicationServiceGraph *m_graph{nullptr};
    platform::DesktopApplicationsPlatform *m_platform{nullptr};
    providers::ApplicationIconImageProvider *m_images{nullptr};
    models::DesktopApplicationsModel m_applications;
    models::DesktopApplicationCategoriesModel m_categories;
    QHash<quint64, Pending> m_pending;
    quint64 m_listOperation{0};
    quint64 m_categoriesOperation{0};
    ApplicationFilter m_applicationFilter{ApplicationFilter::All};
    qint64 m_filterCategoryId{0};
    bool m_stagingCleanupDone{false};
    bool m_ready{false};
    bool m_pageVisible{false};
    bool m_applicationTargetsRequested{false};
    bool m_loading{false};
    bool m_categoriesLoading{false};
    int m_uncategorizedCount{0};
    bool m_busy{false};
    QString m_errorMessage;
    QString m_notification;
};

} // namespace something::controllers

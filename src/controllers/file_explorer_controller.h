#pragma once

#include "../application/application_service_graph.h"
#include "../models/file_explorer_model.h"
#include "../services/file_explorer_service.h"

#include <QHash>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

namespace something::controllers {

class FileExplorerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::FileExplorerModel *entries READ entries CONSTANT)
    Q_PROPERTY(QVariantList places READ places NOTIFY stateChanged)
    Q_PROPERTY(QVariantList breadcrumbs READ breadcrumbs NOTIFY stateChanged)
    Q_PROPERTY(QString rootId READ rootId NOTIFY stateChanged)
    Q_PROPERTY(QString relativePath READ relativePath NOTIFY stateChanged)
    Q_PROPERTY(QString parentRelativePath READ parentRelativePath NOTIFY stateChanged)
    Q_PROPERTY(QString query READ query NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool addingRoot READ addingRoot NOTIFY stateChanged)
    Q_PROPERTY(bool actionBusy READ actionBusy NOTIFY stateChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY stateChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY stateChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY stateChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY stateChanged)
    Q_PROPERTY(bool hasParent READ hasParent NOTIFY stateChanged)
    Q_PROPERTY(bool nativeActionsSupported READ nativeActionsSupported CONSTANT)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY stateChanged)

public:
    FileExplorerController(application::ApplicationServiceGraph *graph,
                           services::FileExplorerService *service,
                           platform::FileExplorerShell *shell,
                           QObject *parent = nullptr);

    models::FileExplorerModel *entries() noexcept { return &m_entries; }
    QVariantList places() const { return m_places; }
    QVariantList breadcrumbs() const { return m_breadcrumbs; }
    QString rootId() const { return m_rootId; }
    QString relativePath() const { return m_relativePath; }
    QString parentRelativePath() const { return m_parentRelativePath; }
    QString query() const { return m_query; }
    bool ready() const noexcept { return m_ready; }
    bool loading() const noexcept { return m_loading || (!m_placesLoaded && m_placesOperation != 0); }
    bool addingRoot() const noexcept { return m_addingRoot; }
    bool actionBusy() const noexcept { return m_actionBusy; }
    bool hasMore() const noexcept { return m_hasMore; }
    bool truncated() const noexcept { return m_truncated; }
    bool canGoBack() const noexcept { return m_historyIndex > 0; }
    bool canGoForward() const noexcept { return m_historyIndex >= 0 && m_historyIndex + 1 < m_history.size(); }
    bool hasParent() const noexcept { return !m_relativePath.isEmpty(); }
    bool nativeActionsSupported() const noexcept { return m_shell && m_shell->supportsNativeActions(); }
    QString errorMessage() const { return m_errorMessage; }
    QString notification() const { return m_notification; }

    Q_INVOKABLE void setPageVisible(bool visible);
    Q_INVOKABLE void openPlace(const QString &rootId);
    Q_INVOKABLE void addSessionRoot(const QString &path);
    Q_INVOKABLE void addSessionRootFromUrl(const QUrl &folder);
    Q_INVOKABLE void navigateToPath(const QString &relativePath);
    Q_INVOKABLE void goParent();
    Q_INVOKABLE void goBack();
    Q_INVOKABLE void goForward();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setQuery(const QString &query);
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void selectEntry(const QString &relativePath);
    Q_INVOKABLE void activateEntry(const QString &relativePath);
    Q_INVOKABLE void activateSelected();
    Q_INVOKABLE void recycleFile(const QString &relativePath);
    Q_INVOKABLE void clearMessages();

signals:
    void stateChanged();

private:
    enum class Kind { Places, AddRoot, Listing, OpenFile, RecycleFile };
    enum class HistoryMode { None, Push, Replace, SelectIndex };
    struct Location final { QString rootId; QString relativePath; };
    struct Pending final {
        Kind kind{Kind::Listing};
        application::OperationContext operation;
        QString rootId;
        QString relativePath;
        QString query;
        int offset{0};
        int historyIndex{-1};
        HistoryMode historyMode{HistoryMode::None};
        bool append{false};
    };

    void onLifecycleChanged(application::ApplicationLifecycle::State state);
    void openDeferredNavigation();
    void onWorkerFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    void navigate(QString rootId, QString relativePath, HistoryMode historyMode,
                  int targetHistoryIndex = -1);
    void requestListing(int offset, bool append);
    void requestListingFor(const QString &rootId, const QString &relativePath,
                           const QString &query, int offset, bool append,
                           HistoryMode historyMode = HistoryMode::None,
                           int targetHistoryIndex = -1);
    void rememberLocation(const Location &location, HistoryMode historyMode,
                          int targetHistoryIndex);
    void requestPlaces();
    void cancelListing();
    void cancelAll();
    void finish(quint64 id, domain::AppError error = {});
    void fail(quint64 id, domain::AppError error, const QString &errorCode = {});
    void reportErrorCode(const QString &errorCode);
    static domain::AppError appErrorForCode(const QString &errorCode);
    static QVector<models::FileExplorerEntry> entriesFromValue(const QVariantList &values);
    bool containsPlace(const QString &rootId) const;

    application::ApplicationServiceGraph *m_graph{nullptr};
    services::FileExplorerService *m_service{nullptr};
    platform::FileExplorerShell *m_shell{nullptr};
    models::FileExplorerModel m_entries;
    QHash<quint64, Pending> m_pending;
    QTimer m_queryTimer;
    QVariantList m_places;
    QVariantList m_breadcrumbs;
    QVector<Location> m_history;
    int m_historyIndex{-1};
    Location m_deferredLocation;
    HistoryMode m_deferredHistoryMode{HistoryMode::None};
    int m_deferredHistoryIndex{-1};
    quint64 m_placesOperation{0};
    quint64 m_listOperation{0};
    QString m_rootId;
    QString m_defaultRootId;
    QString m_relativePath;
    QString m_parentRelativePath;
    QString m_query;
    QString m_errorMessage;
    QString m_notification;
    bool m_ready{false};
    bool m_placesLoaded{false};
    bool m_pageVisible{false};
    bool m_loading{false};
    bool m_addingRoot{false};
    bool m_actionBusy{false};
    bool m_hasMore{false};
    bool m_truncated{false};
    bool m_hasDeferredNavigation{false};
};

} // namespace something::controllers

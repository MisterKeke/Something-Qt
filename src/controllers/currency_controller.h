#pragma once

#include "../application/application_service_graph.h"
#include "../models/currency_rates_model.h"

#include <QObject>
#include <QHash>
#include <QQueue>
#include <QSet>
#include <QVariantMap>

namespace something::controllers {

class CurrencyController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(something::models::CurrencyRatesModel *allRates READ allRates CONSTANT)
    Q_PROPERTY(something::models::CurrencyFavoritesModel *favorites READ favorites CONSTANT)
    Q_PROPERTY(QVariantMap singleResult READ singleResult NOTIFY singleChanged)
    Q_PROPERTY(QVariantMap conversionResult READ conversionResult NOTIFY conversionChanged)
    Q_PROPERTY(QString allBase READ allBase NOTIFY allChanged)
    Q_PROPERTY(QString allDate READ allDate NOTIFY allChanged)
    Q_PROPERTY(bool singleLoading READ singleLoading NOTIFY singleChanged)
    Q_PROPERTY(bool allLoading READ allLoading NOTIFY allChanged)
    Q_PROPERTY(bool conversionLoading READ conversionLoading NOTIFY conversionChanged)
    Q_PROPERTY(bool favoritesLoading READ favoritesLoading NOTIFY favoritesChanged)
    Q_PROPERTY(bool favoritesBusy READ favoritesBusy NOTIFY favoritesChanged)
    Q_PROPERTY(QString singleError READ singleError NOTIFY singleChanged)
    Q_PROPERTY(QString allError READ allError NOTIFY allChanged)
    Q_PROPERTY(QString conversionError READ conversionError NOTIFY conversionChanged)
    Q_PROPERTY(QString favoritesError READ favoritesError NOTIFY favoritesChanged)
    Q_PROPERTY(QString notification READ notification NOTIFY favoritesChanged)

public:
    explicit CurrencyController(application::ApplicationServiceGraph *graph,
                                QObject *parent = nullptr);

    models::CurrencyRatesModel *allRates() noexcept { return &m_allRates; }
    models::CurrencyFavoritesModel *favorites() noexcept { return &m_favorites; }
    QVariantMap singleResult() const { return m_singleResult; }
    QVariantMap conversionResult() const { return m_conversionResult; }
    QString allBase() const { return m_allBase; }
    QString allDate() const { return m_allDate; }
    bool singleLoading() const noexcept { return m_singleLoading; }
    bool allLoading() const noexcept { return m_allLoading; }
    bool conversionLoading() const noexcept { return m_conversionLoading; }
    bool favoritesLoading() const noexcept { return m_favoritesLoading; }
    bool favoritesBusy() const noexcept { return m_favoritesBusy; }
    QString singleError() const { return m_singleError; }
    QString allError() const { return m_allError; }
    QString conversionError() const { return m_conversionError; }
    QString favoritesError() const { return m_favoritesError; }
    QString notification() const { return m_notification; }

    Q_INVOKABLE void lookupSingle(QString baseCode, QString quoteCode);
    Q_INVOKABLE void loadAllRates(QString baseCode);
    Q_INVOKABLE void convert(double amount, QString baseCode, QString quoteCode);
    Q_INVOKABLE void addFavorite(QString baseCode, QString quoteCode);
    Q_INVOKABLE void removeFavorite(QString baseCode, QString quoteCode);
    Q_INVOKABLE void selectFavorite(QString baseCode, QString quoteCode);
    Q_INVOKABLE void refreshFavorites();
    Q_INVOKABLE void setAllRatesFilter(QString filter);
    Q_INVOKABLE void clearNotification();

signals:
    void singleChanged();
    void allChanged();
    void conversionChanged();
    void favoritesChanged();
    void favoriteSelected(QString baseCode, QString quoteCode);

private:
    enum class Kind { LoadFavorites, AddFavorite, RemoveFavorite,
                      SingleRequest, SingleParse, AllRequest, AllParse,
                      ConversionRequest, ConversionParse,
                      FavoriteRequest, FavoriteParse };
    struct Pending final {
        Kind kind{Kind::LoadFavorites};
        QString base;
        QString quote;
        double amount{0.0};
        quint64 generation{0};
    };

    void onLifecycleState(application::ApplicationLifecycle::State state);
    void onStorageFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    void onHttpsFinished(quint64 id, const QByteArray &body, domain::AppError error);
    void onValueJobFinished(quint64 id, const QVariantMap &values, domain::AppError error);
    quint64 submitStorage(Kind kind, storage::StorageCommand command,
                          QString freshnessKey, QString base = {}, QString quote = {});
    quint64 submitNetwork(Kind kind, providers::ProviderRequest request,
                          QString freshnessKey, const Pending &pending);
    quint64 submitParse(Kind kind, QByteArray body, QString freshnessKey,
                        const Pending &pending);
    void cancelPending(quint64 id);
    void cancelKind(Kind first, Kind second);
    void loadFavoritesSnapshot(bool refreshAfterLoad);
    void dispatchFavoriteRates();
    void completeFavoriteRate(const Pending &pending, bool found, double rate,
                              QString date, QString status, QString error = {});
    void setSingleFailure(domain::AppError error, const QString &fallback);
    void setAllFailure(domain::AppError error, const QString &fallback);
    void setConversionFailure(domain::AppError error, const QString &fallback);
    void failFavoriteRequest(const Pending &pending, domain::AppError error);

    application::ApplicationServiceGraph *m_graph{nullptr};
    models::CurrencyRatesModel m_allRates;
    models::CurrencyFavoritesModel m_favorites;
    QHash<quint64, Pending> m_pending;
    QVariantMap m_singleResult;
    QVariantMap m_conversionResult;
    QString m_allBase;
    QString m_allDate;
    QString m_singleError;
    QString m_allError;
    QString m_conversionError;
    QString m_favoritesError;
    QString m_notification;
    quint64 m_singleGeneration{0};
    quint64 m_allGeneration{0};
    quint64 m_conversionGeneration{0};
    quint64 m_favoritesGeneration{0};
    quint64 m_singleOperation{0};
    quint64 m_allOperation{0};
    quint64 m_conversionOperation{0};
    quint64 m_favoritesLoadOperation{0};
    quint64 m_mutationOperation{0};
    QQueue<storage::CurrencyPair> m_waitingFavoriteRates;
    QSet<quint64> m_favoriteRateOperations;
    int m_activeFavoriteRateRequests{0};
    bool m_ready{false};
    bool m_refreshAfterFavoriteLoad{true};
    bool m_singleLoading{false};
    bool m_allLoading{false};
    bool m_conversionLoading{false};
    bool m_favoritesLoading{false};
    bool m_favoritesBusy{false};
};

} // namespace something::controllers

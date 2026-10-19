#pragma once

#include <QAbstractListModel>
#include <QVariantMap>

namespace something::models {

class CurrencyRatesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY countChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)

public:
    enum Role { CodeRole = Qt::UserRole + 1, RateRole, DateRole };
    Q_ENUM(Role)
    explicit CurrencyRatesModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_visibleRows.size(); }
    int totalCount() const noexcept { return m_allRows.size(); }
    QString filter() const { return m_filter; }
    void setFilter(QString filter);
    void replace(QVariantList sortedRows);
    void clear();

signals:
    void countChanged();
    void filterChanged();

private:
    void rebuild();
    QVariantList m_allRows;
    QVariantList m_visibleRows;
    QString m_filter;
};

class CurrencyFavoritesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { BaseCodeRole = Qt::UserRole + 1, QuoteCodeRole, PairCodeRole,
                RateFoundRole, RateRole, RateDateRole, RateStatusRole, ErrorMessageRole };
    Q_ENUM(Role)
    explicit CurrencyFavoritesModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_rows.size(); }
    Q_INVOKABLE QVariantMap rowAt(int row) const;
    bool contains(const QString &base, const QString &quote) const;
    void replace(QVariantList rows);
    void setRate(const QString &base, const QString &quote, bool found,
                 double rate, QString date, QString status, QString errorMessage = {});
    void markAll(QString status, QString errorMessage = {});

signals:
    void countChanged();

private:
    QVariantList m_rows;
};

} // namespace something::models

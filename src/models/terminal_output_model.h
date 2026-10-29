#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QVariant>
#include <QVector>

namespace something::models {

class TerminalOutputModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int retainedBytes READ retainedBytes NOTIFY retainedBytesChanged)

public:
    enum Role { TextRole = Qt::UserRole + 1, KindRole, SequenceRole, TimestampRole };
    Q_ENUM(Role)

    static constexpr int MaximumRows = 400;
    static constexpr int MaximumTranscriptBytes = 256 * 1024;
    static constexpr int MaximumRowBytes = 16 * 1024;

    explicit TerminalOutputModel(QObject *parent = nullptr);
    int count() const noexcept { return static_cast<int>(m_entries.size()); }
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int retainedBytes() const noexcept { return m_retainedBytes; }

    Q_INVOKABLE void clear();
    void append(QString text, QString kind);

signals:
    void aboutToAppend();
    void countChanged();
    void retainedBytesChanged();

private:
    struct Entry final {
        QString text;
        QString kind;
        quint64 sequence{0};
        int byteSize{0};
        QString timestamp;
    };

    QVector<Entry> m_entries;
    int m_retainedBytes{0};
    quint64 m_nextSequence{1};
};

} // namespace something::models

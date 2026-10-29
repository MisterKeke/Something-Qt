#include "terminal_output_model.h"

#include <QByteArray>
#include <QDateTime>
#include <utility>

namespace something::models {

TerminalOutputModel::TerminalOutputModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int TerminalOutputModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_entries.size());
}

QVariant TerminalOutputModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};
    const Entry &entry = m_entries.at(index.row());
    switch (role) {
    case TextRole: return entry.text;
    case KindRole: return entry.kind;
    case SequenceRole: return QVariant::fromValue<qulonglong>(entry.sequence);
    case TimestampRole: return entry.timestamp;
    default: return {};
    }
}

QHash<int, QByteArray> TerminalOutputModel::roleNames() const
{
    return {{TextRole, "text"}, {KindRole, "kind"}, {SequenceRole, "sequence"},
            {TimestampRole, "timestamp"}};
}

void TerminalOutputModel::clear()
{
    if (m_entries.isEmpty()) return;
    beginResetModel();
    m_entries.clear();
    m_retainedBytes = 0;
    endResetModel();
    emit countChanged();
    emit retainedBytesChanged();
}

void TerminalOutputModel::append(QString text, QString kind)
{
    static const QByteArray marker("\n[transcript row truncated]");
    QByteArray encoded = text.toUtf8();
    if (encoded.size() > MaximumRowBytes) {
        const int contentLimit = MaximumRowBytes - static_cast<int>(marker.size());
        QString shortened = QString::fromUtf8(encoded.left(contentLimit));
        while (shortened.toUtf8().size() > contentLimit) shortened.chop(1);
        text = shortened + QString::fromUtf8(marker);
        encoded = text.toUtf8();
    }
    if (encoded.size() > MaximumTranscriptBytes) return;

    emit aboutToAppend();
    while (!m_entries.isEmpty()
           && (m_entries.size() >= MaximumRows
               || m_retainedBytes + encoded.size() > MaximumTranscriptBytes)) {
        beginRemoveRows({}, 0, 0);
        m_retainedBytes -= m_entries.constFirst().byteSize;
        m_entries.removeFirst();
        endRemoveRows();
    }

    const int row = m_entries.size();
    beginInsertRows({}, row, row);
    const int byteSize = static_cast<int>(encoded.size());
    m_retainedBytes += byteSize;
    m_entries.push_back({std::move(text), std::move(kind), m_nextSequence++, byteSize,
                         QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))});
    endInsertRows();
    emit countChanged();
    emit retainedBytesChanged();
}

} // namespace something::models

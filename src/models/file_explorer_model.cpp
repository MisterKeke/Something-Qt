#include "file_explorer_model.h"

#include <utility>

namespace something::models {

FileExplorerModel::FileExplorerModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int FileExplorerModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant FileExplorerModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) return {};
    const FileExplorerEntry &entry = m_entries.at(index.row());
    switch (role) {
    case NameRole: return entry.name;
    case RelativePathRole: return entry.relativePath;
    case TypeRole: return entry.type;
    case ModifiedRole: return entry.modified;
    case SizeRole: return QVariant::fromValue(entry.size);
    case DirectoryRole: return entry.directory;
    case CanNavigateRole: return entry.canNavigate;
    case CanOpenRole: return entry.canOpen;
    case CanRecycleRole: return entry.canRecycle;
    case SelectedRole: return entry.relativePath == m_selectedPath;
    default: return {};
    }
}

QHash<int, QByteArray> FileExplorerModel::roleNames() const
{
    return {{NameRole, "name"}, {RelativePathRole, "relativePath"},
            {TypeRole, "entryType"}, {ModifiedRole, "modified"}, {SizeRole, "size"},
            {DirectoryRole, "directory"}, {CanNavigateRole, "canNavigate"},
            {CanOpenRole, "canOpen"}, {CanRecycleRole, "canRecycle"},
            {SelectedRole, "selected"}};
}

void FileExplorerModel::updateSelection(const QString &path)
{
    const QString previousPath = m_selectedPath;
    const QString previousName = m_selectedName;
    const bool previousCanRecycle = m_selectedCanRecycle;
    m_selectedPath = path;
    m_selectedName.clear();
    m_selectedCanRecycle = false;
    const int selectedRow = rowForPath(path);
    if (selectedRow >= 0) {
        m_selectedName = m_entries.at(selectedRow).name;
        m_selectedCanRecycle = m_entries.at(selectedRow).canRecycle;
    }
    if (previousPath == m_selectedPath && previousName == m_selectedName
        && previousCanRecycle == m_selectedCanRecycle) return;
    const int oldRow = rowForPath(previousPath);
    if (oldRow >= 0) emit dataChanged(index(oldRow), index(oldRow), {SelectedRole});
    if (selectedRow >= 0) emit dataChanged(index(selectedRow), index(selectedRow), {SelectedRole});
    emit selectionChanged();
}

void FileExplorerModel::replace(QVector<FileExplorerEntry> entries)
{
    const QString selected = m_selectedPath;
    const int oldCount = m_entries.size();
    beginResetModel();
    m_entries = std::move(entries);
    endResetModel();
    if (oldCount != m_entries.size()) emit countChanged();
    updateSelection(selected);
}

void FileExplorerModel::append(QVector<FileExplorerEntry> entries)
{
    if (entries.isEmpty()) return;
    const int first = m_entries.size();
    const int last = first + entries.size() - 1;
    beginInsertRows({}, first, last);
    m_entries += std::move(entries);
    endInsertRows();
    emit countChanged();
    const int selectedRow = rowForPath(m_selectedPath);
    if (selectedRow >= 0) emit dataChanged(index(selectedRow), index(selectedRow), {SelectedRole});
}

void FileExplorerModel::clear()
{
    if (m_entries.isEmpty() && m_selectedPath.isEmpty()) return;
    const int oldCount = m_entries.size();
    beginResetModel();
    m_entries.clear();
    endResetModel();
    if (oldCount) emit countChanged();
    updateSelection({});
}

bool FileExplorerModel::selectPath(const QString &relativePath)
{
    if (!relativePath.isEmpty() && rowForPath(relativePath) < 0) return false;
    updateSelection(relativePath);
    return true;
}

int FileExplorerModel::rowForPath(const QString &relativePath) const
{
    if (relativePath.isEmpty()) return -1;
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries.at(row).relativePath == relativePath) return row;
    return -1;
}

FileExplorerEntry FileExplorerModel::entryAt(int row) const
{
    return row >= 0 && row < m_entries.size() ? m_entries.at(row) : FileExplorerEntry{};
}

} // namespace something::models

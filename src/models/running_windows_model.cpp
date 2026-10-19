#include "running_windows_model.h"

#include <algorithm>

namespace something::models {

RunningWindowsModel::RunningWindowsModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int RunningWindowsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_windows.size();
}

QVariant RunningWindowsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_windows.size()) return {};
    const platform::RunningWindow &window = m_windows.at(index.row());
    switch (role) {
    case WindowIdRole: return window.id;
    case TitleRole: return window.title;
    case ProcessNameRole: return window.processName;
    case IconDataUrlRole: return window.iconDataUrl;
    case MinimizedRole: return window.minimized;
    case ActiveRole: return window.active;
    default: return {};
    }
}

QHash<int, QByteArray> RunningWindowsModel::roleNames() const
{
    return {{WindowIdRole, "windowId"}, {TitleRole, "title"},
            {ProcessNameRole, "processName"}, {IconDataUrlRole, "iconDataUrl"},
            {MinimizedRole, "minimized"}, {ActiveRole, "active"}};
}

bool RunningWindowsModel::less(const platform::RunningWindow &left,
                               const platform::RunningWindow &right)
{
    const int title = QString::compare(left.title, right.title, Qt::CaseInsensitive);
    if (title != 0) return title < 0;
    const int process = QString::compare(left.processName, right.processName, Qt::CaseInsensitive);
    if (process != 0) return process < 0;
    return left.id < right.id;
}

void RunningWindowsModel::applyRowChange(int row, const platform::RunningWindow &window)
{
    const platform::RunningWindow old = m_windows.at(row);
    if (old.id == window.id && old.title == window.title && old.processName == window.processName
        && old.iconDataUrl == window.iconDataUrl && old.minimized == window.minimized
        && old.active == window.active) return;
    m_windows[row] = window;
    QVector<int> roles;
    if (old.id != window.id) roles.push_back(WindowIdRole);
    if (old.title != window.title) roles.push_back(TitleRole);
    if (old.processName != window.processName) roles.push_back(ProcessNameRole);
    if (old.iconDataUrl != window.iconDataUrl) roles.push_back(IconDataUrlRole);
    if (old.minimized != window.minimized) roles.push_back(MinimizedRole);
    if (old.active != window.active) roles.push_back(ActiveRole);
    emit dataChanged(index(row), index(row), roles);
}

void RunningWindowsModel::replace(QVector<platform::RunningWindow> windows)
{
    std::sort(windows.begin(), windows.end(), less);
    const int oldCount = m_windows.size();
    for (int target = 0; target < windows.size(); ++target) {
        const QString &wantedId = windows.at(target).id;
        if (target < m_windows.size() && m_windows.at(target).id == wantedId) {
            applyRowChange(target, windows.at(target));
            continue;
        }
        int source = -1;
        for (int row = target + 1; row < m_windows.size(); ++row) {
            if (m_windows.at(row).id == wantedId) { source = row; break; }
        }
        if (source >= 0) {
            beginMoveRows({}, source, source, {}, target);
            m_windows.move(source, target);
            endMoveRows();
            applyRowChange(target, windows.at(target));
        } else {
            beginInsertRows({}, target, target);
            m_windows.insert(target, windows.at(target));
            endInsertRows();
        }
    }
    while (m_windows.size() > windows.size()) {
        const int last = m_windows.size() - 1;
        beginRemoveRows({}, last, last);
        m_windows.removeAt(last);
        endRemoveRows();
    }
    if (oldCount != m_windows.size()) emit countChanged();
}

int RunningWindowsModel::rowForId(const QString &id) const
{
    for (int row = 0; row < m_windows.size(); ++row)
        if (m_windows.at(row).id == id) return row;
    return -1;
}

QString RunningWindowsModel::idAt(int row) const
{
    return row >= 0 && row < m_windows.size() ? m_windows.at(row).id : QString();
}

} // namespace something::models

#pragma once

#include <QAbstractListModel>
#include <QVector>

namespace something::models {

struct FileExplorerEntry final {
    QString name;
    QString relativePath;
    QString type;
    QString modified;
    qulonglong size{0};
    bool directory{false};
    bool canNavigate{false};
    bool canOpen{false};
    bool canRecycle{false};
};

class FileExplorerModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString selectedPath READ selectedPath NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedName READ selectedName NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedCanRecycle READ selectedCanRecycle NOTIFY selectionChanged)

public:
    enum Role { NameRole = Qt::UserRole + 1, RelativePathRole, TypeRole, ModifiedRole,
                SizeRole, DirectoryRole, CanNavigateRole, CanOpenRole, CanRecycleRole,
                SelectedRole };
    Q_ENUM(Role)

    explicit FileExplorerModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const noexcept { return m_entries.size(); }
    QString selectedPath() const { return m_selectedPath; }
    QString selectedName() const { return m_selectedName; }
    bool selectedCanRecycle() const noexcept { return m_selectedCanRecycle; }

    void replace(QVector<FileExplorerEntry> entries);
    void append(QVector<FileExplorerEntry> entries);
    void clear();
    bool selectPath(const QString &relativePath);
    int rowForPath(const QString &relativePath) const;
    FileExplorerEntry entryAt(int row) const;

signals:
    void countChanged();
    void selectionChanged();

private:
    void updateSelection(const QString &path);
    QVector<FileExplorerEntry> m_entries;
    QString m_selectedPath;
    QString m_selectedName;
    bool m_selectedCanRecycle{false};
};

} // namespace something::models

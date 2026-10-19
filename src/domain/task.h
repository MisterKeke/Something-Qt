#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace something::domain {

struct Subtask final {
    qint64 id{0};
    QString title;
    bool completed{false};
    int position{0};
};

struct SubtaskInput final {
    qint64 id{0}; // zero creates a new subtask
    QString title;
    bool completed{false};
};

struct Task final {
    qint64 id{0};
    QString title;
    QString description;
    bool completed{false};
    QString createdAt;
    QString updatedAt;
    QString dueDate;
    QString priority{QStringLiteral("medium")};
    QString difficulty;
    qint64 revision{0};
    QStringList tags;
    QVector<Subtask> subtasks;
};

struct TaskInput final {
    QString title;
    QString description;
    QString dueDate; // empty means no due date
    QString priority{QStringLiteral("medium")};
    std::optional<QString> difficulty; // omitted on update preserves the existing value
    std::optional<QStringList> tags; // omitted on update preserves existing tags
    std::optional<QVector<SubtaskInput>> subtasks; // omitted on update preserves existing subtasks
};

struct TaskFilter final {
    QString query;
    QStringList tags; // every requested tag is required (AND semantics)
    QString completion{QStringLiteral("all")}; // all, complete, incomplete
    QString priority;
    QString difficulty;
    QString dueDate;
    QString dueFrom;
    QString dueTo;
    bool overdue{false};
    bool undated{false};
    QString sort{QStringLiteral("created_at")};
    QString direction{QStringLiteral("desc")};
    int limit{100};
    int offset{0};
};

} // namespace something::domain

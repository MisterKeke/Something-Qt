#include "models/terminal_output_model.h"

#include <QtTest>

using something::models::TerminalOutputModel;

class TerminalOutputModelTest final : public QObject {
    Q_OBJECT
private slots:
    void trimsRowsAndUtf8Bytes();
    void clearDropsTranscriptOnly();
};

void TerminalOutputModelTest::trimsRowsAndUtf8Bytes()
{
    TerminalOutputModel model;
    for (int i = 0; i < TerminalOutputModel::MaximumRows + 12; ++i)
        model.append(QStringLiteral("row-%1").arg(i), QStringLiteral("normal"));
    QCOMPARE(model.count(), TerminalOutputModel::MaximumRows);
    QCOMPARE(model.data(model.index(0, 0), TerminalOutputModel::TextRole).toString(),
             QStringLiteral("row-12"));

    const QString unicode(TerminalOutputModel::MaximumRowBytes, QChar(0x732b));
    model.clear();
    model.append(unicode, QStringLiteral("success"));
    const QString retained = model.data(model.index(0, 0), TerminalOutputModel::TextRole).toString();
    QVERIFY(retained.toUtf8().size() <= TerminalOutputModel::MaximumRowBytes);
    QVERIFY(model.retainedBytes() <= TerminalOutputModel::MaximumTranscriptBytes);
    QVERIFY(retained.contains(QStringLiteral("transcript row truncated")));

    model.clear();
    const QString large(TerminalOutputModel::MaximumRowBytes, QLatin1Char('x'));
    for (int i = 0; i < 24; ++i) model.append(large, QStringLiteral("normal"));
    QVERIFY(model.count() < 24);
    QVERIFY(model.retainedBytes() <= TerminalOutputModel::MaximumTranscriptBytes);
}

void TerminalOutputModelTest::clearDropsTranscriptOnly()
{
    TerminalOutputModel model;
    model.append(QStringLiteral("pending request"), QStringLiteral("pending"));
    model.clear();
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.retainedBytes(), 0);
    model.append(QStringLiteral("late result"), QStringLiteral("success"));
    QCOMPARE(model.count(), 1);
    QCOMPARE(model.data(model.index(0, 0), TerminalOutputModel::TextRole).toString(),
             QStringLiteral("late result"));
}

QTEST_MAIN(TerminalOutputModelTest)
#include "terminal_output_model_test.moc"

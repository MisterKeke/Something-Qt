#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QWindow>

namespace something::controllers {

class CursorTrailController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QPointF position READ position NOTIFY positionChanged)
    Q_PROPERTY(bool inside READ inside NOTIFY insideChanged)
    Q_PROPERTY(bool pressed READ pressed NOTIFY pressedChanged)
public:
    explicit CursorTrailController(QObject *parent = nullptr) : QObject(parent) {}
    ~CursorTrailController() override;
    void attachToWindow(QWindow *window);
    QPointF position() const { return m_position; }
    bool inside() const { return m_inside; }
    bool pressed() const { return m_pressed; }
signals:
    void positionChanged();
    void insideChanged();
    void pressedChanged();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void updatePosition(const QPointF &position);
    void setPressed(bool pressed);
    void clear();
    QPointer<QWindow> m_window;
    QPointF m_position;
    bool m_inside{false};
    bool m_pressed{false};
    bool m_blocked{false};
    bool m_cursorHidden{false};
    QMetaObject::Connection m_windowDestroyed;
};

} // namespace something::controllers

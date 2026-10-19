#include "cursor_trail_controller.h"

#include <QEnterEvent>
#include <QEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QRectF>

namespace something::controllers {

CursorTrailController::~CursorTrailController()
{
    if (m_window) m_window->removeEventFilter(this);
    clear();
}

void CursorTrailController::attachToWindow(QWindow *window)
{
    if (m_window) m_window->removeEventFilter(this);
    disconnect(m_windowDestroyed);
    clear();
    m_blocked = false;
    m_window = window;
    if (m_window) {
        m_window->installEventFilter(this);
        m_windowDestroyed = connect(m_window, &QObject::destroyed, this, [this] { clear(); });
    }
}

void CursorTrailController::clear()
{
    setPressed(false);
    if (m_cursorHidden) {
        QGuiApplication::restoreOverrideCursor();
        m_cursorHidden = false;
    }
    if (!m_inside) return;
    m_inside = false;
    emit insideChanged();
}

void CursorTrailController::setPressed(bool pressed)
{
    if (pressed == m_pressed) return;
    m_pressed = pressed;
    emit pressedChanged();
}

void CursorTrailController::updatePosition(const QPointF &position)
{
    if (!m_window || m_blocked || !m_window->isVisible() || !m_window->isActive()
        || !QRectF(QPointF(), QSizeF(m_window->size())).contains(position)) {
        clear();
        return;
    }
    // Publish the entry position before showing the effect, so followers can
    // snap to it while hidden rather than sweeping in from the last location.
    if (position != m_position) {
        m_position = position;
        emit positionChanged();
    }
    if (!m_inside) {
        m_inside = true;
        // Qt Quick controls can choose their own arrow/hand/I-beam. A scoped
        // application override prevents them showing through the custom ring.
        // clear() pairs this push on every exit/block/deactivation path.
        QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
        m_cursorHidden = true;
        emit insideChanged();
    }
}

bool CursorTrailController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_window.data()) return false;
    switch (event->type()) {
    case QEvent::Enter:
        updatePosition(static_cast<QEnterEvent *>(event)->position());
        break;
    case QEvent::HoverMove:
        updatePosition(static_cast<QHoverEvent *>(event)->position());
        break;
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        updatePosition(mouse->position());
        setPressed(m_inside && mouse->buttons().testFlag(Qt::LeftButton));
        break;
    }
    case QEvent::WindowBlocked:
        m_blocked = true;
        clear();
        break;
    case QEvent::WindowUnblocked:
        m_blocked = false;
        break;
    case QEvent::Leave:
    case QEvent::Hide:
    case QEvent::Close:
    case QEvent::Resize:
    case QEvent::WindowDeactivate:
    case QEvent::NonClientAreaMouseMove:
    case QEvent::NonClientAreaMouseButtonPress:
        clear();
        break;
    default:
        break;
    }
    // Observe only this window, without accepting or consuming input. No
    // desktop hook, mouse grab or polling timer is used. The blank override
    // exists only while this window's client area displays the custom cursor.
    return false;
}

} // namespace something::controllers

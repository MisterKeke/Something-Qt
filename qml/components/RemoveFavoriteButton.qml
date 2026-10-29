import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

Button {
    id: root
    required property string pairName
    implicitWidth: 32
    implicitHeight: 32
    padding: 8
    hoverEnabled: true
    Accessible.name: qsTr("Remove favorite %1").arg(pairName)
    ToolTip.visible: hovered || activeFocus
    ToolTip.delay: 500
    ToolTip.text: Accessible.name
    scale: down ? 0.92 : 1
    opacity: enabled ? 1 : 0.4
    Behavior on scale { NumberAnimation { duration: ThemeModule.Theme.controlMotion; easing.type: Easing.OutCubic } }
    Behavior on opacity { NumberAnimation { duration: ThemeModule.Theme.controlMotion } }

    contentItem: Image {
        source: root.hovered || root.down ? "qrc:/something/assets/icons/trash-active.svg"
                                         : "qrc:/something/assets/icons/trash.svg"
        sourceSize: Qt.size(32, 32)
        fillMode: Image.PreserveAspectFit
    }
    background: Rectangle {
        radius: ThemeModule.Theme.controlRadius
        color: root.down ? ThemeModule.Theme.errorSurface
               : root.hovered ? ThemeModule.Theme.destructiveHover : ThemeModule.Theme.destructiveSurface
        border.color: root.activeFocus ? ThemeModule.Theme.focusHighlight : ThemeModule.Theme.error
        border.width: root.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
        Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        Behavior on border.color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
    }
}

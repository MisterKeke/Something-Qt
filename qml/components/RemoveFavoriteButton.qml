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
        radius: 9
        color: root.down ? "#482436" : root.hovered ? "#342231" : "#151b2b"
        border.color: root.activeFocus ? ThemeModule.Theme.cyan
                     : root.hovered ? "#9b4a65" : ThemeModule.Theme.border
        border.width: root.activeFocus ? 2 : 1
        Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        Behavior on border.color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
    }
}

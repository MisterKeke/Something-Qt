import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

Button {
    id: root

    property bool selected: false
    property color tint: ThemeModule.Theme.accent
    hoverEnabled: true

    implicitHeight: 34
    leftPadding: 13
    rightPadding: 13
    font.family: ThemeModule.Theme.displayFont
    font.pixelSize: 12
    Accessible.role: Accessible.Button
    Accessible.name: text

    contentItem: Text {
        text: root.text
        textFormat: Text.PlainText
        color: root.selected ? ThemeModule.Theme.text : ThemeModule.Theme.textDim
        Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        font: root.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: height / 2
        color: root.selected ? Qt.darker(root.tint, 1.8) : "#171a2a"
        border.color: root.activeFocus ? ThemeModule.Theme.cyan
                      : root.selected ? root.tint : Qt.darker(root.tint, 1.45)
        border.width: root.activeFocus ? 2 : 1
        Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        Behavior on border.color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
    }
}

import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

TextField {
    id: root

    property string accessibleLabel: placeholderText

    implicitHeight: 42
    leftPadding: 12
    rightPadding: 12
    color: ThemeModule.Theme.text
    placeholderTextColor: ThemeModule.Theme.textDim
    selectionColor: ThemeModule.Theme.accent
    selectedTextColor: ThemeModule.Theme.text
    font.family: ThemeModule.Theme.bodyFont
    font.pixelSize: 14
    Accessible.role: Accessible.EditableText
    Accessible.name: accessibleLabel

    background: Rectangle {
        radius: 9
        color: "#0d101a"
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? ThemeModule.Theme.cyan : ThemeModule.Theme.border
        Behavior on border.color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
    }
}

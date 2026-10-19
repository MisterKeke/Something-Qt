import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

Button {
    id: root

    property bool destructive: false
    property bool quiet: false
    hoverEnabled: true
    scale: down ? 0.98 : 1
    Behavior on scale { NumberAnimation { duration: ThemeModule.Theme.controlMotion; easing.type: Easing.OutCubic } }

    implicitHeight: 40
    implicitWidth: Math.max(88, label.implicitWidth + 30)
    leftPadding: 15
    rightPadding: 15
    topPadding: 8
    bottomPadding: 8
    font.family: ThemeModule.Theme.displayFont
    font.pixelSize: 13
    font.weight: Font.DemiBold
    Accessible.role: Accessible.Button
    Accessible.name: text

    contentItem: Text {
        id: label
        text: root.text
        color: root.enabled ? ThemeModule.Theme.text
                             : Qt.darker(ThemeModule.Theme.textDim, 1.1)
        Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        font: root.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: 10
        color: root.destructive
               ? (root.down ? "#54202f" : "#341924")
               : root.quiet
                 ? (root.down ? "#292d43" : root.hovered ? "#22263a" : "#171a2a")
                 : (root.down ? "#6474df" : root.hovered ? "#8998ff" : "#7c8cff")
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? ThemeModule.Theme.cyan
                     : root.destructive ? "#884155"
                     : root.quiet ? ThemeModule.Theme.border : "#9ca9ff"
        Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        Behavior on border.color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: parent.radius + 4
            color: "transparent"
            border.color: ThemeModule.Theme.accentHover
            border.width: 2
            visible: root.activeFocus
            z: -1
        }
    }
}

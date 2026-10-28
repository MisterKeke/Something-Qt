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
    leftPadding: ThemeModule.Theme.controlPadding
    rightPadding: ThemeModule.Theme.controlPadding
    topPadding: 8
    bottomPadding: 8
    font.family: ThemeModule.Theme.fontFamily
    font.pixelSize: ThemeModule.Theme.labelFontSize
    font.weight: Font.DemiBold
    Accessible.role: Accessible.Button
    Accessible.name: text

    contentItem: Text {
        id: label
        text: root.text
        color: root.enabled ? ThemeModule.Theme.textPrimary
                             : ThemeModule.Theme.textDisabled
        Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        font: root.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: ThemeModule.Theme.controlRadius
        color: !root.enabled ? ThemeModule.Theme.disabledSurface
               : root.destructive
               ? (root.down ? ThemeModule.Theme.errorSurface
                  : root.hovered ? ThemeModule.Theme.destructiveHover : ThemeModule.Theme.destructiveSurface)
               : root.quiet
                 ? (root.down ? ThemeModule.Theme.shadowBorder
                              : root.hovered ? ThemeModule.Theme.hoverSurface : ThemeModule.Theme.raisedSurface)
                 : (root.down ? ThemeModule.Theme.primaryButtonPressed
                              : root.hovered ? ThemeModule.Theme.primaryButtonHover : ThemeModule.Theme.selectionBlue)
        border.width: root.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
        border.color: !root.enabled ? ThemeModule.Theme.borderColor
                     : root.activeFocus ? ThemeModule.Theme.focusHighlight
                     : root.destructive ? ThemeModule.Theme.error
                     : root.quiet ? ThemeModule.Theme.borderColor : ThemeModule.Theme.strongBorder
        Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        Behavior on border.color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: parent.radius + 2
            color: "transparent"
            border.color: ThemeModule.Theme.focusHighlight
            border.width: 2
            visible: root.activeFocus
            z: -1
        }
    }
}

import QtQuick
import Something.App as ThemeModule

Rectangle {
    id: root

    property int padding: ThemeModule.Theme.panelPadding
    property color fillColor: ThemeModule.Theme.panelSurface
    default property alias contentData: content.data

    radius: ThemeModule.Theme.panelRadius
    color: root.fillColor
    border.color: ThemeModule.Theme.borderColor
    border.width: ThemeModule.Theme.panelBorderWidth
    implicitWidth: content.implicitWidth + root.padding * 2
    implicitHeight: content.implicitHeight + root.padding * 2
    clip: true

    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        color: "transparent"
        border.color: Qt.alpha(ThemeModule.Theme.strongBorder, 0.2)
        border.width: 1
    }

    Column {
        id: content
        anchors.fill: parent
        anchors.margins: root.padding
        spacing: ThemeModule.Theme.cardContentSpacing
    }
}

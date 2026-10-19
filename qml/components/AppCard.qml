import QtQuick
import Something.App as ThemeModule

Rectangle {
    id: root

    property int padding: 18
    property color fillColor: "#e611131d"
    default property alias contentData: content.data

    radius: ThemeModule.Theme.cornerRadius
    color: root.fillColor
    border.color: "#397c8cff"
    border.width: 1
    implicitWidth: content.implicitWidth + root.padding * 2
    implicitHeight: content.implicitHeight + root.padding * 2
    clip: true

    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        color: "transparent"
        border.color: "#05ffffff"
        border.width: 1
    }

    Column {
        id: content
        anchors.fill: parent
        anchors.margins: root.padding
        spacing: 10
    }
}

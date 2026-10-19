import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

Item {
    id: root

    property string state: "empty"
    property string title: ""
    property string message: ""

    implicitHeight: 112

    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width - 28, 520)
        spacing: 8

        BusyIndicator {
            anchors.horizontalCenter: parent.horizontalCenter
            running: root.state === "loading"
            visible: running
            width: 32
            height: 32
        }

        Label {
            width: parent.width
            text: root.title
            visible: text.length > 0
            color: root.state === "error" ? ThemeModule.Theme.error : ThemeModule.Theme.text
            font.family: ThemeModule.Theme.displayFont
            font.pixelSize: 15
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        Label {
            width: parent.width
            text: root.message
            visible: text.length > 0
            color: ThemeModule.Theme.textDim
            font.family: ThemeModule.Theme.bodyFont
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }
}

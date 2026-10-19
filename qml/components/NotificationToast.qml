import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

Rectangle {
    id: root

    property string message: ""
    property string displayedMessage: ""
    signal dismissed()

    visible: opacity > 0
    implicitWidth: Math.min(parent ? parent.width - 32 : 420, toastText.implicitWidth + 38)
    implicitHeight: 48
    radius: 12
    color: "#ec171a2a"
    border.color: ThemeModule.Theme.cyan
    border.width: 1
    opacity: root.message.length > 0 ? 1 : 0
    Accessible.role: Accessible.AlertMessage
    Accessible.name: message

    Label {
        id: toastText
        anchors.centerIn: parent
        width: Math.min(480, parent.width - 32)
        text: root.displayedMessage
        color: ThemeModule.Theme.text
        font.family: ThemeModule.Theme.bodyFont
        font.pixelSize: 13
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
    }

    Timer {
        id: dismissTimer
        interval: 3600
        onTriggered: root.dismissed()
    }

    onMessageChanged: {
        if (message.length > 0)
            displayedMessage = message
        if (message.length > 0)
            dismissTimer.restart()
    }

    Behavior on opacity { NumberAnimation { duration: ThemeModule.Theme.pageMotion; easing.type: Easing.OutCubic } }
}

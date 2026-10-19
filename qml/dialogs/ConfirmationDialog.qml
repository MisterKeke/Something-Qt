import QtQuick
import QtQuick.Controls
import "../components" as Shared
import Something.App as ThemeModule

Dialog {
    id: root

    property string message: ""
    property string confirmText: qsTr("Confirm")
    property bool destructive: false
    signal confirmed()

    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(440, parent ? parent.width - 40 : 400)
    padding: 22
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    title: qsTr("Please confirm")
    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: ThemeModule.Theme.pageMotion; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: ThemeModule.Theme.pageMotion; easing.type: Easing.OutCubic }
        }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: ThemeModule.Theme.controlMotion } }

    onAccepted: root.confirmed()

    contentItem: Label {
        text: root.message
        color: ThemeModule.Theme.textDim
        font.family: ThemeModule.Theme.bodyFont
        font.pixelSize: 14
        wrapMode: Text.Wrap
    }

    footer: DialogButtonBox {
        alignment: Qt.AlignRight

        Shared.AppButton {
            text: qsTr("Cancel")
            quiet: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        Shared.AppButton {
            text: root.confirmText
            destructive: root.destructive
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
    }

    background: Rectangle {
        color: ThemeModule.Theme.panelAlt
        border.color: ThemeModule.Theme.accent
        border.width: 1
        radius: 16
    }
}

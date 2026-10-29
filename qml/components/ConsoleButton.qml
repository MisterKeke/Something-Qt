import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Something.App as ThemeModule

Button {
    id: root
    property string symbol: "chevron"
    property color accent: "#00cfff"
    property bool primary: false
    implicitHeight: primary ? 56 : 46
    implicitWidth: Math.max(primary ? 152 : 116, caption.implicitWidth + 72)
    leftPadding: 20
    rightPadding: 20
    hoverEnabled: true
    opacity: enabled ? 1 : 0.45
    scale: down ? 0.98 : 1
    font.family: ThemeModule.Theme.bodyFont
    font.pixelSize: primary ? 16 : 14
    font.weight: Font.DemiBold
    Accessible.role: Accessible.Button
    Accessible.name: text
    Behavior on scale { NumberAnimation { duration: 100 } }

    contentItem: RowLayout {
        spacing: 12
        ConsoleIcon {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 21
            Layout.preferredHeight: 21
            symbol: root.symbol
            ink: root.primary ? "#f4f7ff" : root.accent
        }
        Text {
            id: caption
            Layout.fillWidth: true
            text: root.text
            textFormat: Text.PlainText
            color: "#eef4ff"
            font: root.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
    background: Item {
        ConsoleFrame {
            anchors.fill: parent
            radius: 9
            lineWidth: root.primary ? 2 : 1
            startColor: root.accent
            middleColor: root.primary ? "#1266ff" : root.accent
            endColor: root.primary ? "#c42aff" : root.accent
            surfaceColor: root.down ? "#102748" : root.hovered ? "#0b203b" : "#040f22"
            glow: root.primary
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            radius: 7
            visible: root.primary
            opacity: root.down ? 0.75 : root.hovered ? 1 : 0.9
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: "#008dff" }
                GradientStop { position: 0.52; color: "#303cff" }
                GradientStop { position: 1; color: "#be13f5" }
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: 12
            color: "transparent"
            border.width: 2
            border.color: "#c4f5ff"
            visible: root.activeFocus
        }
    }
}

import QtQuick

// Layered borders keep the neon treatment within Qt Quick, without effects imports.
Item {
    id: root
    property color surfaceColor: "#030e20"
    property color startColor: "#00d7ff"
    property color middleColor: "#1266ff"
    property color endColor: "#c42aff"
    property real radius: 18
    property real lineWidth: 2
    property bool glow: true

    Rectangle {
        anchors.fill: parent
        anchors.margins: -8
        radius: root.radius + 8
        color: "transparent"
        border.width: 8
        border.color: root.startColor
        opacity: 0.025
        visible: root.glow
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        radius: root.radius + 3
        color: "transparent"
        border.width: 4
        border.color: root.startColor
        opacity: 0.08
        visible: root.glow
    }
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: root.startColor }
            GradientStop { position: 0.67; color: root.middleColor }
            GradientStop { position: 1; color: root.endColor }
        }
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: root.lineWidth
        radius: Math.max(0, root.radius - root.lineWidth)
        color: root.surfaceColor
    }
}

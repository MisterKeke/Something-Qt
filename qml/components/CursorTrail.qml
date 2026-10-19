import QtQuick

Item {
    id: root
    required property var tracker
    anchors.fill: parent
    clip: true
    enabled: false
    z: 10000
    readonly property int tailCount: 8

    Repeater {
        model: root.tailCount
        delegate: Image {
            id: dot
            required property int index
            property real followX: root.tracker.position.x
            property real followY: root.tracker.position.y
            readonly property real dx: followX - root.tracker.position.x
            readonly property real dy: followY - root.tracker.position.y
            readonly property real distance: Math.sqrt(dx * dx + dy * dy)
            readonly property real limit: distance > 36 ? 36 / distance : 1
            width: 17 - index * 1.15
            height: width
            x: root.tracker.position.x + dx * limit - width / 2
            y: root.tracker.position.y + dy * limit - height / 2
            source: "qrc:/something/assets/icons/cursor-glow.svg"
            sourceSize: Qt.size(32, 32)
            opacity: root.tracker.inside ? (0.38 - index * 0.035) * Math.min(1, distance / 7) : 0
            visible: opacity > 0
            Behavior on followX {
                enabled: root.tracker.inside
                SmoothedAnimation { velocity: -1; duration: 35 + dot.index * 9; reversingMode: SmoothedAnimation.Immediate }
            }
            Behavior on followY {
                enabled: root.tracker.inside
                SmoothedAnimation { velocity: -1; duration: 35 + dot.index * 9; reversingMode: SmoothedAnimation.Immediate }
            }
        }
    }
    Image {
        width: 44
        height: 44
        x: root.tracker.position.x - width / 2
        y: root.tracker.position.y - height / 2
        source: "qrc:/something/assets/icons/cursor-ring.svg"
        sourceSize: Qt.size(96, 96)
        opacity: root.tracker.inside ? 0.82 : 0
        visible: opacity > 0
        scale: root.tracker.pressed ? 0.82 : 1
        Behavior on opacity { NumberAnimation { duration: 70 } }
        Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
    }
}

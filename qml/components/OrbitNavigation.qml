import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import Something.App as ThemeModule

Item {
    id: root

    property var items: []
    property string currentId: "dashboard"
    signal navigate(string routeId)
    signal homeRequested()
    readonly property int routeCount: items.length
    readonly property bool compact: width < 540
    readonly property real orbitRadius: Math.min((width - 150) / 2, width * 0.38, 310)
    readonly property real orbitItemSize: routeCount > 0
        ? Math.max(60, Math.min(88, Math.floor(2 * Math.PI * orbitRadius / routeCount * 0.7)))
        : 64
    readonly property real orbitButtonWidth: routeCount > 0
        ? Math.min(124, 2 * orbitRadius * Math.sin(Math.PI / routeCount)) : 124
    readonly property int columns: width < 520 ? 3 : 4
    implicitHeight: compact
                     ? Math.ceil(routeCount / columns) * 86 + 100
                     : orbitRadius * 2 + 150

    function spokeX(index) {
        const angle = -Math.PI / 2 + index * 2 * Math.PI / routeCount
        return width / 2 + Math.cos(angle) * orbitRadius - orbitButtonWidth / 2
    }

    function spokeY(index) {
        const angle = -Math.PI / 2 + index * 2 * Math.PI / routeCount
        return height / 2 + Math.sin(angle) * orbitRadius - 49
    }

    function focusFirstItem() {
        const button = root.compact ? compactNavRepeater.itemAt(0) : orbitNavRepeater.itemAt(0)
        if (button) button.forceActiveFocus()
    }

    Canvas {
        anchors.fill: parent
        visible: !root.compact

        onPaint: {
            const context = getContext("2d")
            context.reset()
            const cx = width / 2
            const cy = height / 2
            context.beginPath()
            context.arc(cx, cy, root.orbitRadius, 0, 2 * Math.PI)
            context.strokeStyle = ThemeModule.Theme.orbitRingColor
            context.lineWidth = 1
            context.stroke()

            for (let i = 0; i < root.routeCount; ++i) {
                const angle = -Math.PI / 2 + i * 2 * Math.PI / root.routeCount
                context.beginPath()
                context.moveTo(cx + Math.cos(angle) * 58, cy + Math.sin(angle) * 58)
                context.lineTo(cx + Math.cos(angle) * (root.orbitRadius - 42),
                               cy + Math.sin(angle) * (root.orbitRadius - 42))
                context.strokeStyle = ThemeModule.Theme.orbitSpokeColor
                context.stroke()
            }
        }

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onVisibleChanged: requestPaint()
    }

    Repeater {
        id: orbitNavRepeater
        model: root.items
        delegate: Button {
            id: orbitButton
            required property int index
            required property var modelData
            readonly property var route: modelData

            visible: !root.compact
            x: root.spokeX(index)
            y: root.spokeY(index)
            width: root.orbitButtonWidth
            height: 104
            padding: 0
            hoverEnabled: true
            scale: down ? 0.97 : hovered || activeFocus ? 1.035 : 1
            Behavior on scale { NumberAnimation { duration: ThemeModule.Theme.controlMotion; easing.type: Easing.OutCubic } }
            Accessible.name: route.title
            Accessible.description: qsTr("Open %1").arg(route.title)
            onClicked: root.navigate(route.id)

            contentItem: Column {
                spacing: 7
                anchors.fill: parent

                Item {
                    width: root.orbitItemSize
                    height: root.orbitItemSize
                    anchors.horizontalCenter: parent.horizontalCenter

                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: ThemeModule.Theme.raisedSurface
                        border.color: orbitButton.activeFocus ? ThemeModule.Theme.focusHighlight
                                     : orbitButton.hovered ? ThemeModule.Theme.accentCyanLight
                                     : root.currentId === orbitButton.route.id
                                       ? ThemeModule.Theme.accentCyan : ThemeModule.Theme.textDisabled
                        border.width: orbitButton.activeFocus || root.currentId === orbitButton.route.id ? 2 : 1
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -4
                            radius: width / 2
                            color: "transparent"
                            border.width: 2
                            border.color: ThemeModule.Theme.focusHighlight
                            visible: orbitButton.activeFocus
                        }
                    }

                    Image {
                        id: character
                        anchors.fill: parent
                        anchors.margins: 2
                        source: root.compact ? "" : orbitButton.route.asset
                        sourceSize: Qt.size(96, 96)
                        fillMode: Image.PreserveAspectCrop
                        layer.enabled: true
                        layer.effect: MultiEffect {
                            maskEnabled: true
                            maskSource: Image {
                                source: "qrc:/something/assets/icons/circle-mask.svg"
                                sourceSize: Qt.size(256, 256)
                            }
                        }
                        asynchronous: true
                        smooth: true
                        cache: true
                        visible: status === Image.Ready
                    }

                    Label {
                        anchors.centerIn: parent
                        text: orbitButton.route.glyph
                        visible: !character.visible
                        color: ThemeModule.Theme.focusHighlight
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: root.orbitItemSize * 0.45
                        font.weight: Font.DemiBold
                    }
                }

                Text {
                    width: parent.width
                    text: orbitButton.route.title
                    color: root.currentId === orbitButton.route.id
                           ? ThemeModule.Theme.textPrimary : ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignTop
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    style: Text.Outline
                    styleColor: Qt.alpha(ThemeModule.Theme.windowBackground, 0.86)
                }
            }

            background: Rectangle { color: "transparent" }
        }
    }

    Button {
        id: homeButton
        hoverEnabled: true
        scale: down ? 0.98 : hovered || activeFocus ? 1.025 : 1
        Behavior on scale { NumberAnimation { duration: ThemeModule.Theme.controlMotion; easing.type: Easing.OutCubic } }
        x: root.compact ? 0 : root.width / 2 - width / 2
        y: root.compact ? 6 : root.height / 2 - height / 2
        width: root.compact ? root.width : Math.min(220, root.orbitRadius * 0.82)
        height: root.compact ? 76 : width + 28
        padding: 0
        Accessible.name: qsTr("Dashboard")
        Accessible.description: qsTr("Return to the Dashboard")
        onClicked: root.homeRequested()

        contentItem: Column {
            spacing: 5

            Rectangle {
                width: root.compact ? 48 : homeButton.width - 8
                height: width
                anchors.horizontalCenter: parent.horizontalCenter
                radius: width / 2
                color: ThemeModule.Theme.raisedSurface
                border.color: homeButton.activeFocus ? ThemeModule.Theme.focusHighlight
                              : homeButton.hovered || root.currentId === "dashboard"
                              ? ThemeModule.Theme.accentCyan : ThemeModule.Theme.textDisabled
                border.width: homeButton.activeFocus ? ThemeModule.Theme.focusBorderWidth : 2

                Image {
                    anchors.fill: parent
                    anchors.margins: 2
                    source: "qrc:/something/assets/appicon.png"
                    sourceSize: Qt.size(420, 420)
                    fillMode: Image.PreserveAspectCrop
                    layer.enabled: true
                    layer.effect: MultiEffect {
                        maskEnabled: true
                        maskSource: Image {
                            source: "qrc:/something/assets/icons/circle-mask.svg"
                            sourceSize: Qt.size(256, 256)
                        }
                    }
                    smooth: true
                    cache: true
                }
            }

            Text {
                width: parent.width
                text: qsTr("Main Page")
                color: ThemeModule.Theme.textPrimary
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: root.compact ? 12 : 14
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
                style: Text.Outline
                styleColor: Qt.alpha(ThemeModule.Theme.windowBackground, 0.86)
            }
        }

        background: Rectangle { color: "transparent" }
    }

    Repeater {
        id: compactNavRepeater
        model: root.items
        delegate: Button {
            id: compactNavButton
            hoverEnabled: true
            scale: down ? 0.97 : hovered || activeFocus ? 1.025 : 1
            Behavior on scale { NumberAnimation { duration: ThemeModule.Theme.controlMotion; easing.type: Easing.OutCubic } }
            required property int index
            required property var modelData
            readonly property var route: modelData

            visible: root.compact
            x: (index % root.columns) * root.width / root.columns
            y: 84 + Math.floor(index / root.columns) * 86
            width: root.width / root.columns
            height: 82
            padding: 2
            Accessible.name: route.title
            onClicked: root.navigate(route.id)

            contentItem: Column {
                spacing: 4

                Rectangle {
                    width: 48
                    height: 48
                    anchors.horizontalCenter: parent.horizontalCenter
                    radius: width / 2
                    color: ThemeModule.Theme.raisedSurface
                    border.color: compactNavButton.activeFocus ? ThemeModule.Theme.focusHighlight
                                  : compactNavButton.hovered ? ThemeModule.Theme.accentCyanLight
                                  : root.currentId === route.id
                                  ? ThemeModule.Theme.accentCyan : ThemeModule.Theme.borderColor
                    border.width: compactNavButton.activeFocus || root.currentId === route.id ? 2 : 1

                    Image {
                        id: compactCharacter
                        anchors.fill: parent
                        anchors.margins: 1
                        source: root.compact ? route.asset : ""
                        sourceSize: Qt.size(64, 64)
                        fillMode: Image.PreserveAspectCrop
                        layer.enabled: true
                        layer.effect: MultiEffect {
                            maskEnabled: true
                            maskSource: Image {
                                source: "qrc:/something/assets/icons/circle-mask.svg"
                                sourceSize: Qt.size(256, 256)
                            }
                        }
                        asynchronous: true
                        cache: true
                        visible: status === Image.Ready
                    }
                    Label {
                        anchors.centerIn: parent
                        text: route.glyph
                        visible: !compactCharacter.visible
                        color: ThemeModule.Theme.focusHighlight
                        font.pixelSize: 20
                        font.weight: Font.Bold
                    }
                }
                Text {
                    width: parent.width
                    text: route.title
                    color: ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: ThemeModule.Theme.metadataFontSize
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                }
            }
            background: Rectangle { color: "transparent" }
        }
    }
}

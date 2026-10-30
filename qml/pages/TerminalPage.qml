import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Item {
    id: root

    required property var controller
    property var lifecycleController: null
    property string returnRoute: "dashboard"
    property string returnTitle: qsTr("Main Page")
    property bool showReturnButton: true
    signal navigateRequested(string routeId)

    implicitHeight: 720

    function focusInput() {
        commandField.forceActiveFocus()
        commandField.cursorPosition = commandField.text.length
    }

    function captureViewState() {
        return { transcriptY: transcriptView.contentY, followNewest: followNewest }
    }

    function restoreViewState(state) {
        const restoredFollow = state && state.followNewest !== undefined
                               ? Boolean(state.followNewest) : true
        const hasSavedPosition = state && state.transcriptY !== undefined
        followNewest = restoredFollow
        adjustingTranscriptScroll = true
        Qt.callLater(function() {
            if (restoredFollow)
                transcriptView.positionViewAtEnd()
            else if (hasSavedPosition)
                transcriptView.contentY = Number(state.transcriptY)
            else
                transcriptView.contentY = 0
            Qt.callLater(function() {
                root.followNewest = restoredFollow || transcriptView.atYEnd
                root.adjustingTranscriptScroll = false
            })
        })
    }

    property bool followNewest: true
    property bool scrollAfterAppend: true
    property bool adjustingTranscriptScroll: false
    property var suggestions: []
    property int suggestionIndex: 0
    property bool applyingCompletion: false

    function scrollToNewest() {
        adjustingTranscriptScroll = true
        transcriptView.positionViewAtEnd()
        adjustingTranscriptScroll = false
        followNewest = true
    }

    function chooseSuggestion(index) {
        if (index < 0 || index >= suggestions.length) return
        const applied = controller.applyCompletion(commandField.text, suggestions[index])
        applyingCompletion = true
        commandField.text = applied.text
        commandField.cursorPosition = applied.cursorPosition
        suggestions = []
        applyingCompletion = false
        commandField.forceActiveFocus()
    }

    function requestCompletion() {
        const result = controller.completions(commandField.text, commandField.cursorPosition)
        const values = result.suggestions || []
        if (values.length === 0) {
            suggestions = []
            return false
        }
        if (suggestions.length > 0) {
            suggestionIndex = (suggestionIndex + 1) % suggestions.length
            return true
        }
        if (values.length === 1) {
            chooseSuggestionFrom(values[0])
            return true
        }
        suggestions = values
        suggestionIndex = 0
        return true
    }

    function chooseSuggestionFrom(suggestion) {
        const applied = controller.applyCompletion(commandField.text, suggestion)
        applyingCompletion = true
        commandField.text = applied.text
        commandField.cursorPosition = applied.cursorPosition
        suggestions = []
        applyingCompletion = false
        commandField.forceActiveFocus()
    }

    Connections {
        target: root.controller.output
        function onAboutToAppend() {
            root.scrollAfterAppend = root.followNewest
        }
        function onCountChanged() {
            if (root.scrollAfterAppend) {
                Qt.callLater(function() {
                    if (root.followNewest)
                        root.scrollToNewest()
                })
            }
        }
    }

    readonly property string monoFont: ThemeModule.Theme.fontFamily
    readonly property bool compact: width < 1120
    property string readyTime: Qt.formatTime(new Date(), "HH:mm:ss")

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: "#020d1e" }
            GradientStop { position: 0.55; color: "#030c1c" }
            GradientStop { position: 1; color: "#050e25" }
        }
    }

    // Static circuit details are repainted only when the viewport changes.
    Canvas {
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const wash = ctx.createRadialGradient(width * 0.45, height, 0,
                                                  width * 0.45, height, width * 0.65)
            wash.addColorStop(0, "rgba(12, 56, 141, 0.17)")
            wash.addColorStop(1, "rgba(0, 12, 31, 0)")
            ctx.fillStyle = wash
            ctx.fillRect(0, 0, width, height)
            ctx.lineWidth = 1
            ctx.strokeStyle = "#0c3158"
            function line(points) {
                ctx.beginPath()
                ctx.moveTo(points[0], points[1])
                for (let i = 2; i < points.length; i += 2)
                    ctx.lineTo(points[i], points[i + 1])
                ctx.stroke()
            }
            line([0, 85, 45, 130, width - 475, 130, width - 370, 18, width, 18])
            line([width - 470, 130, width - 420, 108, width - 400, 108, width - 340, 38])
            line([0, height * 0.43, 17, height * 0.41, 17, height * 0.34, 30, height * 0.32])
            line([width, height * 0.59, width - 30, height * 0.63, width - 30, height * 0.73])
            ctx.strokeStyle = "#154aaa"
            for (let i = 0; i < 5; ++i) {
                line([9, height * 0.55 + i * 10, 20, height * 0.55 + i * 10 + 14])
                line([width - 25, height * 0.66 + i * 10, width - 2, height * 0.66 + i * 10 - 25])
            }
            ctx.fillStyle = "#6326df"
            ctx.fillRect(10, height * 0.45, 5, 58)
            ctx.fillStyle = "#00baff"
            ctx.fillRect(width - 18, height * 0.32, 5, 42)
        }
    }

    ColumnLayout {
        id: pageContent
        anchors.fill: parent
        anchors.margins: root.width < 700 ? 16 : 24
        anchors.topMargin: 22
        spacing: 20

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                spacing: 24
                Item {
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: 72
                    Shared.ConsoleFrame {
                        anchors.fill: parent
                        radius: 14
                        lineWidth: 3
                        endColor: "#008aff"
                        surfaceColor: "#031126"
                    }
                    Shared.ConsoleIcon {
                        anchors.centerIn: parent
                        width: 44
                        height: 44
                        symbol: "terminal"
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3
                    Label {
                        text: qsTr("Console")
                        color: "#f1f5ff"
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 36
                        font.weight: Font.Bold
                        font.letterSpacing: 1.5
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Type run full to open the full application window.")
                        color: "#8cc5ff"
                        font.family: root.monoFont
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }
                }
                Item {
                    visible: !root.compact
                    Layout.preferredWidth: 300
                    Layout.preferredHeight: 72
                    Row {
                        anchors.centerIn: parent
                        spacing: 18
                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: "#00eee6"
                            anchors.verticalCenter: parent.verticalCenter
                            Rectangle {
                                anchors.centerIn: parent
                                width: 18
                                height: 18
                                radius: 9
                                color: "#1800eee6"
                            }
                        }
                        Column {
                            spacing: 6
                            Text {
                                text: "S O M E T H I N G"
                                color: "#a5cfff"
                                font.family: root.monoFont
                                font.pixelSize: 13
                                font.letterSpacing: 3
                            }
                            Text {
                                text: qsTr("APPLICATION CONSOLE")
                                color: "#6493c6"
                                font.family: root.monoFont
                                font.pixelSize: 10
                                font.letterSpacing: 4
                            }
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: root.compact ? 0 : 96
                spacing: 9
                Shared.ConsoleIcon {
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    symbol: "info"
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Only registered application commands are supported. This console does not run operating-system commands or scripts.")
                    color: "#a8d0fb"
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    Accessible.name: text
                }
                Shared.StatusBadge {
                    visible: root.controller.pendingCount > 0
                    text: qsTr("%1 pending").arg(root.controller.pendingCount)
                    badgeColor: "#00cfff"
                    Accessible.name: qsTr("%1 commands pending").arg(root.controller.pendingCount)
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.lifecycleController
                     && root.lifecycleController.startupErrorMessage.length > 0
            text: root.lifecycleController ? root.lifecycleController.startupErrorMessage : ""
            textFormat: Text.PlainText
            color: ThemeModule.Theme.error
            font.family: root.monoFont
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 140
            Shared.ConsoleFrame {
                anchors.fill: parent
                radius: 19
                surfaceColor: "#05152e"
            }
            Rectangle {
                id: transcriptSurface
                anchors.fill: parent
                anchors.margins: 16
                radius: 11
                color: "#020d1b"
                border.color: "#0a2544"
                clip: true

                Canvas {
                    anchors.fill: parent
                    anchors.margins: 3
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        for (let x = 8; x < width; x += 11) {
                            const alpha = 0.04 + 0.10 * (1 - x / Math.max(1, width))
                            ctx.fillStyle = "rgba(0, 157, 234, " + alpha + ")"
                            for (let y = 9; y < height; y += 11)
                                ctx.fillRect(x, y, 1, 1)
                        }
                    }
                }

                RowLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 18
                    spacing: 20
                    visible: root.controller.output.count === 0
                    Shared.ConsoleIcon {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 22
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            text: !root.lifecycleController ? qsTr("Your application console is ready")
                                  : root.lifecycleController.startupErrorMessage.length > 0
                                    ? qsTr("Application startup failed")
                                  : root.lifecycleController.lifecycleState === "running"
                                    ? qsTr("Your application console is ready")
                                    : qsTr("Starting the application…")
                            color: "#8bd7ff"
                            font.family: root.monoFont
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Enter help to see the registered commands. Only commands provided by Something are accepted; no shell, operating-system command, script, or executable is run.")
                            color: "#8dbff3"
                            font.family: root.monoFont
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                    }
                    Label {
                        Layout.alignment: Qt.AlignTop
                        visible: root.width > 900
                        text: root.readyTime
                        color: "#5984b5"
                        font.family: root.monoFont
                        font.pixelSize: 12
                        Accessible.ignored: true
                    }
                }

                ListView {
                    id: transcriptView
                    anchors.fill: parent
                    anchors.margins: 18
                    visible: root.controller.output.count > 0
                    clip: true
                    spacing: 12
                    model: root.controller.output
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }
                    onMovementEnded: root.followNewest = atYEnd
                    onContentYChanged: {
                        if (!root.adjustingTranscriptScroll)
                            root.followNewest = atYEnd
                    }
                    onHeightChanged: {
                        if (root.followNewest)
                            Qt.callLater(function() {
                                if (root.followNewest) root.scrollToNewest()
                            })
                    }

                    delegate: Item {
                        id: outputRow
                        required property string text
                        required property string kind
                        required property string timestamp
                        width: ListView.view.width - 12
                        implicitHeight: Math.max(24, outputText.implicitHeight + 10)

                        Rectangle {
                            anchors.fill: parent
                            radius: 5
                            color: outputRow.kind === "error" ? "#22ff4c7b"
                                 : outputRow.kind === "warning" ? "#18ffbf4d"
                                 : outputRow.kind === "normal" ? "#0d008aff" : "transparent"
                        }
                        Shared.ConsoleIcon {
                            x: 0
                            y: 5
                            width: 18
                            height: 18
                            visible: outputRow.kind === "normal" || outputRow.kind === "pending"
                            ink: outputRow.kind === "pending" ? "#8dbff3" : "#00d7ff"
                        }
                        TextEdit {
                            id: outputText
                            anchors.left: parent.left
                            anchors.right: outputTime.left
                            anchors.top: parent.top
                            anchors.leftMargin: 38
                            anchors.rightMargin: 14
                            anchors.topMargin: 5
                            // The command echo already has a separate prompt icon.
                            text: outputRow.kind === "normal" && outputRow.text.startsWith("> ")
                                  ? outputRow.text.substring(2) : outputRow.text
                            textFormat: TextEdit.PlainText
                            readOnly: true
                            color: outputRow.kind === "success" ? "#83ddbf"
                                 : outputRow.kind === "warning" ? "#ffcf7c"
                                 : outputRow.kind === "error" ? "#ff8ca7"
                                 : outputRow.kind === "pending" ? "#00cfff"
                                 : outputRow.kind === "normal" ? "#d9edff" : "#8dbff3"
                            font.family: root.monoFont
                            font.pixelSize: 13
                            wrapMode: TextEdit.Wrap
                            selectByMouse: true
                            selectionColor: "#215a9e"
                            selectedTextColor: "#ffffff"
                            Accessible.name: outputRow.text
                        }
                        Label {
                            id: outputTime
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 6
                            width: root.width > 900 ? implicitWidth : 0
                            visible: root.width > 900
                            text: outputRow.timestamp
                            color: "#5984b5"
                            font.family: root.monoFont
                            font.pixelSize: 11
                            Accessible.ignored: true
                        }
                    }
                }
            }
        }

        RowLayout {
            id: commandRow
            Layout.fillWidth: true
            spacing: 16
            TextField {
                id: commandField
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                leftPadding: 56
                rightPadding: 16
                color: "#d9edff"
                placeholderTextColor: "#658dbf"
                selectionColor: "#215a9e"
                selectedTextColor: "#ffffff"
                font.family: root.monoFont
                font.pixelSize: 15
                Accessible.role: Accessible.EditableText
                Accessible.name: qsTr("Application command")
                placeholderText: qsTr("Type a command…")
                maximumLength: 4096
                selectByMouse: true
                background: Item {
                    Shared.ConsoleFrame {
                        anchors.fill: parent
                        radius: 9
                        endColor: "#087cff"
                        surfaceColor: commandField.activeFocus ? "#041228" : "#020c1c"
                    }
                    Shared.ConsoleIcon {
                        anchors.left: parent.left
                        anchors.leftMargin: 13
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18
                        height: 22
                        ink: "#00f0f4"
                    }
                    Rectangle {
                        x: 40
                        y: 11
                        width: 1
                        height: parent.height - 22
                        color: commandField.activeFocus ? "#1678b0" : "#143958"
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        radius: 11
                        color: "transparent"
                        border.color: "#5500cfff"
                        visible: commandField.activeFocus
                    }
                }
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Tab) {
                        event.accepted = root.requestCompletion()
                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                               && root.suggestions.length > 0) {
                        root.chooseSuggestion(root.suggestionIndex)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Escape && root.suggestions.length > 0) {
                        root.suggestions = []
                        event.accepted = true
                    } else if (event.key === Qt.Key_Up && root.suggestions.length > 0) {
                        root.suggestionIndex = (root.suggestionIndex + root.suggestions.length - 1)
                                               % root.suggestions.length
                        event.accepted = true
                    } else if (event.key === Qt.Key_Down && root.suggestions.length > 0) {
                        root.suggestionIndex = (root.suggestionIndex + 1) % root.suggestions.length
                        event.accepted = true
                    } else if (event.key === Qt.Key_Up) {
                        commandField.text = root.controller.historyUp(commandField.text)
                        commandField.cursorPosition = commandField.text.length
                        event.accepted = true
                    } else if (event.key === Qt.Key_Down) {
                        commandField.text = root.controller.historyDown(commandField.text)
                        commandField.cursorPosition = commandField.text.length
                        event.accepted = true
                    }
                }
                onTextChanged: if (!root.applyingCompletion) root.suggestions = []
                onAccepted: {
                    if (root.suggestions.length > 0) {
                        root.chooseSuggestion(root.suggestionIndex)
                        return
                    }
                    const submitted = text
                    clear()
                    root.controller.submit(submitted)
                }
            }
            Shared.ConsoleButton {
                text: qsTr("Run")
                symbol: "play"
                primary: true
                Accessible.name: qsTr("Run application command")
                onClicked: {
                    const submitted = commandField.text
                    commandField.clear()
                    root.controller.submit(submitted)
                    root.focusInput()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 16
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                visible: !root.compact
                RowLayout {
                    anchors.fill: parent
                    spacing: 8
                    KeyCap { symbol: "up" }
                    Label { text: "/"; color: "#6795c4" }
                    KeyCap { symbol: "down" }
                    Label {
                        text: qsTr("recalls command history;")
                        color: "#8ab7e8"
                        font.family: root.monoFont
                        font.pixelSize: 11
                    }
                    KeyCap { symbol: "down" }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("restores your unfinished input.")
                        color: "#8ab7e8"
                        font.family: root.monoFont
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                }
            }
            Shared.ConsoleButton {
                visible: root.showReturnButton
                Layout.maximumWidth: root.compact ? 240 : 210
                text: qsTr("Return to %1").arg(root.returnTitle)
                symbol: "back"
                Accessible.name: text
                onClicked: root.navigateRequested(root.returnRoute || "dashboard")
            }
            Item { Layout.fillWidth: true; visible: root.compact }
            Shared.ConsoleButton {
                text: qsTr("Help")
                symbol: "book"
                Accessible.name: qsTr("Show command help")
                onClicked: {
                    root.controller.submit("help")
                    commandField.clear()
                    root.focusInput()
                }
            }
            Shared.ConsoleButton {
                text: qsTr("Clear output")
                symbol: "trash"
                accent: "#df32fb"
                Accessible.name: qsTr("Clear console output")
                onClicked: {
                    root.controller.clearOutput()
                    root.followNewest = true
                    root.scrollAfterAppend = true
                    root.focusInput()
                }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: root.compact
            text: qsTr("↑ / ↓ recalls command history; ↓ restores your unfinished input. Tab completes commands.")
            color: "#8ab7e8"
            font.family: root.monoFont
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }

    // Completion overlays the transcript rather than reducing its viewport.
    Rectangle {
        id: completionPanel
        anchors.left: pageContent.left
        anchors.right: pageContent.right
        y: pageContent.y + commandRow.y - height - 10
        height: Math.min(root.suggestions.length, 5) * 40 + 12
        visible: root.suggestions.length > 0
        color: "#07172c"
        radius: 9
        border.color: "#168cce"
        z: 10
        ListView {
            id: suggestionList
            anchors.fill: parent
            anchors.margins: 6
            model: root.suggestions
            currentIndex: root.suggestionIndex
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }
            Accessible.name: qsTr("Command completion suggestions")
            delegate: ItemDelegate {
                id: suggestionDelegate
                required property var modelData
                required property int index
                width: suggestionList.width
                height: 40
                highlighted: index === root.suggestionIndex
                text: modelData.text
                Accessible.name: qsTr("Suggestion: %1").arg(modelData.text)
                contentItem: Text {
                    text: suggestionDelegate.text
                    textFormat: Text.PlainText
                    color: suggestionDelegate.highlighted ? "#e4f6ff" : "#8dbff3"
                    font.family: root.monoFont
                    font.pixelSize: 13
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
                background: Rectangle {
                    color: suggestionDelegate.highlighted ? "#12365a" : "transparent"
                    radius: 5
                }
                onHoveredChanged: if (hovered) root.suggestionIndex = index
                onClicked: root.chooseSuggestion(index)
            }
        }
    }

    component KeyCap: Rectangle {
        property alias symbol: keyIcon.symbol
        implicitWidth: 27
        implicitHeight: 28
        radius: 6
        color: "#07172e"
        border.color: "#102d51"
        Shared.ConsoleIcon {
            id: keyIcon
            anchors.centerIn: parent
            width: 16
            height: 16
            ink: "#a6d3ff"
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Item {
    id: root

    required property var controller
    property string returnRoute: "dashboard"
    property string returnTitle: qsTr("Main Page")
    signal navigateRequested(string routeId)

    implicitHeight: pageContent.implicitHeight

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

    ColumnLayout {
        id: pageContent
        width: root.width
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Only registered application commands are supported. This console does not run operating-system commands or scripts.")
                color: ThemeModule.Theme.textDim
                font.family: ThemeModule.Theme.bodyFont
                font.pixelSize: 13
                wrapMode: Text.Wrap
                Accessible.name: text
            }
            Shared.StatusBadge {
                visible: root.controller.pendingCount > 0
                text: qsTr("%1 pending").arg(root.controller.pendingCount)
                badgeColor: ThemeModule.Theme.cyan
                Accessible.name: qsTr("%1 commands pending").arg(root.controller.pendingCount)
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 420
            radius: ThemeModule.Theme.cornerRadius
            color: "#e611131d"
            border.color: ThemeModule.Theme.border
            border.width: 1
            clip: true

            Item {
                anchors.fill: parent
                anchors.margins: 12

                ColumnLayout {
                    anchors.fill: parent
                    visible: root.controller.output.count === 0
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Your application console is ready")
                        color: ThemeModule.Theme.text
                        font.family: ThemeModule.Theme.displayFont
                        font.pixelSize: 19
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Enter help to see the registered commands. Only commands provided by Something are accepted; no shell, operating-system command, script, or executable is run.")
                        color: ThemeModule.Theme.textDim
                        font.family: ThemeModule.Theme.bodyFont
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignHCenter
                    }
                    Item { Layout.fillHeight: true }
                }

                ListView {
                    id: transcriptView
                    anchors.fill: parent
                    visible: root.controller.output.count > 0
                    clip: true
                    spacing: 5
                    model: root.controller.output
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    onMovementEnded: root.followNewest = atYEnd
                    onContentYChanged: {
                        if (!root.adjustingTranscriptScroll)
                            root.followNewest = atYEnd
                    }

                    delegate: Rectangle {
                        id: outputRow
                        required property string text
                        required property string kind
                        width: ListView.view.width - 10
                        implicitHeight: outputText.implicitHeight + 16
                        radius: 7
                        color: kind === "normal" ? "#171a2a"
                             : kind === "pending" ? "#14252d"
                             : kind === "error" ? "#2b1821"
                             : kind === "warning" ? "#2b2416" : "#10131d"
                        border.width: 1
                        border.color: kind === "normal" ? ThemeModule.Theme.border
                                    : kind === "pending" ? "#345e69"
                                    : kind === "error" ? "#704052"
                                    : kind === "warning" ? "#705a31" : "#202638"

                        TextEdit {
                            id: outputText
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 8
                            text: outputRow.text
                            textFormat: TextEdit.PlainText
                            readOnly: true
                            color: outputRow.kind === "success" ? ThemeModule.Theme.success
                                 : outputRow.kind === "warning" ? ThemeModule.Theme.warning
                                 : outputRow.kind === "error" ? ThemeModule.Theme.error
                                 : outputRow.kind === "pending" ? ThemeModule.Theme.cyan
                                 : outputRow.kind === "normal" ? ThemeModule.Theme.text
                                 : ThemeModule.Theme.textDim
                            font.family: ThemeModule.Theme.bodyFont
                            font.pixelSize: 13
                            wrapMode: TextEdit.Wrap
                            selectByMouse: true
                            Accessible.name: text
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Shared.AppTextField {
                id: commandField
                Layout.fillWidth: true
                accessibleLabel: qsTr("Application command")
                placeholderText: qsTr("Type a command…")
                maximumLength: 4096
                selectByMouse: true
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
            Shared.AppButton {
                text: qsTr("Run")
                Accessible.name: qsTr("Run application command")
                onClicked: {
                    const submitted = commandField.text
                    commandField.clear()
                    root.controller.submit(submitted)
                    root.focusInput()
                }
            }
        }

        ListView {
            id: suggestionList
            Layout.fillWidth: true
            Layout.preferredHeight: root.suggestions.length > 0
                                   ? Math.min(root.suggestions.length, 5) * 38 : 0
            visible: root.suggestions.length > 0
            model: root.suggestions
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            Accessible.name: qsTr("Command completion suggestions")
            delegate: ItemDelegate {
                required property var modelData
                required property int index
                width: suggestionList.width
                height: 38
                highlighted: index === root.suggestionIndex
                text: modelData.text
                Accessible.name: qsTr("Suggestion: %1").arg(modelData.text)
                onHoveredChanged: if (hovered) root.suggestionIndex = index
                onClicked: root.chooseSuggestion(index)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Up/Down recalls command history; Down restores your unfinished input.")
                color: ThemeModule.Theme.textDim
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
            Shared.AppButton {
                text: qsTr("Return to %1").arg(root.returnTitle)
                quiet: true
                Accessible.name: text
                onClicked: root.navigateRequested(root.returnRoute || "dashboard")
            }
            Shared.AppButton {
                text: qsTr("Help")
                quiet: true
                Accessible.name: qsTr("Show command help")
                onClicked: {
                    commandField.text = "help"
                    root.controller.submit(commandField.text)
                    commandField.clear()
                    root.focusInput()
                }
            }
            Shared.AppButton {
                text: qsTr("Clear output")
                quiet: true
                Accessible.name: qsTr("Clear console output")
                    onClicked: {
                        root.controller.clearOutput()
                        root.followNewest = true
                        root.scrollAfterAppend = true
                    }
            }
        }
    }
}

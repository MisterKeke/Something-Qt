import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Dialog {
    id: root

    property var controller
    property int selectedIndex: 0
    readonly property var filters: [
        { label: qsTr("All"), value: "all" },
        { label: qsTr("Pages"), value: "navigation" },
        { label: qsTr("Tasks"), value: "tasks" },
        { label: qsTr("Apps"), value: "applications" },
        { label: qsTr("Running"), value: "running" },
        { label: qsTr("Games"), value: "games" },
        { label: qsTr("Favorites"), value: "favorites" },
        { label: qsTr("Setups"), value: "setups" }
    ]

    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(820, parent ? parent.width - 40 : 780)
    height: Math.min(700, parent ? parent.height - 40 : 660)
    padding: 18
    title: qsTr("Universal Search")
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: {
        runningApplicationsController.setSearchVisible(true)
        searchField.text = ""
        selectedIndex = 0
        controller.setCategoryFilter("all")
        controller.setQuery("")
        Qt.callLater(function() { searchField.forceActiveFocus() })
    }
    onClosed: {
        runningApplicationsController.setSearchVisible(false)
        controller.reset()
    }

    background: Rectangle {
        color: ThemeModule.Theme.raisedSurface
        border.color: ThemeModule.Theme.accentCyan
        border.width: 1
        radius: ThemeModule.Theme.panelRadius
    }

    contentItem: ColumnLayout {
        spacing: 12

        Shared.AppTextField {
            id: searchField
            Layout.fillWidth: true
            placeholderText: qsTr("Search pages, tasks, apps, games, channels, and setups")
            accessibleLabel: qsTr("Search Something-Qt")
            maximumLength: 200
            onTextEdited: {
                root.selectedIndex = 0
                root.controller.setQuery(text)
            }
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Escape) {
                    root.close()
                    event.accepted = true
                } else if (event.key === Qt.Key_Down) {
                    if (root.controller.results.count > 0)
                        root.selectedIndex = Math.min(root.selectedIndex + 1,
                                                      root.controller.results.count - 1)
                    event.accepted = true
                } else if (event.key === Qt.Key_Up) {
                    if (root.controller.results.count > 0)
                        root.selectedIndex = Math.max(0, root.selectedIndex - 1)
                    event.accepted = true
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    if (searchField.inputMethodComposing) {
                        event.accepted = false
                        return
                    }
                    if (root.controller.results.count > 0)
                        root.controller.activateResult(root.selectedIndex, "")
                    event.accepted = true
                }
            }
        }

        Flickable {
            Layout.fillWidth: true
            contentWidth: filterRow.implicitWidth
            contentHeight: filterRow.implicitHeight
            clip: true
            implicitHeight: filterRow.implicitHeight
            RowLayout {
                id: filterRow
                spacing: 6
                Repeater {
                    model: root.filters
                    delegate: Shared.FilterChip {
                        required property var modelData
                        text: modelData.label
                        selected: root.controller.categoryFilter === modelData.value
                        onClicked: {
                            root.selectedIndex = 0
                            root.controller.setCategoryFilter(modelData.value)
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.controller.message.length > 0
            text: root.controller.message
            textFormat: Text.PlainText
            color: ThemeModule.Theme.warning
            font.family: ThemeModule.Theme.fontFamily
            font.pixelSize: ThemeModule.Theme.metadataFontSize
            wrapMode: Text.Wrap
        }

        ListView {
            id: resultList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 5
            model: root.controller.results
            currentIndex: root.selectedIndex
            ScrollBar.vertical: ThemeModule.AppScrollBar { policy: ScrollBar.AsNeeded }
            onCurrentIndexChanged: root.selectedIndex = currentIndex

            delegate: Rectangle {
                id: resultRow
                required property int index
                required property string title
                required property string subtitle
                required property string source
                required property string icon
                required property var actions
                property int resultIndex: index
                width: ListView.view.width
                height: resultContent.implicitHeight + 10
                color: root.selectedIndex === resultIndex
                       ? ThemeModule.Theme.hoverSurface : ThemeModule.Theme.panelSurface
                border.color: root.selectedIndex === resultIndex
                              ? ThemeModule.Theme.focusHighlight : ThemeModule.Theme.borderColor
                border.width: root.selectedIndex === resultIndex
                              ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
                radius: ThemeModule.Theme.panelRadius

                MouseArea {
                    anchors.fill: parent
                    z: 0
                    onClicked: root.selectedIndex = resultRow.resultIndex
                    onDoubleClicked: {
                        root.selectedIndex = resultRow.resultIndex
                        root.controller.activateResult(resultRow.resultIndex, "")
                    }
                }

                RowLayout {
                    id: resultContent
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 10
                    z: 1

                    Item {
                        Layout.preferredWidth: 34
                        Layout.preferredHeight: 34
                        Image {
                            anchors.fill: parent
                            source: resultRow.icon
                            visible: resultRow.icon.startsWith("data:image/")
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        Label {
                            anchors.centerIn: parent
                            visible: !resultRow.icon.startsWith("data:image/")
                            text: resultRow.icon
                            color: ThemeModule.Theme.accentCyan
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 22
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        Label {
                            Layout.fillWidth: true
                            text: resultRow.title
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: ThemeModule.Theme.labelFontSize
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: resultRow.subtitle.length > 0
                                  ? resultRow.subtitle + "  ·  " + resultRow.source
                                  : resultRow.source
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: ThemeModule.Theme.metadataFontSize
                        }
                    }

                    Repeater {
                        model: resultRow.actions
                        delegate: Shared.AppButton {
                            required property var modelData
                            text: modelData.label
                            quiet: true
                            onClicked: root.controller.activateResult(resultRow.resultIndex,
                                                                      modelData.id)
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.controller.results.count === 0 && root.controller.message.length === 0
            horizontalAlignment: Text.AlignHCenter
            text: root.controller.searching ? qsTr("Searching…")
                  : root.controller.query.trim().length === 0
                    ? qsTr("Type to search your pages and saved content.")
                    : qsTr("No results found.")
            textFormat: Text.PlainText
            color: ThemeModule.Theme.textSecondary
            font.family: ThemeModule.Theme.fontFamily
            font.pixelSize: ThemeModule.Theme.metadataFontSize
            padding: 8
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("↑  ↓  Move   ·   Enter  Open   ·   Esc  Close")
            color: ThemeModule.Theme.textDisabled
            font.family: ThemeModule.Theme.fontFamily
            font.pixelSize: ThemeModule.Theme.metadataFontSize
            horizontalAlignment: Text.AlignRight
        }
    }

    Connections {
        target: root.controller.results
        function onCountChanged() {
            if (root.controller.results.count <= 0)
                root.selectedIndex = 0
            else
                root.selectedIndex = Math.max(0, Math.min(root.selectedIndex,
                                                          root.controller.results.count - 1))
        }
    }
}

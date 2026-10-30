import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Item {
    id: root

    property var controller
    property var releaseController
    property var savedViewState: ({})
    property string currentSection: "prices"

    implicitHeight: content.implicitHeight + 8
    Accessible.name: qsTr("Steam tracking")

    function captureViewState() {
        return {
            section: currentSection,
            prices: pricePanel.captureViewState(),
            releases: releasePanel.captureViewState()
        }
    }

    function restoreViewState(state) {
        savedViewState = state || {}
        currentSection = releaseController
                         && Number(releaseController.pendingReleaseDetailsAppId) > 0
                         ? "releases"
                         : savedViewState.section === "releases" ? "releases" : "prices"
        Qt.callLater(function() {
            pricePanel.restoreViewState(root.savedViewState.prices || {})
            releasePanel.restoreViewState(root.savedViewState.releases || {})
            root.focusPendingReleaseDetails()
        })
    }

    function focusPendingReleaseDetails() {
        if (!releaseController
                || Number(releaseController.pendingReleaseDetailsAppId) <= 0) return
        currentSection = "releases"
        Qt.callLater(function() { releasePanel.revealPendingCalendarRelease() })
    }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Shared.FilterChip {
                text: qsTr("Price Tracker")
                selected: root.currentSection === "prices"
                onClicked: root.currentSection = "prices"
            }
            Shared.FilterChip {
                text: qsTr("Release Radar")
                selected: root.currentSection === "releases"
                tint: ThemeModule.Theme.accentCyan
                onClicked: root.currentSection = "releases"
            }
            Item { Layout.fillWidth: true }
        }

        StackLayout {
            id: sectionStack
            Layout.fillWidth: true
            Layout.preferredHeight: root.currentSection === "releases"
                                    ? releasePanel.implicitHeight : pricePanel.implicitHeight
            currentIndex: root.currentSection === "releases" ? 1 : 0

            Item {
                Layout.fillWidth: true
                implicitHeight: pricePanel.implicitHeight
                ThemeModule.SteamPriceTrackingPanel {
                    id: pricePanel
                    width: parent.width
                    controller: root.controller
                }
            }

            Item {
                Layout.fillWidth: true
                implicitHeight: releasePanel.implicitHeight
                ThemeModule.SteamReleaseRadarPage {
                    id: releasePanel
                    width: parent.width
                    controller: root.releaseController
                    active: root.currentSection === "releases"
                }
            }
        }
    }

    Connections {
        target: root.releaseController
        function onStateChanged() { root.focusPendingReleaseDetails() }
    }

}

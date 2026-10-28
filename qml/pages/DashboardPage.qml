import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Item {
    id: root
    property var controller: dashboardController
    property var runningController: runningApplicationsController
    property var navigationItems: []
    readonly property bool sideBySide: width >= 1080
    readonly property int railWidth: width >= 1600 ? 280 : width >= 1300 ? 250 : 224
    signal navigateRequested(string routeId)
    implicitHeight: dashboardGrid.implicitHeight
    function restoreShellFocus() {
        Qt.callLater(function() {
            if (root.visible) navigation.focusFirstItem()
        })
    }
    Component.onCompleted: {
        controller.setPageVisible(visible)
        runningController.setDashboardVisible(visible)
    }
    Component.onDestruction: {
        controller.setPageVisible(false)
        runningController.setDashboardVisible(false)
    }
    onVisibleChanged: {
        if (controller) controller.setPageVisible(visible)
        if (runningController) runningController.setDashboardVisible(visible)
    }
    GridLayout {
        id: dashboardGrid
        width: root.width
        columns: root.sideBySide ? 3 : 1
        columnSpacing: 18
        rowSpacing: 18
        Shared.DashboardLeftRail {
            Layout.row: root.sideBySide ? 0 : 1
            Layout.column: 0
            Layout.preferredWidth: root.sideBySide ? root.railWidth : -1
            Layout.minimumWidth: root.sideBySide ? root.railWidth : 0
            Layout.maximumWidth: root.sideBySide ? root.railWidth : Infinity
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            runningController: root.runningController
            onNavigateRequested: routeId => root.navigateRequested(routeId)
        }
        ColumnLayout {
            Layout.row: 0
            Layout.column: root.sideBySide ? 1 : 0
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.alignment: Qt.AlignTop
            spacing: 18
            Shared.OrbitNavigation {
                id: navigation
                Layout.fillWidth: true
                items: root.navigationItems
                currentId: "dashboard"
                onNavigate: routeId => root.navigateRequested(routeId)
                onHomeRequested: root.navigateRequested("dashboard")
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3
                Label {
                    text: qsTr("Main Page")
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: root.sideBySide ? 42 : 32
                    font.weight: Font.Bold
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Your navigation hub, favorite updates, and calendar at a glance")
                    color: ThemeModule.Theme.focusHighlight
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }
            }
            CalendarPage {
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                embedded: true
            }
            Shared.DashboardNewsCard {
                Layout.fillWidth: true
                controller: root.controller
            }
        }
        Shared.DashboardRightRail {
            Layout.row: root.sideBySide ? 0 : 2
            Layout.column: root.sideBySide ? 2 : 0
            Layout.preferredWidth: root.sideBySide ? root.railWidth : -1
            Layout.minimumWidth: root.sideBySide ? root.railWidth : 0
            Layout.maximumWidth: root.sideBySide ? root.railWidth : Infinity
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            controller: root.controller
            onNavigateRequested: routeId => root.navigateRequested(routeId)
        }
    }
}

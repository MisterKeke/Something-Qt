import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Something.App as ThemeModule

Button {
    id: root
    property var controller: weatherController
    readonly property bool hasForecast: controller.primaryForecast.count > 0
    signal weatherRequested()
    implicitHeight: Math.max(150, weatherContent.implicitHeight + 32)
    padding: 16
    Accessible.name: qsTr("Weather. Open forecast and city search.")
    onClicked: weatherRequested()
    background: Rectangle {
        radius: 14
        color: root.hovered ? "#ed1b1f2f" : "#e611131d"
        border.color: root.activeFocus ? ThemeModule.Theme.cyan : ThemeModule.Theme.border
        Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        Behavior on border.color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
    }
    contentItem: ColumnLayout {
        id: weatherContent
        spacing: 7
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Weather")
                color: ThemeModule.Theme.text
                font.family: ThemeModule.Theme.displayFont
                font.pixelSize: 18
                font.bold: true
            }
            Label {
                text: root.hasForecast ? String(root.controller.primaryToday.date || "") : "›"
                color: ThemeModule.Theme.textDim
                font.pixelSize: 11
            }
        }
        Label {
            Layout.fillWidth: true
            text: root.hasForecast ? root.controller.primaryLocation : qsTr("Your local forecast")
            color: ThemeModule.Theme.textDim
            elide: Text.ElideRight
            font.pixelSize: 12
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: root.hasForecast ? root.controller.primaryCurrent.icon || "☼" : "☼"; color: ThemeModule.Theme.text; font.pixelSize: 25 }
            Label {
                text: root.hasForecast && root.controller.primaryCurrent.hasTemperature
                    ? Math.round(root.controller.primaryCurrent.temperature) + "°" : "—"
                color: ThemeModule.Theme.text
                font.pixelSize: 26
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                text: root.hasForecast ? root.controller.primaryCurrent.condition || qsTr("Unavailable")
                                      : root.controller.primaryLoading ? qsTr("Loading…") : qsTr("Choose a city")
                color: ThemeModule.Theme.text
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }
        Label {
            Layout.fillWidth: true
            text: root.hasForecast ? qsTr("High %1° · Low %2°").arg(Math.round(root.controller.primaryToday.maximum)).arg(Math.round(root.controller.primaryToday.minimum))
                                  : qsTr("Open Weather to search for a city")
            color: ThemeModule.Theme.textDim
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            visible: root.hasForecast && root.controller.primaryError.length > 0
            text: root.controller.primaryStatus
            color: ThemeModule.Theme.warning
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }
}

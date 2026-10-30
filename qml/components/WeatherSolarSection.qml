import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Something.App as ThemeModule

Item {
    id: root
    property var solarInfo: ({})
    property string dayLabel: ""
    implicitHeight: panel.implicitHeight
    Accessible.name: qsTr("Sunrise and sunset information")

    function flag(name) {
        return Boolean(solarInfo && solarInfo[name] === true)
    }
    function hasValue(name) {
        return flag("has" + name)
    }
    function value(name) {
        return solarInfo && solarInfo[name] !== undefined
                ? String(solarInfo[name]) : ""
    }
    function stateText() {
        switch (value("solarState")) {
        case "beforeSunrise": return qsTr("Before sunrise")
        case "daylight": return qsTr("Daylight")
        case "afterSunset": return qsTr("After sunset")
        case "polarDay": return qsTr("Polar day")
        case "polarNight": return qsTr("Polar night")
        default: return ""
        }
    }
    function eventText() {
        switch (value("nextEvent")) {
        case "sunrise": return qsTr("Sunrise")
        case "sunset": return qsTr("Sunset")
        default: return ""
        }
    }
    function remainingText(seconds) {
        const totalMinutes = Math.ceil(Math.max(0, Number(seconds)) / 60)
        const hours = Math.floor(totalMinutes / 60)
        const minutes = totalMinutes % 60
        if (hours > 0 && minutes > 0) return qsTr("%1h %2m").arg(hours).arg(minutes)
        if (hours > 0) return qsTr("%1h").arg(hours)
        return qsTr("%1m").arg(minutes)
    }

    Rectangle {
        id: panel
        anchors.fill: parent
        implicitHeight: solarContent.implicitHeight + 24
        radius: ThemeModule.Theme.panelRadius
        color: ThemeModule.Theme.insetSurface
        border.color: ThemeModule.Theme.borderColor

        ColumnLayout {
            id: solarContent
            anchors.fill: parent
            anchors.margins: 12
            spacing: 9

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Sunrise & Sunset")
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }
                Label {
                    text: root.dayLabel
                    visible: text.length > 0
                    color: ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: ThemeModule.Theme.metadataFontSize
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: width >= 600 ? 3 : width >= 420 ? 2 : 1
                columnSpacing: 8
                rowSpacing: 8

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 58
                    color: ThemeModule.Theme.raisedSurface
                    border.color: ThemeModule.Theme.borderColor
                    radius: ThemeModule.Theme.panelRadius
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 7
                        Label { text: "↗"; color: ThemeModule.Theme.accentCyan; font.pixelSize: 20 }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: root.hasValue("Sunrise") ? root.value("sunrise") : "—"
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Label {
                                text: qsTr("Sunrise")
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: ThemeModule.Theme.metadataFontSize
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 58
                    color: ThemeModule.Theme.raisedSurface
                    border.color: ThemeModule.Theme.borderColor
                    radius: ThemeModule.Theme.panelRadius
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 7
                        Label { text: "↘"; color: ThemeModule.Theme.warning; font.pixelSize: 20 }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: root.hasValue("Sunset") ? root.value("sunset") : "—"
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Label {
                                text: qsTr("Sunset")
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: ThemeModule.Theme.metadataFontSize
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 58
                    color: ThemeModule.Theme.raisedSurface
                    border.color: ThemeModule.Theme.borderColor
                    radius: ThemeModule.Theme.panelRadius
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 7
                        Label { text: "☀"; color: ThemeModule.Theme.accentCyanLight; font.pixelSize: 19 }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: root.hasValue("DaylightDuration")
                                      ? root.value("daylightDuration") : "—"
                                color: ThemeModule.Theme.textPrimary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Label {
                                text: qsTr("Daylight")
                                color: ThemeModule.Theme.textSecondary
                                font.family: ThemeModule.Theme.fontFamily
                                font.pixelSize: ThemeModule.Theme.metadataFontSize
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.flag("hasSolarState") || root.flag("hasNextEvent")
                spacing: 8
                Label {
                    visible: root.flag("hasSolarState")
                    text: root.stateText()
                    color: ThemeModule.Theme.accentCyanLight
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: ThemeModule.Theme.metadataFontSize
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.flag("hasNextEvent")
                    text: qsTr("%1 in %2").arg(root.eventText())
                             .arg(root.remainingText(root.value("secondsUntilNextEvent")))
                    color: ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: ThemeModule.Theme.metadataFontSize
                    horizontalAlignment: Text.AlignRight
                    wrapMode: Text.Wrap
                }
            }

            Label {
                Layout.fillWidth: true
                visible: !root.hasValue("Sunrise") && !root.hasValue("Sunset")
                         && !root.hasValue("DaylightDuration")
                         && !root.flag("hasSolarState")
                         && !root.flag("hasNextEvent")
                text: qsTr("Solar information is unavailable for this day.")
                color: ThemeModule.Theme.textDisabled
                font.family: ThemeModule.Theme.fontFamily
                font.pixelSize: ThemeModule.Theme.metadataFontSize
                wrapMode: Text.Wrap
            }
        }
    }
}

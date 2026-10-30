pragma Singleton

import QtQuick

QtObject {
    readonly property color windowBackground: "#030A19"
    readonly property color panelSurface: "#071329"
    readonly property color raisedSurface: "#0B1C36"
    readonly property color insetSurface: "#020916"
    readonly property color hoverSurface: "#10244F"
    readonly property color borderColor: "#294A78"
    readonly property color strongBorder: "#5278B2"
    readonly property color shadowBorder: "#10244F"
    readonly property color focusHighlight: "#7DADFF"
    readonly property color textPrimary: "#EAF2FF"
    readonly property color textSecondary: "#9EBCEB"
    readonly property color textDisabled: "#6F86A9"
    readonly property color accentCyan: "#00D9FF"
    readonly property color accentCyanLight: "#70E9FF"
    readonly property color accentViolet: "#B388FF"
    readonly property color selectionBlue: "#0752E6"
    readonly property color success: "#28EAA0"
    readonly property color warning: "#FFD44A"
    readonly property color error: "#FF537D"
    readonly property color pendingSurface: "#0D2B3A"
    readonly property color warningSurface: "#352B12"
    readonly property color errorSurface: "#3C1727"
    readonly property color destructiveSurface: "#42172B"
    readonly property color destructiveHover: "#5B1D38"
    readonly property color primaryButtonHover: "#1262F4"
    readonly property color primaryButtonPressed: "#043EB6"
    readonly property color disabledSurface: "#0B172B"
    readonly property color scrim: "#B8030A19"
    readonly property string orbitRingColor: "rgba(41, 74, 120, 0.74)"
    readonly property string orbitSpokeColor: "rgba(0, 217, 255, 0.16)"
    readonly property int panelRadius: 2
    readonly property int controlRadius: 2
    readonly property int badgeRadius: 2
    readonly property int panelBorderWidth: 1
    readonly property int focusBorderWidth: 2
    readonly property int controlMotion: 120
    readonly property int pageMotion: 180
    readonly property string fontFamily: "Cascadia Code"
    readonly property int bodyFontSize: 14
    readonly property int labelFontSize: 13
    readonly property int headingFontSize: 20
    readonly property int metadataFontSize: 11
    readonly property int controlPadding: 12
    readonly property int panelPadding: 16
    readonly property int controlSpacing: 8
    readonly property int cardContentSpacing: 10
}

pragma Singleton

import QtQuick

QtObject {
    readonly property color background: "#06070d"
    readonly property color panel: "#11131d"
    readonly property color panelAlt: "#171a2a"
    readonly property color border: "#2a2f46"
    readonly property color text: "#f5f7ff"
    readonly property color textDim: "#9aa4c7"
    readonly property color accent: "#7c8cff"
    readonly property color accentHover: "#9ca9ff"
    readonly property color cyan: "#22d3ee"
    readonly property color pink: "#ff4fd8"
    readonly property color error: "#ff6b8a"
    readonly property color success: "#49f2a4"
    readonly property color warning: "#ffbf4d"
    readonly property int cornerRadius: 14
    readonly property int controlMotion: 120
    readonly property int pageMotion: 180
    readonly property string displayFont: "Segoe UI Variable Display"
    readonly property string bodyFont: "Segoe UI"
}

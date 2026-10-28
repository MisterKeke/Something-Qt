import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

Label {
    id: root

    property color badgeColor: ThemeModule.Theme.accentCyan

    leftPadding: 9
    rightPadding: 9
    topPadding: 4
    bottomPadding: 4
    font.family: ThemeModule.Theme.fontFamily
    font.pixelSize: ThemeModule.Theme.metadataFontSize
    font.weight: Font.DemiBold
    color: ThemeModule.Theme.textPrimary

    background: Rectangle {
        radius: ThemeModule.Theme.badgeRadius
        color: Qt.alpha(root.badgeColor, 0.2)
        border.color: Qt.alpha(root.badgeColor, 0.56)
        border.width: 1
    }
}

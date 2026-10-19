import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

Label {
    id: root

    property color badgeColor: ThemeModule.Theme.accent

    leftPadding: 9
    rightPadding: 9
    topPadding: 4
    bottomPadding: 4
    font.family: ThemeModule.Theme.bodyFont
    font.pixelSize: 11
    font.weight: Font.DemiBold
    color: ThemeModule.Theme.text

    background: Rectangle {
        radius: height / 2
        color: Qt.alpha(root.badgeColor, 0.2)
        border.color: Qt.alpha(root.badgeColor, 0.56)
        border.width: 1
    }
}

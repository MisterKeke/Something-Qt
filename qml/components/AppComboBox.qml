import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

ComboBox {
    id: root

    property string accessibleLabel: ""
    implicitHeight: 42
    hoverEnabled: true
    font.family: ThemeModule.Theme.fontFamily
    font.pixelSize: ThemeModule.Theme.labelFontSize
    Accessible.name: accessibleLabel.length > 0 ? accessibleLabel : currentText

    contentItem: Text {
        leftPadding: ThemeModule.Theme.controlPadding - 1
        rightPadding: 30
        text: root.displayText
        textFormat: Text.PlainText
        color: root.enabled ? ThemeModule.Theme.textPrimary : ThemeModule.Theme.textDisabled
        font: root.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: root.width - width - 12
        y: root.topPadding + (root.availableHeight - height) / 2
        text: "⌄"
        color: root.enabled ? ThemeModule.Theme.textSecondary : ThemeModule.Theme.textDisabled
        font.pixelSize: 16
    }

    background: Rectangle {
        radius: ThemeModule.Theme.controlRadius
        color: !root.enabled ? ThemeModule.Theme.disabledSurface
               : root.popup.visible ? ThemeModule.Theme.raisedSurface
               : root.hovered && !root.activeFocus ? ThemeModule.Theme.raisedSurface
               : ThemeModule.Theme.insetSurface
        border.width: root.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
        border.color: root.activeFocus ? ThemeModule.Theme.focusHighlight
                     : root.popup.visible ? ThemeModule.Theme.strongBorder : ThemeModule.Theme.borderColor
        Behavior on border.color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
    }

    popup: Popup {
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: ThemeModule.Theme.controlMotion; easing.type: Easing.OutCubic } }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: ThemeModule.Theme.controlMotion } }
        y: root.height - 1
        width: root.width
        implicitHeight: Math.min(contentItem.implicitHeight, 280)
        padding: 4
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            ScrollBar.vertical: ThemeModule.AppScrollBar { }
        }
        background: Rectangle {
            color: ThemeModule.Theme.raisedSurface
            border.color: ThemeModule.Theme.strongBorder
            radius: ThemeModule.Theme.panelRadius
        }
    }

    delegate: ItemDelegate {
        width: root.width - 8
        contentItem: Text {
            text: {
                if (root.textRole.length === 0) return modelData
                if (typeof modelData !== "undefined" && modelData !== null
                        && typeof modelData[root.textRole] !== "undefined")
                    return modelData[root.textRole]
                return model[root.textRole]
            }
            textFormat: Text.PlainText
            color: !root.enabled ? ThemeModule.Theme.textDisabled
                   : highlighted ? ThemeModule.Theme.textPrimary : ThemeModule.Theme.textSecondary
            font: root.font
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        highlighted: root.highlightedIndex === index
        background: Rectangle {
            radius: ThemeModule.Theme.controlRadius
            color: parent.down ? ThemeModule.Theme.shadowBorder
                   : parent.highlighted ? ThemeModule.Theme.hoverSurface : "transparent"
            Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        }
    }
}

import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

ComboBox {
    id: root

    property string accessibleLabel: ""
    implicitHeight: 42
    font.family: ThemeModule.Theme.bodyFont
    font.pixelSize: 13
    Accessible.name: accessibleLabel.length > 0 ? accessibleLabel : currentText

    contentItem: Text {
        leftPadding: 11
        rightPadding: 30
        text: root.displayText
        color: ThemeModule.Theme.text
        font: root.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: root.width - width - 12
        y: root.topPadding + (root.availableHeight - height) / 2
        text: "⌄"
        color: ThemeModule.Theme.textDim
        font.pixelSize: 16
    }

    background: Rectangle {
        radius: 9
        color: "#0d101a"
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? ThemeModule.Theme.cyan : ThemeModule.Theme.border
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
            ScrollIndicator.vertical: ScrollIndicator { }
        }
        background: Rectangle {
            color: ThemeModule.Theme.panelAlt
            border.color: ThemeModule.Theme.accent
            radius: 9
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
            color: highlighted ? ThemeModule.Theme.text : ThemeModule.Theme.textDim
            font: root.font
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        highlighted: root.highlightedIndex === index
        background: Rectangle {
            radius: 6
            color: parent.highlighted ? "#383d67" : "transparent"
            Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        }
    }
}

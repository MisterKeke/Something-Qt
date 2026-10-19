import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import "../dialogs" as Dialogs
import Something.App as ThemeModule

Item {
    id: root

    property string providerSource: "telegram"
    property bool telegram: providerSource === "telegram"
    readonly property var contentModel: telegram ? favoritesController.telegramPosts
                                                  : favoritesController.youTubeVideos
    readonly property var favoriteModel: telegram ? favoritesController.telegramFavorites
                                                   : favoritesController.youTubeFavorites
    readonly property var categoriesModel: telegram ? favoritesController.telegramCategories
                                                     : favoritesController.youTubeCategories
    property string categoryToDelete: ""
    property string categoryToDeleteName: ""

    implicitHeight: pageColumn.implicitHeight

    function activeReference() {
        return telegram ? favoritesController.telegramChannel : favoritesController.youTubeChannel
    }

    function captureViewState() {
        return { contentListY: contentList.contentY }
    }

    function restoreViewState(state) {
        Qt.callLater(function() {
            contentList.contentY = Number(state.contentListY || 0)
        })
    }

    function formatTimestamp(value) {
        if (!value) return ""
        const parsed = new Date(value)
        return Number.isNaN(parsed.getTime()) ? value : parsed.toLocaleString()
    }

    function reloadReference() {
        channelField.text = activeReference()
    }

    function openDeleteCategory(categoryId, categoryName) {
        categoryToDelete = String(categoryId)
        categoryToDeleteName = categoryName
        deleteCategoryDialog.title = qsTr("Delete category?")
        deleteCategoryDialog.open()
    }

    onProviderSourceChanged: {
        favoritesController.setSource(providerSource)
        reloadReference()
    }

    Component.onCompleted: {
        favoritesController.setSource(providerSource)
        reloadReference()
    }

    Connections {
        target: favoritesController
        function onChannelReferenceChanged(source, reference) {
            if (source === root.providerSource) channelField.text = reference
        }
        function onStateChanged() {
            if (addFavoriteDialog.waitingForSave && !favoritesController.mutationBusy
                    && favoritesController.mutationError.length === 0) {
                addFavoriteDialog.waitingForSave = false
                addFavoriteDialog.close()
            }
        }
    }

    ColumnLayout {
        id: pageColumn
        width: root.width
        spacing: 14

        Shared.AppCard {
            Layout.fillWidth: true
            padding: 16

            ColumnLayout {
                width: parent.width
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: root.telegram ? qsTr("Public Telegram channel") : qsTr("YouTube channel")
                        color: ThemeModule.Theme.text
                        font.family: ThemeModule.Theme.displayFont
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }
                    Shared.AppButton {
                        text: qsTr("Categories")
                        quiet: true
                        onClicked: categoryDialog.open()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Shared.AppTextField {
                        id: channelField
                        Layout.fillWidth: true
                        accessibleLabel: root.telegram ? qsTr("Telegram channel username")
                                                       : qsTr("YouTube channel ID or handle")
                        placeholderText: root.telegram ? qsTr("Channel username")
                                                       : qsTr("Channel ID or @handle")
                        onAccepted: favoritesController.loadChannel(root.providerSource, text, true)
                    }
                    Shared.AppButton {
                        text: qsTr("Load")
                        enabled: !favoritesController.telegramLoading && !favoritesController.youTubeLoading
                        onClicked: favoritesController.loadChannel(root.providerSource, channelField.text, true)
                    }
                    Shared.AppButton {
                        text: qsTr("Refresh")
                        quiet: true
                        enabled: channelField.text.length > 0
                                 && !favoritesController.telegramLoading && !favoritesController.youTubeLoading
                        onClicked: favoritesController.loadChannel(root.providerSource, channelField.text, true)
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.telegram ? favoritesController.telegramError.length > 0
                                           : favoritesController.youTubeError.length > 0
                    text: root.telegram ? favoritesController.telegramError : favoritesController.youTubeError
                    textFormat: Text.PlainText
                    color: ThemeModule.Theme.error
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: activeReference().length > 0
                    Shared.StatusBadge {
                        text: root.telegram ? qsTr("Telegram") : qsTr("YouTube")
                        badgeColor: root.telegram ? ThemeModule.Theme.cyan : ThemeModule.Theme.error
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.telegram ? "@" + favoritesController.telegramChannel
                                           : (favoritesController.youTubeChannelId.length > 0
                                              ? favoritesController.youTubeChannelId
                                              : favoritesController.youTubeChannel)
                        textFormat: Text.PlainText
                        color: ThemeModule.Theme.textDim
                        font.family: ThemeModule.Theme.bodyFont
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                    }
                    Shared.AppButton {
                        text: qsTr("Add favorite")
                                enabled: !favoritesController.mutationBusy
                                         && (root.telegram || favoritesController.youTubeChannelId.length > 0)
                        onClicked: addFavoriteDialog.open()
                    }
                }

                Label {
                    visible: favoritesController.mutationError.length > 0
                    Layout.fillWidth: true
                    text: favoritesController.mutationError
                    textFormat: Text.PlainText
                    color: ThemeModule.Theme.error
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }
                Label {
                    visible: favoritesController.externalOpenError.length > 0
                    Layout.fillWidth: true
                    text: favoritesController.externalOpenError
                    textFormat: Text.PlainText
                    color: ThemeModule.Theme.error
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }
            }
        }

        Shared.AppCard {
            Layout.fillWidth: true
            padding: 16
            ColumnLayout {
                width: parent.width
                spacing: 9
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Favorites")
                        color: ThemeModule.Theme.text
                        font.family: ThemeModule.Theme.displayFont
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                    }
                    BusyIndicator {
                        visible: favoritesController.favoritesLoading
                        running: visible
                        implicitWidth: 24
                        implicitHeight: 24
                        Accessible.name: qsTr("Loading favorites")
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 7
                    Shared.FilterChip {
                        text: qsTr("All")
                        selected: favoriteModel.categoryFilter === "all"
                        onClicked: favoriteModel.categoryFilter = "all"
                    }
                    Shared.FilterChip {
                        text: qsTr("Uncategorized")
                        selected: favoriteModel.categoryFilter === "uncategorized"
                        onClicked: favoriteModel.categoryFilter = "uncategorized"
                    }
                    Repeater {
                        model: categoriesModel
                        delegate: Shared.FilterChip {
                            required property string name
                            required property string color
                            required property var categoryId
                            text: name
                            tint: color.length > 0 ? color : ThemeModule.Theme.accent
                            selected: favoriteModel.categoryFilter === String(categoryId)
                            onClicked: favoriteModel.categoryFilter = String(categoryId)
                        }
                    }
                }

                Shared.StatePanel {
                    Layout.fillWidth: true
                    visible: favoriteModel.count === 0
                    state: favoritesController.favoritesLoading ? "loading" : "empty"
                    title: qsTr("No matching favorites")
                    message: qsTr("Save a channel from above or choose a different category filter.")
                }

                Repeater {
                    model: favoriteModel
                    delegate: Rectangle {
                        required property string sourceId
                        required property string label
                        required property var categoryId
                        required property string categoryName
                        required property string categoryColor
                        Layout.fillWidth: true
                        implicitHeight: favoriteRow.implicitHeight + 16
                        radius: 10
                        color: ThemeModule.Theme.panelAlt
                        border.color: ThemeModule.Theme.border

                        RowLayout {
                            id: favoriteRow
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 8
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    Layout.fillWidth: true
                                    text: root.telegram ? "@" + label : label
                                    textFormat: Text.PlainText
                                    color: ThemeModule.Theme.text
                                    font.family: ThemeModule.Theme.bodyFont
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                RowLayout {
                                    spacing: 5
                                    Rectangle {
                                        width: 7
                                        height: 7
                                        radius: 4
                                        visible: categoryColor.length > 0
                                        color: visible ? categoryColor : "transparent"
                                        Accessible.ignored: true
                                    }
                                    Label {
                                        text: categoryName.length > 0 ? categoryName : qsTr("Uncategorized")
                                        textFormat: Text.PlainText
                                        color: ThemeModule.Theme.textDim
                                        font.pixelSize: 10
                                    }
                                }
                            }
                            Shared.AppButton {
                                text: qsTr("Open")
                                quiet: true
                                onClicked: favoritesController.loadChannel(root.providerSource, sourceId, true)
                            }
                            Shared.AppComboBox {
                                id: assignCategory
                                Layout.preferredWidth: 170
                                enabled: !favoritesController.mutationBusy
                                accessibleLabel: qsTr("Assign favorite to category")
                                textRole: "name"
                                valueRole: "categoryId"
                                model: categoriesModel
                                currentIndex: categoryId === undefined || categoryId === null
                                              ? -1 : indexOfValue(Number(categoryId))
                                onActivated: favoritesController.assignFavoriteCategory(
                                    root.providerSource, sourceId, String(currentValue))
                            }
                            Shared.AppButton {
                                text: qsTr("Clear category")
                                quiet: true
                                enabled: !favoritesController.mutationBusy
                                onClicked: favoritesController.assignFavoriteCategory(
                                    root.providerSource, sourceId, "uncategorized")
                            }
                            Shared.AppButton {
                                text: qsTr("Remove")
                                destructive: true
                                quiet: true
                                enabled: !favoritesController.mutationBusy
                                onClicked: removeFavoriteDialog.openFor(sourceId, label)
                            }
                        }
                    }
                }
            }
        }

        Shared.AppCard {
            Layout.fillWidth: true
            padding: 16
            ColumnLayout {
                width: parent.width
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: root.telegram ? qsTr("Recent posts") : qsTr("Recent videos")
                        color: ThemeModule.Theme.text
                        font.family: ThemeModule.Theme.displayFont
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                    }
                    BusyIndicator {
                        visible: root.telegram ? favoritesController.telegramLoading
                                               : favoritesController.youTubeLoading
                        running: visible
                        implicitWidth: 24
                        implicitHeight: 24
                        Accessible.name: qsTr("Loading provider content")
                    }
                }

                Shared.StatePanel {
                    Layout.fillWidth: true
                    visible: contentModel.count === 0
                    state: (root.telegram ? favoritesController.telegramError.length > 0
                                          : favoritesController.youTubeError.length > 0) ? "error"
                           : ((root.telegram ? favoritesController.telegramLoading
                                             : favoritesController.youTubeLoading) ? "loading" : "empty")
                    title: (root.telegram ? favoritesController.telegramError.length > 0
                                          : favoritesController.youTubeError.length > 0)
                           ? qsTr("Provider content is unavailable") : qsTr("No recent content")
                    message: (root.telegram ? favoritesController.telegramError
                                            : favoritesController.youTubeError)
                }

                ListView {
                    id: contentList
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(720, Math.max(0, contentHeight))
                    visible: contentModel.count > 0
                    clip: true
                    spacing: 10
                    boundsBehavior: Flickable.StopAtBounds
                    Accessible.name: root.telegram ? qsTr("Recent Telegram posts")
                                                    : qsTr("Recent YouTube videos")
                    model: contentModel
                    delegate: Shared.AppCard {
                        id: itemCard
                        required property string itemId
                        required property string title
                        required property string text
                        required property string preview
                        required property string publishedAt
                        required property string views
                        required property var images
                        required property var imageSources
                        required property string externalUrl
                        required property string channelTitle
                        width: ListView.view.width
                        height: implicitHeight
                        padding: 13

                        ColumnLayout {
                            width: parent.width
                            spacing: 7
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: root.telegram ? "@" + favoritesController.telegramChannel
                                                       : (itemCard.channelTitle.length > 0
                                                          ? itemCard.channelTitle : itemCard.title)
                                    textFormat: Text.PlainText
                                    color: ThemeModule.Theme.cyan
                                    font.family: ThemeModule.Theme.bodyFont
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: root.formatTimestamp(publishedAt)
                                    textFormat: Text.PlainText
                                    color: ThemeModule.Theme.textDim
                                    font.family: ThemeModule.Theme.bodyFont
                                    font.pixelSize: 10
                                }
                                Label {
                                    visible: views.length > 0
                                    text: views
                                    textFormat: Text.PlainText
                                    color: ThemeModule.Theme.textDim
                                    font.family: ThemeModule.Theme.bodyFont
                                    font.pixelSize: 10
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: !root.telegram
                                text: itemCard.title
                                textFormat: Text.PlainText
                                color: ThemeModule.Theme.text
                                font.family: ThemeModule.Theme.displayFont
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                wrapMode: Text.Wrap
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: itemCard.text.length > 0
                                text: itemCard.text
                                textFormat: Text.PlainText
                                color: ThemeModule.Theme.textDim
                                font.family: ThemeModule.Theme.bodyFont
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                                maximumLineCount: 10
                                elide: Text.ElideRight
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: 6
                                Repeater {
                                    model: itemCard.imageSources
                                    delegate: Image {
                                        required property string modelData
                                        visible: modelData.length > 0
                                        source: modelData
                                        width: visible ? 176 : 0
                                        height: visible ? 110 : 0
                                        sourceSize: Qt.size(352, 220)
                                        fillMode: Image.PreserveAspectCrop
                                        asynchronous: true
                                        cache: true
                                        smooth: true
                                        Accessible.name: qsTr("Provider media preview")
                                    }
                                }
                            }
                            Shared.AppButton {
                                text: root.telegram ? qsTr("Open post") : qsTr("Open video")
                                quiet: true
                                onClicked: favoritesController.openExternal(root.providerSource,
                                                                            itemCard.externalUrl)
                            }
                        }
                    }
                }

                Shared.AppButton {
                    visible: root.telegram && favoritesController.telegramHasOlder
                    enabled: !favoritesController.telegramLoading
                    text: favoritesController.telegramLoading ? qsTr("Loading…") : qsTr("Load older posts")
                    quiet: true
                    onClicked: favoritesController.loadOlderTelegramPosts()
                }
                Label {
                    visible: !root.telegram && contentModel.count > 0
                    text: qsTr("Showing the recent feed. YouTube does not provide paging here.")
                    color: ThemeModule.Theme.textDim
                    font.pixelSize: 10
                }
            }
        }
    }

    Dialog {
        id: addFavoriteDialog
        property bool waitingForSave: false
        modal: true
        focus: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, root.width - 30)
        title: qsTr("Save favorite channel")
        standardButtons: Dialog.NoButton
        closePolicy: Popup.NoAutoClose
        onOpened: {
            useCategory.checked = categoriesModel.count > 0
            categoryTarget.currentIndex = categoriesModel.count > 0 ? 0 : -1
            newCategoryName.clear()
            newCategoryColor.clear()
        }
        onRejected: waitingForSave = false
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                Layout.fillWidth: true
                text: root.telegram ? "@" + favoritesController.telegramChannel
                                    : favoritesController.youTubeChannelId
                color: ThemeModule.Theme.textDim
                wrapMode: Text.Wrap
            }
            CheckBox {
                id: useCategory
                text: qsTr("Assign to an existing category")
                checked: categoriesModel.count > 0
                Accessible.name: text
            }
            Shared.AppComboBox {
                id: categoryTarget
                Layout.fillWidth: true
                visible: useCategory.checked
                enabled: visible
                accessibleLabel: qsTr("Favorite category")
                textRole: "name"
                valueRole: "categoryId"
                model: categoriesModel
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Or create a category and save the channel to it.")
                color: ThemeModule.Theme.textDim
                wrapMode: Text.Wrap
            }
            Shared.AppTextField {
                id: newCategoryName
                Layout.fillWidth: true
                accessibleLabel: qsTr("New category name")
                placeholderText: qsTr("Category name")
            }
            Shared.AppTextField {
                id: newCategoryColor
                Layout.fillWidth: true
                accessibleLabel: qsTr("New category color")
                placeholderText: qsTr("Optional color, for example #7c8cff")
            }
            Label {
                visible: favoritesController.mutationError.length > 0
                Layout.fillWidth: true
                text: favoritesController.mutationError
                textFormat: Text.PlainText
                color: ThemeModule.Theme.error
                wrapMode: Text.Wrap
            }
            Shared.AppButton {
                Layout.alignment: Qt.AlignRight
                text: favoritesController.mutationBusy ? qsTr("Saving…") : qsTr("Save favorite")
                enabled: !favoritesController.mutationBusy
                         && (root.telegram || favoritesController.youTubeChannelId.length > 0)
                         && (useCategory.checked ? categoriesModel.count > 0
                                                 : newCategoryName.text.trim().length > 0)
                onClicked: {
                    addFavoriteDialog.waitingForSave = true
                    favoritesController.addFavorite(root.providerSource,
                        useCategory.checked ? String(categoryTarget.currentValue) : "uncategorized",
                        useCategory.checked ? "" : newCategoryName.text,
                        useCategory.checked ? "" : newCategoryColor.text)
                }
            }
        }
        footer: DialogButtonBox {
            alignment: Qt.AlignRight
            Shared.AppButton {
                text: qsTr("Cancel")
                quiet: true
                enabled: !favoritesController.mutationBusy
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
        background: Rectangle {
            color: ThemeModule.Theme.panelAlt
            border.color: ThemeModule.Theme.accent
            radius: 14
        }
    }

    Dialog {
        id: categoryDialog
        modal: true
        focus: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(820, root.width - 24)
        height: Math.min(620, root.height - 24)
        title: qsTr("Manage %1 categories").arg(root.telegram ? "Telegram" : "YouTube")
        standardButtons: Dialog.Close
        contentItem: ColumnLayout {
            spacing: 9
            RowLayout {
                Layout.fillWidth: true
                Shared.AppTextField {
                    id: createCategoryName
                    Layout.fillWidth: true
                    accessibleLabel: qsTr("New category name")
                    placeholderText: qsTr("New category")
                    onAccepted: createCategoryButton.clicked()
                }
                Shared.AppTextField {
                    id: createCategoryColor
                    Layout.preferredWidth: 150
                    accessibleLabel: qsTr("New category color")
                    placeholderText: qsTr("#7c8cff")
                }
                Shared.AppButton {
                    id: createCategoryButton
                    text: qsTr("Add")
                    enabled: !favoritesController.mutationBusy
                             && createCategoryName.text.trim().length > 0
                    onClicked: {
                        favoritesController.createCategory(root.providerSource,
                                                           createCategoryName.text,
                                                           createCategoryColor.text)
                    }
                }
            }
            Label {
                visible: favoritesController.mutationError.length > 0
                Layout.fillWidth: true
                text: favoritesController.mutationError
                textFormat: Text.PlainText
                color: ThemeModule.Theme.error
                wrapMode: Text.Wrap
            }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 7
                model: categoriesModel
                delegate: Item {
                    required property var categoryId
                    required property string name
                    required property string color
                    required property int index
                    width: ListView.view.width
                    height: editorRow.implicitHeight + 14
                    Rectangle {
                        anchors.fill: parent
                        radius: 9
                        color: ThemeModule.Theme.panel
                        border.color: ThemeModule.Theme.border
                    }
                    RowLayout {
                        id: editorRow
                        anchors.fill: parent
                        anchors.margins: 7
                        Shared.AppTextField {
                            id: nameEditor
                            Layout.fillWidth: true
                            text: name
                            accessibleLabel: qsTr("Category name")
                        }
                        Shared.AppTextField {
                            id: colorEditor
                            Layout.preferredWidth: 120
                            text: color
                            accessibleLabel: qsTr("Category color")
                            placeholderText: qsTr("Optional")
                        }
                        Shared.AppButton {
                            text: qsTr("Save")
                            quiet: true
                            enabled: !favoritesController.mutationBusy
                            onClicked: favoritesController.updateCategory(root.providerSource,
                                Number(categoryId), nameEditor.text, colorEditor.text)
                        }
                        Shared.AppButton {
                            text: qsTr("↑")
                            quiet: true
                            enabled: !favoritesController.mutationBusy && index > 0
                            Accessible.name: qsTr("Move category up")
                            onClicked: favoritesController.moveCategory(root.providerSource,
                                Number(categoryId), -1)
                        }
                        Shared.AppButton {
                            text: qsTr("↓")
                            quiet: true
                            enabled: !favoritesController.mutationBusy
                                     && index < categoriesModel.count - 1
                            Accessible.name: qsTr("Move category down")
                            onClicked: favoritesController.moveCategory(root.providerSource,
                                Number(categoryId), 1)
                        }
                        Shared.AppButton {
                            text: qsTr("Delete…")
                            destructive: true
                            quiet: true
                            enabled: !favoritesController.mutationBusy
                            onClicked: root.openDeleteCategory(categoryId, name)
                        }
                    }
                }
            }
            Shared.StatePanel {
                Layout.fillWidth: true
                visible: categoriesModel.count === 0
                state: "empty"
                title: qsTr("No categories")
                message: qsTr("Create one above to organize this source's favorites.")
            }
        }
        background: Rectangle {
            color: ThemeModule.Theme.panelAlt
            border.color: ThemeModule.Theme.accent
            radius: 14
        }
    }

    Dialog {
        id: deleteCategoryDialog
        modal: true
        focus: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, root.width - 24)
        title: qsTr("Delete category")
        standardButtons: Dialog.Cancel
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                Layout.fillWidth: true
                text: qsTr("Delete “%1”? Choose whether assigned favorites become uncategorized or move to another category.")
                    .arg(root.categoryToDeleteName)
                textFormat: Text.PlainText
                color: ThemeModule.Theme.textDim
                wrapMode: Text.Wrap
            }
            Shared.AppComboBox {
                id: moveTarget
                Layout.fillWidth: true
                accessibleLabel: qsTr("Destination category")
                textRole: "name"
                valueRole: "categoryId"
                model: root.deleteTargetModel()
            }
            RowLayout {
                Layout.fillWidth: true
                Shared.AppButton {
                    text: qsTr("Delete and unassign")
                    destructive: true
                    enabled: !favoritesController.mutationBusy
                    onClicked: {
                        favoritesController.deleteCategory(root.providerSource,
                            Number(root.categoryToDelete), "unassign")
                        deleteCategoryDialog.close()
                    }
                }
                Shared.AppButton {
                    Layout.fillWidth: true
                    text: qsTr("Delete and move")
                    destructive: true
                    enabled: !favoritesController.mutationBusy && moveTarget.count > 0
                    onClicked: {
                        favoritesController.deleteCategory(root.providerSource,
                            Number(root.categoryToDelete), "move", String(moveTarget.currentValue))
                        deleteCategoryDialog.close()
                    }
                }
            }
        }
        background: Rectangle {
            color: ThemeModule.Theme.panelAlt
            border.color: ThemeModule.Theme.accent
            radius: 14
        }
    }

    Dialogs.ConfirmationDialog {
        id: removeFavoriteDialog
        property string sourceId: ""
        function openFor(id, name) {
            sourceId = id
            title = qsTr("Remove favorite?")
            message = qsTr("Remove %1 from this source's favorites?").arg(name)
            confirmText = qsTr("Remove")
            destructive = true
            open()
        }
        onConfirmed: favoritesController.removeFavorite(root.providerSource, sourceId)
    }

    function deleteTargetModel() {
        const choices = []
        for (let row = 0; row < categoriesModel.count; ++row) {
            const category = categoriesModel.rowAt(row)
            if (String(category.id) !== categoryToDelete)
                choices.push({ name: category.name, categoryId: category.id })
        }
        return choices
    }
}

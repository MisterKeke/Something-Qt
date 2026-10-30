import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Shared
import Something.App as ThemeModule

Item {
    id: root

    property var controller
    property var savedViewState: ({})
    property real selectedEditCollectionId: 0
    property real pendingDeleteCollectionId: 0
    property real pendingCollectionRestoreId: -1
    property var editGame: ({})
    property string editStatus: ""
    property int editRating: 0
    property string sortSelection: "name_asc"
    property string statusSelection: "all"
    property string lastHandledNotification: ""

    implicitHeight: content.implicitHeight + 8
    Accessible.name: qsTr("Steam Game Collections")

    function captureViewState() {
        return {
            selectedCollectionId: controller ? controller.selectedCollectionId : 0,
            search: searchField.text,
            sortOrder: sortSelection,
            statusFilter: statusSelection
        }
    }

    function restoreViewState(state) {
        savedViewState = state || {}
        searchField.text = String(savedViewState.search || "")
        sortSelection = String(savedViewState.sortOrder || "name_asc")
        statusSelection = String(savedViewState.statusFilter || "all")
        if (controller) {
            const restoredCollectionId = Number(savedViewState.selectedCollectionId || 0)
            if (controller.snapshotLoaded)
                controller.selectCollection(restoredCollectionId)
            else
                pendingCollectionRestoreId = restoredCollectionId
            controller.setSearchText(searchField.text)
            controller.setSortOrder(sortSelection)
            controller.setStatusFilter(statusSelection)
        }
        for (let i = 0; i < sortBox.count; ++i) {
            if (sortBox.valueAt(i) === sortSelection) {
                sortBox.currentIndex = i
                break
            }
        }
    }

    function collectionIndex(collectionId) {
        if (!controller) return -1
        for (let i = 0; i < controller.collections.length; ++i) {
            if (Number(controller.collections[i].id) === Number(collectionId))
                return i
        }
        return -1
    }

    function selectedCollection() {
        const index = collectionIndex(controller ? controller.selectedCollectionId : 0)
        return index < 0 ? ({}) : controller.collections[index]
    }

    function openCollectionEditor(collection) {
        if (controller) controller.clearMessages()
        selectedEditCollectionId = Number(collection ? collection.id : 0)
        collectionNameField.text = collection ? String(collection.name || "") : ""
        collectionDescriptionField.text = collection ? String(collection.description || "") : ""
        collectionColorField.text = collection ? String(collection.color || "") : ""
        collectionIconField.text = collection ? String(collection.icon || "") : ""
        collectionEditor.open()
    }

    function saveCollection() {
        if (!controller) return
        if (selectedEditCollectionId > 0) {
            controller.updateCollection(selectedEditCollectionId, collectionNameField.text,
                collectionDescriptionField.text, collectionColorField.text,
                collectionIconField.text)
        } else {
            controller.createCollection(collectionNameField.text,
                collectionDescriptionField.text, collectionColorField.text,
                collectionIconField.text)
        }
    }

    function openGameEditor(game) {
        if (!game || Number(game.collectionId) <= 0) return
        if (controller) controller.clearMessages()
        editGame = game
        editStatus = String(game.status || "")
        editRating = Number(game.rating || 0)
        gameCommentField.text = String(game.comment || "")
        gameEditor.open()
    }

    function saveGameDetails() {
        if (!controller || !editGame) return
        controller.updateGame(Number(editGame.collectionId), Number(editGame.appId),
            editStatus, editRating, gameCommentField.text)
    }

    function statusLabel(status) {
        switch (String(status || "")) {
        case "backlog": return qsTr("Backlog")
        case "playing": return qsTr("Playing")
        case "completed": return qsTr("Completed")
        case "paused": return qsTr("Paused")
        case "dropped": return qsTr("Dropped")
        default: return qsTr("No status")
        }
    }

    function ratingStars(rating) {
        const count = Math.max(0, Math.min(5, Number(rating) || 0))
        let stars = ""
        for (let index = 0; index < count; ++index)
            stars += "★"
        return stars
    }

    function hasMembership(game, collectionId) {
        if (!game || !game.memberships) return false
        for (let i = 0; i < game.memberships.length; ++i) {
            if (Number(game.memberships[i].collectionId) === Number(collectionId))
                return true
        }
        return false
    }

    function transferActions(game) {
        const actions = []
        if (!controller || !game || !game.memberships)
            return actions
        for (let sourceIndex = 0; sourceIndex < game.memberships.length; ++sourceIndex) {
            const source = game.memberships[sourceIndex]
            for (let targetIndex = 0; targetIndex < controller.collections.length; ++targetIndex) {
                const target = controller.collections[targetIndex]
                if (Number(source.collectionId) === Number(target.id)
                        || hasMembership(game, target.id))
                    continue
                actions.push({
                    appId: Number(game.appId),
                    sourceCollectionId: Number(source.collectionId),
                    sourceCollectionName: String(source.collectionName),
                    targetCollectionId: Number(target.id),
                    targetCollectionName: String(target.name)
                })
            }
        }
        return actions
    }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 14

        Shared.AppCard {
            Layout.fillWidth: true
            padding: 18
            ColumnLayout {
                width: parent.width
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Game Collections")
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }
                    Shared.AppButton {
                        text: qsTr("New collection")
                        enabled: root.controller && root.controller.snapshotLoaded
                                 && !root.controller.busy && !root.controller.addBusy
                        onClicked: root.openCollectionEditor(null)
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Organize games locally. Collections are independent from price tracking, release dates, and game updates.")
                    color: ThemeModule.Theme.textSecondary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 8

            Shared.FilterChip {
                text: qsTr("All games · %1")
                    .arg(root.controller ? root.controller.allGamesCount : 0)
                selected: root.controller && root.controller.selectedCollectionId === 0
                onClicked: if (root.controller) root.controller.selectCollection(0)
            }
            Repeater {
                model: root.controller ? root.controller.collections : []
                delegate: Shared.FilterChip {
                    required property var modelData
                    text: (modelData.icon && String(modelData.icon).length > 0
                           ? String(modelData.icon) + "  " : "")
                          + String(modelData.name) + " · " + Number(modelData.gameCount || 0)
                    selected: root.controller
                              && Number(root.controller.selectedCollectionId)
                                 === Number(modelData.id)
                    tint: String(modelData.color || "").length > 0
                          ? modelData.color : ThemeModule.Theme.accentCyan
                    onClicked: if (root.controller)
                        root.controller.selectCollection(Number(modelData.id))
                }
            }
        }

        Shared.AppCard {
            Layout.fillWidth: true
            padding: 16
            ColumnLayout {
                width: parent.width
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: root.controller && root.controller.selectedCollectionId > 0
                              ? root.controller.selectedCollectionName : qsTr("All games")
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Shared.AppButton {
                        text: qsTr("Rename / edit")
                        quiet: true
                        visible: root.controller && root.controller.selectedCollectionId > 0
                        enabled: root.controller && !root.controller.busy
                                 && !root.controller.addBusy
                        onClicked: root.openCollectionEditor(root.selectedCollection())
                    }
                    Shared.AppButton {
                        text: "↑"
                        quiet: true
                        Accessible.name: qsTr("Move collection earlier")
                        visible: root.controller && root.controller.selectedCollectionId > 0
                        enabled: root.collectionIndex(root.controller.selectedCollectionId) > 0
                                 && !root.controller.busy && !root.controller.addBusy
                        onClicked: root.controller.moveCollection(
                            Number(root.controller.selectedCollectionId), -1)
                    }
                    Shared.AppButton {
                        text: "↓"
                        quiet: true
                        Accessible.name: qsTr("Move collection later")
                        visible: root.controller && root.controller.selectedCollectionId > 0
                        enabled: root.collectionIndex(root.controller.selectedCollectionId) >= 0
                                 && root.collectionIndex(root.controller.selectedCollectionId)
                                    < root.controller.collections.length - 1
                                 && !root.controller.busy && !root.controller.addBusy
                        onClicked: root.controller.moveCollection(
                            Number(root.controller.selectedCollectionId), 1)
                    }
                    Shared.AppButton {
                        text: qsTr("Delete")
                        quiet: true
                        visible: root.controller && root.controller.selectedCollectionId > 0
                        enabled: root.controller && !root.controller.busy
                                 && !root.controller.addBusy
                        onClicked: {
                            root.pendingDeleteCollectionId =
                                Number(root.controller.selectedCollectionId)
                            root.controller.clearMessages()
                            deleteCollectionDialog.open()
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Shared.AppTextField {
                        id: storeUrlField
                        Layout.fillWidth: true
                        placeholderText: qsTr("Add directly from a Steam Store URL")
                        accessibleLabel: qsTr("Steam Store URL for a game to add to this collection")
                        inputMethodHints: Qt.ImhUrlCharactersOnly
                        enabled: root.controller && root.controller.snapshotLoaded
                                 && root.controller.selectedCollectionId > 0
                                 && !root.controller.busy && !root.controller.addBusy
                        onAccepted: if (root.controller) root.controller.addGame(text)
                    }
                    Shared.AppButton {
                        text: root.controller && root.controller.addBusy
                              ? qsTr("Adding…") : qsTr("Add game")
                        enabled: root.controller && root.controller.selectedCollectionId > 0
                                 && root.controller.snapshotLoaded
                                 && !root.controller.busy && !root.controller.addBusy
                        onClicked: if (root.controller) root.controller.addGame(storeUrlField.text)
                    }
                    Shared.AppComboBox {
                        id: savedGamePicker
                        Layout.preferredWidth: 240
                        model: root.controller && root.controller.selectedCollectionId > 0
                               ? root.controller.availableCatalogGames(
                                     Number(root.controller.selectedCollectionId)) : []
                        textRole: "name"
                        valueRole: "appId"
                        accessibleLabel: qsTr("Existing Steam catalog game")
                        enabled: root.controller && root.controller.selectedCollectionId > 0
                                 && !root.controller.busy && !root.controller.addBusy
                        displayText: count > 0 ? currentText : qsTr("Existing games")
                    }
                    Shared.AppButton {
                        text: qsTr("Add saved")
                        quiet: true
                        enabled: root.controller && savedGamePicker.count > 0
                                 && root.controller.selectedCollectionId > 0
                                 && !root.controller.busy && !root.controller.addBusy
                        onClicked: root.controller.addExistingGame(
                            Number(savedGamePicker.currentValue),
                            Number(root.controller.selectedCollectionId))
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.controller && root.controller.addError.length > 0
                    text: root.controller ? root.controller.addError : ""
                    color: ThemeModule.Theme.error
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.controller && root.controller.errorMessage.length > 0
                    text: root.controller ? root.controller.errorMessage : ""
                    color: ThemeModule.Theme.error
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Shared.AppTextField {
                id: searchField
                Layout.fillWidth: true
                placeholderText: qsTr("Search games, comments, or collections")
                accessibleLabel: qsTr("Search games in collections")
                onTextChanged: if (root.controller) root.controller.setSearchText(text)
            }
            Shared.AppComboBox {
                id: sortBox
                Layout.preferredWidth: 180
                model: [
                    { label: qsTr("Name A–Z"), value: "name_asc" },
                    { label: qsTr("Name Z–A"), value: "name_desc" },
                    { label: qsTr("Recently added"), value: "recently_added" }
                ]
                textRole: "label"
                valueRole: "value"
                accessibleLabel: qsTr("Sort collection games")
                Component.onCompleted: {
                    for (let i = 0; i < count; ++i) {
                        if (valueAt(i) === root.sortSelection) {
                            currentIndex = i
                            break
                        }
                    }
                }
                onActivated: function(index) {
                    root.sortSelection = currentValue
                    if (root.controller) root.controller.setSortOrder(currentValue)
                }
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 7
            Shared.FilterChip {
                text: qsTr("All statuses")
                selected: root.statusSelection === "all"
                onClicked: {
                    root.statusSelection = "all"
                    if (root.controller) root.controller.setStatusFilter("all")
                }
            }
            Repeater {
                model: [
                    { label: qsTr("Backlog"), value: "backlog" },
                    { label: qsTr("Playing"), value: "playing" },
                    { label: qsTr("Completed"), value: "completed" },
                    { label: qsTr("Paused"), value: "paused" },
                    { label: qsTr("Dropped"), value: "dropped" }
                ]
                delegate: Shared.FilterChip {
                    required property var modelData
                    text: modelData.label
                    selected: root.statusSelection === modelData.value
                    onClicked: {
                        root.statusSelection = modelData.value
                        if (root.controller)
                            root.controller.setStatusFilter(modelData.value)
                    }
                }
            }
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller && root.controller.loading
                     && !root.controller.snapshotLoaded
            state: "loading"
            title: qsTr("Loading saved collections")
        }
        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller && !root.controller.loading
                     && !root.controller.snapshotLoaded
                     && root.controller.errorMessage.length > 0
            state: "error"
            title: qsTr("Collections could not be loaded")
            message: root.controller ? root.controller.errorMessage : ""
        }
        Shared.AppButton {
            Layout.alignment: Qt.AlignHCenter
            visible: root.controller && !root.controller.loading
                     && !root.controller.snapshotLoaded
                     && root.controller.errorMessage.length > 0
            text: qsTr("Try again")
            quiet: true
            onClicked: if (root.controller) root.controller.retryLoad()
        }
        Shared.StatePanel {
            Layout.fillWidth: true
            visible: root.controller && root.controller.snapshotLoaded
                     && root.controller.games.length === 0
            state: "empty"
            title: root.controller && root.controller.selectedCollectionId > 0
                   ? qsTr("This collection is empty") : qsTr("No games in collections yet")
            message: root.controller && root.controller.selectedCollectionId > 0
                     ? qsTr("Add a game by Steam Store URL or choose one already in the local catalog.")
                     : qsTr("Create a collection, then add Steam games by Store URL.")
        }

        Flow {
            id: collectionGames
            Layout.fillWidth: true
            spacing: 12
            visible: root.controller && root.controller.games.length > 0
            Repeater {
                model: root.controller ? root.controller.games : []
                delegate: Shared.AppCard {
                    id: gameCard
                    required property var modelData
                    width: Math.max(184, Math.min(252, Math.floor((root.width - 48) / 4)))
                    padding: 12
                    property var game: modelData

                    Column {
                        width: parent.width
                        spacing: 8
                        Item {
                            width: parent.width
                            height: 168
                            Rectangle {
                                anchors.fill: parent
                                radius: ThemeModule.Theme.controlRadius
                                color: ThemeModule.Theme.insetSurface
                                visible: gameArtwork.status !== Image.Ready
                                Text {
                                    anchors.centerIn: parent
                                    text: String(gameCard.game.name || "?").length > 0
                                          ? String(gameCard.game.name).charAt(0).toUpperCase() : "?"
                                    color: ThemeModule.Theme.textSecondary
                                    font.pixelSize: 42
                                    font.weight: Font.DemiBold
                                }
                            }
                            Image {
                                id: gameArtwork
                                anchors.fill: parent
                                source: String(gameCard.game.artworkSource || "")
                                asynchronous: true
                                cache: false
                                fillMode: Image.PreserveAspectCrop
                                visible: status === Image.Ready
                                clip: true
                            }
                            Shared.AppButton {
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 6
                                text: "⋯"
                                quiet: true
                                Accessible.name: qsTr("Actions for %1")
                                                    .arg(gameCard.game.name)
                                onClicked: {
                                    gameActionMenu.game = gameCard.game
                                    gameActionMenu.popup()
                                }
                            }
                        }
                        Label {
                            width: parent.width
                            text: String(gameCard.game.name || "")
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Label {
                            width: parent.width
                            visible: root.controller.selectedCollectionId > 0
                            text: root.statusLabel(gameCard.game.status)
                                  + (Number(gameCard.game.rating || 0) > 0
                                     ? "  ·  " + root.ratingStars(gameCard.game.rating) : "")
                            color: ThemeModule.Theme.accentCyan
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        Label {
                            width: parent.width
                            visible: root.controller.selectedCollectionId === 0
                            text: (gameCard.game.memberships || []).map(function(membership) {
                                return String(membership.collectionName)
                                    + (membership.status ? " · " + root.statusLabel(membership.status) : "")
                            }).join("  /  ")
                            color: ThemeModule.Theme.textSecondary
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                        Label {
                            width: parent.width
                            visible: root.controller.selectedCollectionId > 0
                                     && String(gameCard.game.comment || "").length > 0
                            text: String(gameCard.game.comment || "")
                            color: ThemeModule.Theme.textSecondary
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        Shared.NotificationToast {
            Layout.alignment: Qt.AlignHCenter
            message: root.controller ? root.controller.notification : ""
            onDismissed: if (root.controller) root.controller.clearNotification()
        }
    }

    Menu {
        id: gameActionMenu
        property var game: ({})
        MenuItem {
            text: qsTr("Open Steam Store")
            onTriggered: if (root.controller)
                root.controller.openStore(Number(gameActionMenu.game.appId))
        }
        MenuItem {
            text: qsTr("Edit personal details")
            visible: root.controller && root.controller.selectedCollectionId > 0
            onTriggered: root.openGameEditor(gameActionMenu.game)
        }
        Repeater {
            model: root.controller && root.controller.selectedCollectionId === 0
                   && gameActionMenu.game && gameActionMenu.game.memberships
                   ? gameActionMenu.game.memberships : []
            delegate: MenuItem {
                required property var modelData
                text: qsTr("Edit details in %1").arg(modelData.collectionName)
                onTriggered: root.openGameEditor(modelData)
            }
        }
        MenuSeparator { visible: root.controller && root.controller.collections.length > 0 }
        Repeater {
            model: root.transferActions(gameActionMenu.game)
            delegate: MenuItem {
                required property var modelData
                text: qsTr("Copy from %1 to %2")
                    .arg(modelData.sourceCollectionName)
                    .arg(modelData.targetCollectionName)
                onTriggered: if (root.controller)
                    root.controller.copyGame(Number(modelData.sourceCollectionId),
                                             Number(modelData.targetCollectionId),
                                             Number(modelData.appId))
            }
        }
        Repeater {
            model: root.transferActions(gameActionMenu.game)
            delegate: MenuItem {
                required property var modelData
                text: qsTr("Move from %1 to %2")
                    .arg(modelData.sourceCollectionName)
                    .arg(modelData.targetCollectionName)
                onTriggered: if (root.controller)
                    root.controller.moveGame(Number(modelData.sourceCollectionId),
                                             Number(modelData.targetCollectionId),
                                             Number(modelData.appId))
            }
        }
        MenuSeparator {
            visible: gameActionMenu.game && gameActionMenu.game.memberships
                     && gameActionMenu.game.memberships.length > 0
        }
        Repeater {
            model: gameActionMenu.game && gameActionMenu.game.memberships
                   ? gameActionMenu.game.memberships : []
            delegate: MenuItem {
                required property var modelData
                text: qsTr("Remove from %1").arg(modelData.collectionName)
                onTriggered: if (root.controller)
                    root.controller.removeGame(Number(modelData.collectionId),
                                               Number(gameActionMenu.game.appId))
            }
        }
    }

    Dialog {
        id: collectionEditor
        modal: true
        focus: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(520, parent ? parent.width - 32 : 480)
        padding: 18
        title: root.selectedEditCollectionId > 0
               ? qsTr("Edit collection") : qsTr("Create collection")
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: ThemeModule.Theme.raisedSurface
            border.color: ThemeModule.Theme.strongBorder
            radius: ThemeModule.Theme.panelRadius
        }
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                Layout.fillWidth: true
                text: qsTr("Collection names are unique. Colors use #RRGGBB; icons are optional.")
                color: ThemeModule.Theme.textSecondary
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: root.controller && root.controller.errorMessage.length > 0
                text: root.controller ? root.controller.errorMessage : ""
                color: ThemeModule.Theme.error
                wrapMode: Text.Wrap
            }
            Shared.AppTextField {
                id: collectionNameField
                Layout.fillWidth: true
                placeholderText: qsTr("Name")
                accessibleLabel: qsTr("Collection name")
                maximumLength: 80
            }
            Shared.AppTextField {
                id: collectionDescriptionField
                Layout.fillWidth: true
                placeholderText: qsTr("Description (optional)")
                accessibleLabel: qsTr("Collection description")
                maximumLength: 2000
            }
            RowLayout {
                Layout.fillWidth: true
                Shared.AppTextField {
                    id: collectionColorField
                    Layout.fillWidth: true
                    placeholderText: qsTr("#RRGGBB")
                    accessibleLabel: qsTr("Collection color")
                    maximumLength: 7
                }
                Shared.AppTextField {
                    id: collectionIconField
                    Layout.preferredWidth: 110
                    placeholderText: qsTr("Icon")
                    accessibleLabel: qsTr("Collection icon")
                    maximumLength: 16
                }
            }
        }
        footer: DialogButtonBox {
            alignment: Qt.AlignRight
            Shared.AppButton {
                text: qsTr("Cancel")
                quiet: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Shared.AppButton {
                text: root.selectedEditCollectionId > 0
                      ? qsTr("Save") : qsTr("Create")
                enabled: root.controller && !root.controller.busy
                         && !root.controller.addBusy
                         && collectionNameField.text.trim().length > 0
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                onClicked: root.saveCollection()
            }
        }
    }

    Dialog {
        id: deleteCollectionDialog
        modal: true
        focus: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(500, parent ? parent.width - 32 : 460)
        padding: 18
        title: qsTr("Delete collection?")
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: ThemeModule.Theme.raisedSurface
            border.color: ThemeModule.Theme.strongBorder
            radius: ThemeModule.Theme.panelRadius
        }
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                Layout.fillWidth: true
                text: qsTr("This removes only the collection and its memberships. It will not delete games from Steam or remove price tracking, Release Radar, or Game Updates data.")
                color: ThemeModule.Theme.textSecondary
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: root.controller && root.controller.errorMessage.length > 0
                text: root.controller ? root.controller.errorMessage : ""
                color: ThemeModule.Theme.error
                wrapMode: Text.Wrap
            }
        }
        footer: DialogButtonBox {
            alignment: Qt.AlignRight
            Shared.AppButton {
                text: qsTr("Cancel")
                quiet: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Shared.AppButton {
                text: qsTr("Delete collection")
                enabled: root.controller && !root.controller.busy
                         && !root.controller.addBusy
                DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
                onClicked: {
                    if (root.controller)
                        root.controller.deleteCollection(root.pendingDeleteCollectionId)
                }
            }
        }
    }

    Dialog {
        id: gameEditor
        modal: true
        focus: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(520, parent ? parent.width - 32 : 480)
        padding: 18
        title: qsTr("Personal game details")
        onOpened: {
            root.selectStatusIndex()
            root.selectRatingIndex()
        }
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: ThemeModule.Theme.raisedSurface
            border.color: ThemeModule.Theme.strongBorder
            radius: ThemeModule.Theme.panelRadius
        }
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                Layout.fillWidth: true
                text: String(root.editGame.name || "")
                color: ThemeModule.Theme.textPrimary
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: root.controller && root.controller.errorMessage.length > 0
                text: root.controller ? root.controller.errorMessage : ""
                color: ThemeModule.Theme.error
                wrapMode: Text.Wrap
            }
            Shared.AppComboBox {
                id: gameStatusBox
                Layout.fillWidth: true
                model: [
                    { label: qsTr("No status"), value: "" },
                    { label: qsTr("Backlog"), value: "backlog" },
                    { label: qsTr("Playing"), value: "playing" },
                    { label: qsTr("Completed"), value: "completed" },
                    { label: qsTr("Paused"), value: "paused" },
                    { label: qsTr("Dropped"), value: "dropped" }
                ]
                textRole: "label"
                valueRole: "value"
                accessibleLabel: qsTr("Personal game status")
                Component.onCompleted: root.selectStatusIndex()
                onActivated: root.editStatus = currentValue
            }
            Shared.AppComboBox {
                id: gameRatingBox
                Layout.fillWidth: true
                model: [
                    { label: qsTr("No rating"), value: 0 },
                    { label: "★", value: 1 },
                    { label: "★★", value: 2 },
                    { label: "★★★", value: 3 },
                    { label: "★★★★", value: 4 },
                    { label: "★★★★★", value: 5 }
                ]
                textRole: "label"
                valueRole: "value"
                accessibleLabel: qsTr("Personal rating")
                Component.onCompleted: root.selectRatingIndex()
                onActivated: root.editRating = Number(currentValue)
            }
            TextArea {
                id: gameCommentField
                Layout.fillWidth: true
                Layout.preferredHeight: 100
                placeholderText: qsTr("Personal comment (optional)")
                onTextChanged: {
                    if (text.length > 4000)
                        text = text.slice(0, 4000)
                }
                wrapMode: TextEdit.Wrap
                color: ThemeModule.Theme.textPrimary
                placeholderTextColor: ThemeModule.Theme.textSecondary
                selectionColor: ThemeModule.Theme.accentCyan
                background: Rectangle {
                    color: ThemeModule.Theme.insetSurface
                    border.color: ThemeModule.Theme.borderColor
                    radius: ThemeModule.Theme.controlRadius
                }
            }
        }
        footer: DialogButtonBox {
            alignment: Qt.AlignRight
            Shared.AppButton {
                text: qsTr("Cancel")
                quiet: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Shared.AppButton {
                text: qsTr("Save")
                enabled: root.controller && !root.controller.busy
                         && !root.controller.addBusy
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                onClicked: root.saveGameDetails()
            }
        }
    }

    function selectStatusIndex() {
        if (!gameStatusBox) return
        for (let i = 0; i < gameStatusBox.count; ++i) {
            if (gameStatusBox.valueAt(i) === root.editStatus) {
                gameStatusBox.currentIndex = i
                return
            }
        }
    }

    function selectRatingIndex() {
        if (!gameRatingBox) return
        for (let i = 0; i < gameRatingBox.count; ++i) {
            if (Number(gameRatingBox.valueAt(i)) === root.editRating) {
                gameRatingBox.currentIndex = i
                return
            }
        }
    }

    Connections {
        target: root.controller
        function onStateChanged() {
            if (root.controller && root.controller.snapshotLoaded
                    && root.pendingCollectionRestoreId >= 0) {
                root.controller.selectCollection(root.pendingCollectionRestoreId)
                root.pendingCollectionRestoreId = -1
            }
            if (root.controller) {
                const notification = root.controller.notification
                if (collectionEditor.visible && !root.controller.busy
                        && (notification.startsWith("Created ")
                            || notification === "Collection updated."))
                    collectionEditor.close()
                if (deleteCollectionDialog.visible && !root.controller.busy
                        && notification.startsWith("Collection deleted."))
                    deleteCollectionDialog.close()
                if (gameEditor.visible && !root.controller.busy
                        && notification === "Game details saved.")
                    gameEditor.close()
                if (notification.length === 0)
                    root.lastHandledNotification = ""
                else if (notification !== root.lastHandledNotification
                         && notification.startsWith("Added ")) {
                    storeUrlField.text = ""
                    root.lastHandledNotification = notification
                }
            }
        }
    }
}

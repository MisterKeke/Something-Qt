import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "../components" as Shared
import "../dialogs" as Dialogs
import Something.App as ThemeModule

Item {
    id: root
    property var controller
    implicitHeight: 600

    property int selectedId: 0
    property string selectedName: ""
    property bool selectedHasCustomIcon: false
    property int relocateId: 0
    property int iconId: 0
    property string selectedFilter: "all"
    property var categoryOptions: []
    property int editingCategoryId: 0
    property int categoryToDeleteId: 0
    property string categoryToDeleteName: ""
    property bool categoryDeletePending: false
    property bool deleteWasSelected: false

    function rebuildCategoryOptions() {
        const options = [{ id: 0, label: qsTr("Uncategorized") }]
        for (let i = 0; i < controller.categories.count; ++i) {
            const category = controller.categories.rowAt(i)
            options.push({ id: category.id, label: category.name,
                           applicationCount: category.applicationCount })
        }
        categoryOptions = options
    }

    function categoryIndexFor(id) {
        const wanted = id === null || typeof id === "undefined" ? "0" : String(id)
        for (let i = 0; i < categoryOptions.length; ++i)
            if (String(categoryOptions[i].id) === wanted) return i
        return 0
    }

    function currentCategoryTitle() {
        for (let i = 1; i < categoryOptions.length; ++i)
            if (String(categoryOptions[i].id) === selectedFilter)
                return categoryOptions[i].label + " · " + categoryOptions[i].applicationCount
        return qsTr("Application category")
    }

    function selectAllApplications() {
        selectedFilter = "all"
        controller.loadAllApplications()
    }

    function selectUncategorized() {
        selectedFilter = "uncategorized"
        controller.loadUncategorizedApplications()
    }

    function selectCategory(id) {
        selectedFilter = String(id)
        controller.loadApplicationsByCategory(Number(id))
    }

    function assignCategory(applicationId, index) {
        if (index <= 0) controller.removeApplicationCategory(applicationId)
        else controller.assignApplicationCategory(applicationId, Number(categoryOptions[index].id))
    }

    function filterMatches(text) {
        return String(text).toLocaleLowerCase().includes(categorySearch.text.trim().toLocaleLowerCase())
    }

    function matchingCategoryCount() {
        let matches = 0
        for (let i = 0; i < controller.categories.count; ++i)
            if (filterMatches(controller.categories.rowAt(i).name)) ++matches
        return matches
    }

    function openNewCategory() {
        controller.clearMessages()
        editingCategoryId = 0
        categoryNameField.text = ""
        categoryDescriptionField.text = ""
        categoryColorField.text = ""
        categoryIconField.text = ""
        categoryEditor.open()
        categoryNameField.forceActiveFocus()
    }

    function openCategoryEditor(id) {
        let record = {}
        for (let i = 0; i < controller.categories.count; ++i) {
            const candidate = controller.categories.rowAt(i)
            if (Number(candidate.id) === Number(id)) {
                record = candidate
                break
            }
        }
        if (!record.id || Number(record.id) <= 0) return
        controller.clearMessages()
        editingCategoryId = Number(record.id)
        categoryNameField.text = String(record.name || "")
        categoryDescriptionField.text = String(record.description || "")
        categoryColorField.text = String(record.color || "")
        categoryIconField.text = String(record.iconIdentifier || "")
        categoryEditor.open()
        categoryNameField.forceActiveFocus()
    }

    function saveCategory() {
        if (editingCategoryId > 0) {
            controller.updateCategory(editingCategoryId, categoryNameField.text,
                                      categoryDescriptionField.text, categoryColorField.text,
                                      categoryIconField.text)
        } else {
            controller.createCategory(categoryNameField.text, categoryDescriptionField.text,
                                      categoryColorField.text, categoryIconField.text)
        }
    }

    function moveCategory(row, direction) {
        const target = row + direction
        if (target < 0 || target >= controller.categories.count) return
        const orderedIds = []
        for (let i = 0; i < controller.categories.count; ++i)
            orderedIds.push(controller.categories.rowAt(i).id)
        const moved = orderedIds[row]
        orderedIds[row] = orderedIds[target]
        orderedIds[target] = moved
        controller.reorderCategories(orderedIds)
    }

    function requestCategoryDelete(id, name) {
        controller.clearMessages()
        categoryToDeleteId = Number(id)
        categoryToDeleteName = String(name)
        deleteWasSelected = selectedFilter === String(id)
        deleteCategoryDialog.title = qsTr("Delete category?")
        deleteCategoryDialog.open()
    }

    function openCategoryManagement() {
        controller.clearMessages()
        categoryManagementDialog.open()
        controller.loadCategories()
    }

    function confirmCategoryDelete() {
        categoryDeletePending = true
        controller.deleteCategory(categoryToDeleteId)
    }

    onVisibleChanged: if (controller) controller.setPageVisible(visible)
    Component.onDestruction: controller.setPageVisible(false)
    Component.onCompleted: {
        rebuildCategoryOptions()
        controller.setPageVisible(visible)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Shared.AppButton {
                text: qsTr("Add application")
                enabled: root.controller.supported && !root.controller.busy
                onClicked: executablePicker.open()
            }
            Shared.AppButton {
                text: qsTr("Refresh")
                quiet: true
                enabled: !root.controller.busy
                onClicked: {
                    root.controller.reload()
                    root.controller.loadCategories()
                }
            }
            Shared.AppButton {
                text: qsTr("Manage categories")
                quiet: true
                visible: root.width < 760
                enabled: !root.controller.busy
                onClicked: root.openCategoryManagement()
            }
            Item { Layout.fillWidth: true }
            BusyIndicator {
                running: root.visible && (root.controller.loading
                         || root.controller.categoriesLoading || root.controller.busy)
                visible: running
            }
        }

        Shared.StatePanel {
            Layout.fillWidth: true
            visible: !root.controller.supported || root.controller.errorMessage.length > 0
                     || (root.controller.loading && root.controller.applications.count === 0)
            state: !root.controller.supported ? "error"
                   : root.controller.errorMessage.length > 0 ? "error" : "loading"
            title: !root.controller.supported ? qsTr("Application launching unavailable")
                   : root.controller.errorMessage.length > 0 ? qsTr("Action could not be completed")
                   : qsTr("Loading saved applications")
            message: !root.controller.supported
                     ? qsTr("Saved application launching is supported on Windows.")
                     : root.controller.errorMessage
        }

        Shared.AppTextField {
            id: categorySearch
            Layout.fillWidth: true
            visible: root.controller.supported && root.controller.categories.count > 4
            accessibleLabel: qsTr("Filter categories")
            placeholderText: qsTr("Filter categories")
            maximumLength: 120
        }

        Flow {
            id: compactCategoryFilters
            Layout.fillWidth: true
            visible: root.width < 760 && root.controller.supported
            spacing: 6

            Shared.FilterChip {
                width: Math.min(230, compactCategoryFilters.width)
                text: qsTr("All applications · %1").arg(root.controller.totalApplicationsCount)
                selected: root.selectedFilter === "all"
                onClicked: root.selectAllApplications()
            }
            Shared.FilterChip {
                width: Math.min(230, compactCategoryFilters.width)
                text: qsTr("Uncategorized · %1").arg(root.controller.uncategorizedCount)
                selected: root.selectedFilter === "uncategorized"
                onClicked: root.selectUncategorized()
            }
            Repeater {
                model: root.controller.categories
                delegate: Shared.FilterChip {
                    required property int categoryId
                    required property string name
                    required property string color
                    required property int applicationCount
                    width: Math.min(230, compactCategoryFilters.width)
                    visible: root.filterMatches(name)
                    text: name + " · " + applicationCount
                    tint: color.length > 0 ? color : ThemeModule.Theme.accentCyan
                    selected: root.selectedFilter === String(categoryId)
                    onClicked: root.selectCategory(categoryId)
                }
            }
        }

        RowLayout {
            id: applicationsContent
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.controller.supported
                     && !(root.controller.loading && root.controller.applications.count === 0)
            spacing: 12

            Shared.AppCard {
                id: categorySidebar
                Layout.preferredWidth: 226
                Layout.minimumWidth: 190
                Layout.fillHeight: true
                visible: root.width >= 760
                padding: 12

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 7
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Categories")
                        color: ThemeModule.Theme.textPrimary
                        font.family: ThemeModule.Theme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }
                    Shared.FilterChip {
                        Layout.fillWidth: true
                        text: qsTr("All applications")
                        selected: root.selectedFilter === "all"
                        onClicked: root.selectAllApplications()
                    }
                    Shared.FilterChip {
                        Layout.fillWidth: true
                        text: qsTr("Uncategorized") + " · " + root.controller.uncategorizedCount
                        selected: root.selectedFilter === "uncategorized"
                        onClicked: root.selectUncategorized()
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: ThemeModule.Theme.borderColor
                    }
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: parent.width
                            spacing: 6
                            Repeater {
                                model: root.controller.categories
                                delegate: Shared.FilterChip {
                                    required property int categoryId
                                    required property string name
                                    required property string color
                                    required property int applicationCount
                                    Layout.fillWidth: true
                                    visible: root.filterMatches(name)
                                    text: name + " · " + applicationCount
                                    tint: color.length > 0 ? color : ThemeModule.Theme.accentCyan
                                    selected: root.selectedFilter === String(categoryId)
                                    onClicked: root.selectCategory(categoryId)
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: !root.controller.categoriesLoading
                                         && (root.controller.categories.count === 0
                                             || root.matchingCategoryCount() === 0)
                                text: root.controller.categories.count === 0
                                      ? qsTr("Create a category to organize saved applications.")
                                      : qsTr("No categories match this filter.")
                                color: ThemeModule.Theme.textSecondary
                                wrapMode: Text.Wrap
                                textFormat: Text.PlainText
                            }
                        }
                    }
                    Shared.AppButton {
                        Layout.fillWidth: true
                        text: qsTr("Manage categories")
                        quiet: true
                        enabled: !root.controller.busy
                        onClicked: root.openCategoryManagement()
                    }
                }
            }

            ColumnLayout {
                id: applicationPane
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                spacing: 9

                Label {
                    Layout.fillWidth: true
                    text: root.selectedFilter === "all" ? qsTr("All applications · %1").arg(root.controller.totalApplicationsCount)
                          : root.selectedFilter === "uncategorized" ? qsTr("Uncategorized · %1").arg(root.controller.uncategorizedCount)
                          : root.currentCategoryTitle()
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                Shared.StatePanel {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.controller.applications.count === 0 && !root.controller.loading
                             && root.controller.errorMessage.length === 0
                    state: "empty"
                    title: root.selectedFilter === "all" && root.controller.totalApplicationsCount === 0
                           ? qsTr("No saved applications") : qsTr("No applications in this category")
                    message: root.selectedFilter === "all" && root.controller.totalApplicationsCount === 0
                             ? qsTr("Add a Windows executable to keep a launch shortcut here.")
                             : qsTr("Applications assigned to this category will appear here.")
                }

                ListView {
                    id: applicationList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.controller.applications.count > 0
                    clip: true
                    spacing: 9
                    model: root.controller.applications
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Shared.AppCard {
                required property int applicationId
                required property string displayName
                required property string executablePath
                required property string iconFilename
                required property string iconSource
                required property string nativeIconDataUrl
                required property bool available
                required property var categoryId

                width: applicationList.width
                implicitHeight: row.implicitHeight + 28

                RowLayout {
                    id: row
                    anchors.fill: parent
                    spacing: 12

                    Rectangle {
                        Layout.preferredWidth: 48
                        Layout.preferredHeight: 48
                        radius: ThemeModule.Theme.panelRadius
                        color: ThemeModule.Theme.raisedSurface
                        Image {
                            id: applicationIcon
                            anchors.fill: parent
                            anchors.margins: 4
                            visible: source.toString().length > 0
                            readonly property string requestedIconSource: iconSource
                            property bool customIconFailed: false
                            source: customIconFailed ? nativeIconDataUrl : requestedIconSource
                            sourceSize: Qt.size(80, 80)
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            onStatusChanged: {
                                if (status === Image.Error && requestedIconSource.startsWith("image://application-icons/"))
                                    customIconFailed = true
                            }
                            onRequestedIconSourceChanged: customIconFailed = false
                        }
                        Label {
                            anchors.centerIn: parent
                            visible: (applicationIcon.customIconFailed
                                      ? nativeIconDataUrl : iconSource).length === 0
                            text: displayName.length ? Array.from(displayName)[0].toUpperCase() : "?"
                            color: ThemeModule.Theme.focusHighlight
                            font.pixelSize: 23
                            font.weight: Font.Bold
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            Layout.fillWidth: true
                            text: displayName
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: executablePath
                            color: ThemeModule.Theme.textSecondary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 11
                            textFormat: Text.PlainText
                            elide: Text.ElideMiddle
                            Accessible.name: qsTr("Executable path")
                        }
                        Shared.StatusBadge {
                            text: available ? qsTr("Available") : qsTr("Location missing")
                            badgeColor: available ? ThemeModule.Theme.success : ThemeModule.Theme.warning
                        }
                        Shared.AppComboBox {
                            Layout.preferredWidth: Math.min(240, applicationPane.width * 0.55)
                            Layout.minimumWidth: 150
                            model: root.categoryOptions
                            textRole: "label"
                            currentIndex: root.categoryIndexFor(categoryId)
                            enabled: !root.controller.busy
                            accessibleLabel: qsTr("Category for %1").arg(displayName)
                            onActivated: root.assignCategory(applicationId, currentIndex)
                        }
                    }

                    Shared.AppButton {
                        text: qsTr("Launch")
                        enabled: available && root.controller.supported && !root.controller.busy
                        Accessible.name: qsTr("Launch %1").arg(displayName)
                        onClicked: root.controller.launchApplication(applicationId)
                    }
                    Shared.AppButton {
                        text: qsTr("Manage")
                        quiet: true
                        enabled: !root.controller.busy
                        Accessible.name: qsTr("Edit %1").arg(displayName)
                        onClicked: {
                            root.selectedId = applicationId
                            root.selectedName = displayName
                            root.selectedHasCustomIcon = iconFilename.length > 0
                            editDialog.open()
                        }
                    }
                }
            }
            }
        }
        }

        Label {
            Layout.fillWidth: true
            visible: root.controller.notification.length > 0
            text: root.controller.notification
            color: ThemeModule.Theme.success
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            Accessible.name: text
        }
    }

    FileDialog {
        id: executablePicker
        title: qsTr("Select an application")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Windows applications (*.exe)")]
        onAccepted: root.controller.addFromFile(selectedFile)
    }

    FileDialog {
        id: relocationPicker
        title: qsTr("Locate the application executable")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Windows applications (*.exe)")]
        onAccepted: root.controller.relocateFromFile(root.relocateId, selectedFile)
    }

    FileDialog {
        id: iconPicker
        title: qsTr("Choose an application icon")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Images (*.ico *.jpg *.jpeg *.png *.webp)")]
        onAccepted: root.controller.importIconFile(root.iconId, selectedFile)
    }

    Dialog {
        id: renameDialog
        modal: true
        focus: true
        title: qsTr("Rename application")
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, parent ? parent.width - 40 : 420)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: ThemeModule.Theme.raisedSurface; radius: ThemeModule.Theme.panelRadius; border.color: ThemeModule.Theme.strongBorder }
        contentItem: ColumnLayout {
            spacing: 12
            Label {
                Layout.fillWidth: true
                text: qsTr("Choose a display name. It may contain up to 120 characters.")
                color: ThemeModule.Theme.textSecondary
                wrapMode: Text.Wrap
            }
            Shared.AppTextField {
                id: nameField
                Layout.fillWidth: true
                accessibleLabel: qsTr("Application display name")
                placeholderText: qsTr("Application name")
                // TextField counts UTF-16 code units; the service enforces the
                // actual 120 Unicode-codepoint rule.
                maximumLength: 240
                onAccepted: root.controller.renameApplication(root.selectedId, text)
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
                id: renameButton
                text: qsTr("Save")
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: root.controller.renameApplication(root.selectedId, nameField.text)
            }
        }
    }

    Dialogs.ConfirmationDialog {
        id: deleteDialog
        title: qsTr("Remove saved application?")
        confirmText: qsTr("Remove saved entry")
        destructive: true
        message: qsTr("The shortcut and its custom icon will be removed. The executable on disk will remain untouched.")
        onConfirmed: root.controller.removeApplication(root.selectedId)
    }

    Dialogs.ConfirmationDialog {
        id: resetIconDialog
        title: qsTr("Reset application icon?")
        confirmText: qsTr("Reset icon")
        message: qsTr("The imported icon will be removed and the application default will be shown.")
        onConfirmed: root.controller.resetIcon(root.selectedId)
    }

    Dialog {
        id: categoryManagementDialog
        modal: true
        focus: true
        title: qsTr("Manage application categories")
        anchors.centerIn: Overlay.overlay
        width: Math.min(620, parent ? parent.width - 32 : 580)
        height: Math.min(560, parent ? parent.height - 32 : 520)
        padding: 16
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: ThemeModule.Theme.raisedSurface
            radius: ThemeModule.Theme.panelRadius
            border.color: ThemeModule.Theme.strongBorder
        }
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                Layout.fillWidth: true
                text: qsTr("Create and organize custom categories. Uncategorized is a virtual group and cannot be deleted.")
                color: ThemeModule.Theme.textSecondary
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Custom categories")
                    color: ThemeModule.Theme.textPrimary
                    font.family: ThemeModule.Theme.fontFamily
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Shared.AppButton {
                    text: qsTr("Create category")
                    enabled: !root.controller.busy
                    onClicked: root.openNewCategory()
                }
            }
            Label {
                Layout.fillWidth: true
                visible: root.controller.errorMessage.length > 0
                text: root.controller.errorMessage
                color: ThemeModule.Theme.error
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
            Shared.StatePanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.controller.categories.count === 0
                state: root.controller.categoriesLoading ? "loading" : "empty"
                title: root.controller.categoriesLoading
                       ? qsTr("Loading categories") : qsTr("No custom categories")
                message: root.controller.categoriesLoading ? ""
                         : qsTr("Create a category to start organizing saved applications.")
            }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.controller.categories.count > 0
                clip: true
                spacing: 7
                model: root.controller.categories
                boundsBehavior: Flickable.StopAtBounds
                delegate: Shared.AppCard {
                    id: categoryEntry
                    required property int categoryId
                    required property string name
                    required property string color
                    required property int applicationCount
                    required property int index
                    width: ListView.view.width
                    implicitHeight: categoryRow.implicitHeight + 18
                    RowLayout {
                        id: categoryRow
                        anchors.fill: parent
                        spacing: 8
                        Rectangle {
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 18
                            radius: ThemeModule.Theme.controlRadius
                            color: categoryEntry.color.length > 0
                                   ? categoryEntry.color : ThemeModule.Theme.accentCyan
                            border.color: ThemeModule.Theme.borderColor
                        }
                        Label {
                            Layout.fillWidth: true
                            text: name + " · " + applicationCount
                            color: ThemeModule.Theme.textPrimary
                            font.family: ThemeModule.Theme.fontFamily
                            font.pixelSize: 13
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                        }
                        Shared.AppButton {
                            text: "↑"
                            quiet: true
                            Layout.preferredWidth: 40
                            Layout.minimumWidth: 40
                            enabled: !root.controller.busy && index > 0
                            Accessible.name: qsTr("Move %1 up").arg(name)
                            onClicked: root.moveCategory(index, -1)
                        }
                        Shared.AppButton {
                            text: "↓"
                            quiet: true
                            Layout.preferredWidth: 40
                            Layout.minimumWidth: 40
                            enabled: !root.controller.busy && index < root.controller.categories.count - 1
                            Accessible.name: qsTr("Move %1 down").arg(name)
                            onClicked: root.moveCategory(index, 1)
                        }
                        Shared.AppButton {
                            text: qsTr("Edit")
                            quiet: true
                            Layout.preferredWidth: 68
                            Layout.minimumWidth: 68
                            enabled: !root.controller.busy
                            Accessible.name: qsTr("Edit %1").arg(name)
                            onClicked: root.openCategoryEditor(categoryId)
                        }
                        Shared.AppButton {
                            text: qsTr("Delete")
                            destructive: true
                            Layout.preferredWidth: 74
                            Layout.minimumWidth: 74
                            enabled: !root.controller.busy
                            Accessible.name: qsTr("Delete %1").arg(name)
                            onClicked: root.requestCategoryDelete(categoryId, name)
                        }
                    }
                }
            }
        }
        footer: DialogButtonBox {
            alignment: Qt.AlignRight
            Shared.AppButton {
                text: qsTr("Close")
                quiet: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
    }

    Dialog {
        id: categoryEditor
        modal: true
        focus: true
        title: root.editingCategoryId > 0 ? qsTr("Edit category") : qsTr("Create category")
        anchors.centerIn: Overlay.overlay
        width: Math.min(500, parent ? parent.width - 32 : 460)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: ThemeModule.Theme.raisedSurface
            radius: ThemeModule.Theme.panelRadius
            border.color: ThemeModule.Theme.strongBorder
        }
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                Layout.fillWidth: true
                text: qsTr("Category names are unique ignoring case and surrounding spaces.")
                color: ThemeModule.Theme.textSecondary
                wrapMode: Text.Wrap
            }
            Shared.AppTextField {
                id: categoryNameField
                Layout.fillWidth: true
                accessibleLabel: qsTr("Category name")
                placeholderText: qsTr("Category name")
                maximumLength: 160
                onAccepted: root.saveCategory()
            }
            Shared.AppTextField {
                id: categoryDescriptionField
                Layout.fillWidth: true
                accessibleLabel: qsTr("Category description")
                placeholderText: qsTr("Description (optional)")
                maximumLength: 600
            }
            Shared.AppTextField {
                id: categoryColorField
                Layout.fillWidth: true
                accessibleLabel: qsTr("Category color")
                placeholderText: qsTr("Color, e.g. #4477CC (optional)")
                maximumLength: 16
            }
            Shared.AppTextField {
                id: categoryIconField
                Layout.fillWidth: true
                accessibleLabel: qsTr("Category icon identifier")
                placeholderText: qsTr("Icon identifier (optional)")
                maximumLength: 160
            }
            Label {
                Layout.fillWidth: true
                visible: root.controller.errorMessage.length > 0
                text: root.controller.errorMessage
                color: ThemeModule.Theme.error
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
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
                enabled: !root.controller.busy
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: root.saveCategory()
            }
        }
    }

    Dialogs.ConfirmationDialog {
        id: deleteCategoryDialog
        title: qsTr("Delete category?")
        confirmText: qsTr("Delete category")
        destructive: true
        message: qsTr("Deleting ‘%1’ keeps its applications and moves them to Uncategorized. Applications and their Setup memberships will not be deleted.").arg(root.categoryToDeleteName)
        onConfirmed: root.confirmCategoryDelete()
    }

    Dialog {
        id: editDialog
        modal: true
        focus: true
        title: qsTr("Application actions")
        anchors.centerIn: Overlay.overlay
        width: Math.min(450, parent ? parent.width - 40 : 420)
        padding: 18
        background: Rectangle { color: ThemeModule.Theme.raisedSurface; radius: ThemeModule.Theme.panelRadius; border.color: ThemeModule.Theme.strongBorder }
        contentItem: ColumnLayout {
            spacing: 8
            Shared.AppButton {
                Layout.fillWidth: true
                text: qsTr("Rename")
                onClicked: {
                    editDialog.close()
                    nameField.text = root.selectedName
                    renameDialog.open()
                }
            }
            Shared.AppButton {
                Layout.fillWidth: true
                text: qsTr("Relocate executable")
                quiet: true
                enabled: root.controller.supported
                onClicked: { editDialog.close(); root.relocateId = root.selectedId; relocationPicker.open() }
            }
            Shared.AppButton {
                Layout.fillWidth: true
                text: qsTr("Import custom icon")
                quiet: true
                onClicked: { editDialog.close(); root.iconId = root.selectedId; iconPicker.open() }
            }
            Shared.AppButton {
                Layout.fillWidth: true
                text: qsTr("Reset custom icon")
                quiet: true
                visible: root.selectedHasCustomIcon
                onClicked: { editDialog.close(); resetIconDialog.open() }
            }
            Shared.AppButton {
                Layout.fillWidth: true
                text: qsTr("Remove saved entry")
                destructive: true
                onClicked: { editDialog.close(); deleteDialog.open() }
            }
            Shared.AppButton {
                Layout.fillWidth: true
                text: qsTr("Close")
                quiet: true
                onClicked: editDialog.close()
            }
        }
    }

    Connections {
        target: root.controller
        function onStateChanged() {
            root.rebuildCategoryOptions()
        }
        function onMutationFinished(success) {
            if (success && renameDialog.visible) renameDialog.close()
            if (success && categoryEditor.visible) categoryEditor.close()
            if (root.categoryDeletePending) {
                if (success && root.deleteWasSelected)
                    root.selectedFilter = "uncategorized"
                root.categoryDeletePending = false
                root.deleteWasSelected = false
            }
        }
    }

}

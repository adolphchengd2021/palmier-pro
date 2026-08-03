import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import "."

ApplicationWindow {
    id: window
    property bool shutdownApproved: false
    property bool discardUnsavedChanges: false
    property bool closeAfterSave: false
    property bool closeAfterEdit: false
    property bool closeAfterExport: false
    property string selectedClipId: ""
    property string selectedTrackId: ""
    property string selectedMediaType: ""
    property string selectedDurationText: ""
    property string selectedTrimStartText: ""
    property string selectedTrimEndText: ""
    property string selectedSpeedText: ""
    property string exportRequestMessage: ""
    property bool projectShutdownReady: false
    property bool persistenceShutdownReady: false
    property bool previewShutdownReady: false
    width: AppTheme.windowWidth
    height: AppTheme.windowHeight
    visible: true
    title: qsTr("Palmier Pro — Windows MVP")
    color: AppTheme.windowBackground

    onClosing: function(closeEvent) {
        if (!shutdownApproved) {
            closeEvent.accepted = false
            if (editingCoordinator.busy) {
                closeAfterEdit = true
                return
            }
            if (persistenceCoordinator.saving) {
                closeAfterSave = true
                return
            }
            if (persistenceCoordinator.dirty && !discardUnsavedChanges) {
                unsavedChangesDialog.open()
                return
            }
            if (exportCoordinator.exporting) {
                if (!exportCloseDialog.visible) exportCloseDialog.open()
                return
            }
            persistenceShutdownReady = persistenceCoordinator.requestShutdown(
                discardUnsavedChanges
            )
            if (!persistenceShutdownReady) {
                if (persistenceCoordinator.dirty) unsavedChangesDialog.open()
                return
            }
            projectShutdownReady = projectCoordinator.requestShutdown()
            editingCoordinator.requestShutdown()
            previewShutdownReady = previewCoordinator.requestShutdown()
            shutdownApproved = projectShutdownReady
                && persistenceShutdownReady
                && previewShutdownReady
            closeEvent.accepted = shutdownApproved
        }
    }

    function finishShutdownIfReady() {
        if (projectShutdownReady
                && persistenceShutdownReady
                && previewShutdownReady
                && !shutdownApproved) {
            shutdownApproved = true
            window.close()
        }
    }

    Connections {
        target: editingCoordinator
        function onOperationFinished(succeeded) {
            if (!window.closeAfterEdit) return
            window.closeAfterEdit = false
            window.close()
        }
        function onClipRemoved(clipId) {
            if (window.selectedClipId !== clipId) return
            window.selectedClipId = ""
            window.selectedTrackId = ""
            window.selectedMediaType = ""
            window.selectedDurationText = ""
            window.selectedTrimStartText = ""
            window.selectedTrimEndText = ""
            window.selectedSpeedText = ""
        }
        function onHistoryRestored() {
            window.selectedClipId = ""
            window.selectedTrackId = ""
            window.selectedMediaType = ""
            window.selectedDurationText = ""
            window.selectedTrimStartText = ""
            window.selectedTrimEndText = ""
            window.selectedSpeedText = ""
        }
    }

    Connections {
        target: persistenceCoordinator
        function onShutdownReady() {
            window.persistenceShutdownReady = true
            window.finishShutdownIfReady()
        }
        function onSaveFinished(succeeded) {
            if (!window.closeAfterSave) return
            if (succeeded
                    && !persistenceCoordinator.dirty
                    && persistenceCoordinator.warningCode.length === 0) {
                window.close()
            } else {
                window.closeAfterSave = false
            }
        }
    }

    Connections {
        target: exportCoordinator
        function onRequestRefused(code, message) {
            window.exportRequestMessage = message
        }
        function onExportFinished(succeeded) {
            window.exportRequestMessage = ""
            if (exportCloseDialog.visible) exportCloseDialog.close()
            if (window.closeAfterExport) {
                window.closeAfterExport = false
                window.close()
            }
        }
    }

    Connections {
        target: projectCoordinator
        function onShutdownReady() {
            window.projectShutdownReady = true
            window.finishShutdownIfReady()
        }
        function onProjectCommitted() {
            window.selectedClipId = ""
            window.selectedTrackId = ""
            window.selectedMediaType = ""
            window.selectedDurationText = ""
            window.selectedTrimStartText = ""
            window.selectedTrimEndText = ""
            window.selectedSpeedText = ""
            window.exportRequestMessage = ""
        }
    }

    Connections {
        target: previewCoordinator
        function onShutdownReady() {
            window.previewShutdownReady = true
            window.finishShutdownIfReady()
        }
    }

    FolderDialog {
        id: projectDialog
        title: qsTr("Open Palmier Pro Project")
        onAccepted: projectCoordinator.openFolder(selectedFolder)
    }

    FileDialog {
        id: saveAsDialog
        objectName: "saveAsDialog"
        title: qsTr("Save Palmier Pro Project As")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Palmier Pro Project (*.palmier)")]
        defaultSuffix: "palmier"
        onAccepted: persistenceCoordinator.saveAs(selectedFile)
    }

    FileDialog {
        id: exportDialog
        title: qsTr("Export Timeline")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("MP4 Video (*.mp4)")]
        defaultSuffix: "mp4"
        onAccepted: exportCoordinator.exportTimeline(selectedFile)
    }

    MessageDialog {
        id: exportCloseDialog
        objectName: "exportCloseDialog"
        title: qsTr("Cancel Export?")
        text: qsTr("Cancel the active export and close Palmier Pro?")
        informativeText: qsTr("The unfinished export file will not be kept.")
        buttons: MessageDialog.Yes | MessageDialog.No
        onButtonClicked: function(button, role) {
            if (button !== MessageDialog.Yes) return
            if (exportCoordinator.exporting) {
                window.closeAfterExport = true
                exportCoordinator.cancel()
            } else {
                window.close()
            }
        }
    }

    MessageDialog {
        id: unsavedChangesDialog
        title: qsTr("Unsaved Changes")
        text: qsTr("Save changes before closing?")
        informativeText: qsTr("Unsaved timeline edits will be lost.")
        buttons: MessageDialog.Save | MessageDialog.Discard | MessageDialog.Cancel
        onButtonClicked: function(button, role) {
            if (button === MessageDialog.Save) {
                window.closeAfterSave = true
                persistenceCoordinator.save()
            } else if (button === MessageDialog.Discard) {
                window.discardUnsavedChanges = true
                window.close()
            }
        }
    }

    header: ToolBar {
        Row {
            id: previewTransportControls
            property bool seekAvailable: previewCoordinator.state === "playing"
                || previewCoordinator.state === "paused"
                || previewCoordinator.state === "completed"
            spacing: AppTheme.itemSpacing
            anchors.verticalCenter: parent.verticalCenter
            Button {
                text: qsTr("Open Project…")
                enabled: !persistenceCoordinator.dirty
                    && !persistenceCoordinator.saving
                    && !editingCoordinator.busy
                    && !exportCoordinator.exporting
                onClicked: projectDialog.open()
            }
            Button {
                text: qsTr("Save")
                enabled: persistenceCoordinator.dirty
                    && !projectCoordinator.loading
                    && !persistenceCoordinator.saving
                    && !editingCoordinator.busy
                    && !exportCoordinator.exporting
                onClicked: persistenceCoordinator.save()
            }
            Button {
                objectName: "saveAsButton"
                text: qsTr("Save As…")
                enabled: persistenceCoordinator.hasProject
                    && !projectCoordinator.loading
                    && !persistenceCoordinator.saving
                    && !editingCoordinator.busy
                    && !exportCoordinator.exporting
                onClicked: saveAsDialog.open()
            }
            Button {
                objectName: "cancelSaveButton"
                text: qsTr("Cancel Save")
                visible: persistenceCoordinator.saving
                onClicked: persistenceCoordinator.cancelSave()
            }
            Button {
                objectName: "undoButton"
                text: qsTr("Undo")
                enabled: editingCoordinator.canUndo && !editingCoordinator.busy
                    && !projectCoordinator.loading
                    && !exportCoordinator.exporting
                onClicked: editingCoordinator.undo()
            }
            Button {
                objectName: "redoButton"
                text: qsTr("Redo")
                enabled: editingCoordinator.canRedo && !editingCoordinator.busy
                    && !projectCoordinator.loading
                    && !exportCoordinator.exporting
                onClicked: editingCoordinator.redo()
            }
            Button {
                objectName: "exportTimelineButton"
                text: qsTr("Export Timeline…")
                enabled: exportCoordinator.hasProject
                    && projectCoordinator.presentationReady
                    && !projectCoordinator.loading
                    && !persistenceCoordinator.saving
                    && !editingCoordinator.busy
                    && !exportCoordinator.exporting
                onClicked: exportDialog.open()
            }
            Button {
                objectName: "cancelExportButton"
                text: qsTr("Cancel Export")
                visible: exportCoordinator.exporting
                enabled: exportCoordinator.canCancel
                onClicked: exportCoordinator.cancel()
            }
            Button {
                text: qsTr("Cancel Open")
                visible: projectCoordinator.loading
                onClicked: projectCoordinator.cancelLoading()
            }
            Label {
                text: exportCoordinator.exporting
                    ? exportCoordinator.state === "cancelling"
                        ? qsTr("Cancelling export…")
                        : qsTr("Exporting…")
                    : projectCoordinator.loading
                    ? qsTr("Opening…")
                    : persistenceCoordinator.saving
                        ? qsTr("Saving…")
                        : persistenceCoordinator.dirty
                            ? qsTr("Edited")
                            : qsTr("Saved")
            }
        }

        Row {
            spacing: AppTheme.itemSpacing
            Label {
                text: qsTr("Clip timing")
                color: AppTheme.secondaryText
            }
            TextField {
                id: durationFrames
                objectName: "durationFramesField"
                width: AppTheme.editFieldWidth
                text: window.selectedDurationText
                placeholderText: qsTr("Duration frames")
                validator: RegularExpressionValidator { regularExpression: /[0-9]*/ }
            }
            TextField {
                id: trimStartFrame
                objectName: "trimStartFrameField"
                width: AppTheme.editFieldWidth
                text: window.selectedTrimStartText
                placeholderText: qsTr("Trim start")
                validator: RegularExpressionValidator { regularExpression: /[0-9]*/ }
            }
            TextField {
                id: trimEndFrame
                objectName: "trimEndFrameField"
                width: AppTheme.editFieldWidth
                text: window.selectedTrimEndText
                placeholderText: qsTr("Trim end")
                validator: RegularExpressionValidator { regularExpression: /[0-9]*/ }
            }
            TextField {
                id: clipSpeed
                objectName: "clipSpeedField"
                width: AppTheme.editFieldWidth
                text: window.selectedSpeedText
                placeholderText: qsTr("Speed")
                validator: RegularExpressionValidator {
                    regularExpression: /([0-9]+(\.[0-9]*)?|\.[0-9]+)?/
                }
            }
            Button {
                objectName: "setClipTimingButton"
                text: qsTr("Set Timing")
                enabled: window.selectedClipId.length > 0
                    && (durationFrames.text.length > 0
                        || trimStartFrame.text.length > 0
                        || trimEndFrame.text.length > 0
                        || clipSpeed.text.length > 0)
                    && durationFrames.acceptableInput
                    && trimStartFrame.acceptableInput
                    && trimEndFrame.acceptableInput
                    && clipSpeed.acceptableInput
                    && projectCoordinator.presentationReady
                    && !projectCoordinator.loading
                    && !editingCoordinator.busy
                    && !exportCoordinator.exporting
                onClicked: editingCoordinator.setClipTiming(
                    window.selectedClipId,
                    durationFrames.text,
                    trimStartFrame.text,
                    trimEndFrame.text,
                    clipSpeed.text
                )
            }
        }
    }

    Column {
        anchors.fill: parent
        anchors.margins: AppTheme.pageMargin
        spacing: AppTheme.itemSpacing

        Row {
            spacing: AppTheme.itemSpacing
            Label {
                text: window.selectedClipId.length > 0
                    ? qsTr("Selected: ") + window.selectedClipId
                    : qsTr("Select a clip")
                color: AppTheme.secondaryText
            }
            TextField {
                id: splitFrame
                width: AppTheme.editFieldWidth
                placeholderText: qsTr("Split frame")
                validator: RegularExpressionValidator { regularExpression: /[0-9]+/ }
            }
            Button {
                text: qsTr("Split")
                enabled: window.selectedClipId.length > 0
                    && splitFrame.acceptableInput
                    && projectCoordinator.presentationReady
                    && !projectCoordinator.loading
                    && !editingCoordinator.busy
                    && !exportCoordinator.exporting
                onClicked: editingCoordinator.splitClip(
                    window.selectedClipId,
                    splitFrame.text
                )
            }
            TextField {
                id: moveTrack
                objectName: "moveTrackField"
                width: AppTheme.editFieldWidth
                placeholderText: qsTr("Move track")
                validator: RegularExpressionValidator { regularExpression: /[0-9]*/ }
            }
            TextField {
                id: moveFrame
                objectName: "moveFrameField"
                width: AppTheme.editFieldWidth
                placeholderText: qsTr("Move frame")
                validator: RegularExpressionValidator { regularExpression: /[0-9]*/ }
            }
            Button {
                objectName: "moveClipButton"
                text: qsTr("Move")
                enabled: window.selectedClipId.length > 0
                    && (moveTrack.text.length > 0 || moveFrame.text.length > 0)
                    && moveTrack.acceptableInput
                    && moveFrame.acceptableInput
                    && projectCoordinator.presentationReady
                    && !projectCoordinator.loading
                    && !editingCoordinator.busy
                    && !exportCoordinator.exporting
                onClicked: editingCoordinator.moveClip(
                    window.selectedClipId,
                    moveTrack.text,
                    moveFrame.text
                )
            }
            Button {
                objectName: "removeClipButton"
                text: qsTr("Remove")
                enabled: window.selectedClipId.length > 0
                    && projectCoordinator.presentationReady
                    && !projectCoordinator.loading
                    && !editingCoordinator.busy
                    && !exportCoordinator.exporting
                onClicked: editingCoordinator.removeClip(window.selectedClipId)
            }
            Label {
                visible: editingCoordinator.busy
                text: qsTr("Editing…")
                color: AppTheme.secondaryText
            }
        }

        Label {
            objectName: "editErrorState"
            visible: editingCoordinator.errorMessage.length > 0
            text: editingCoordinator.errorMessage
            color: AppTheme.errorText
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "exportErrorState"
            visible: exportCoordinator.errorMessage.length > 0
                || window.exportRequestMessage.length > 0
            text: window.exportRequestMessage.length > 0
                ? window.exportRequestMessage
                : exportCoordinator.errorMessage
            color: AppTheme.errorText
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "exportWarningState"
            visible: exportCoordinator.warningMessage.length > 0
            text: exportCoordinator.warningMessage
            color: AppTheme.warningText
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "exportCompletedState"
            visible: exportCoordinator.state === "completed"
                || exportCoordinator.state === "completedOutdated"
            text: qsTr("Exported: ") + exportCoordinator.outputPath
            color: AppTheme.primaryText
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "emptyState"
            visible: projectCoordinator.state === "empty"
            text: qsTr("Open a Palmier Pro project folder.")
        }

        Label {
            objectName: "loadedState"
            visible: projectCoordinator.state === "loaded" || projectCoordinator.state === "loadedWithWarnings"
            text: projectCoordinator.model.timelineName + " — "
                + projectCoordinator.model.frameRateText + qsTr(" fps — ")
                + projectCoordinator.model.durationFramesText + qsTr(" frames")
            color: AppTheme.primaryText
        }

        Label {
            objectName: "warningState"
            visible: projectCoordinator.state === "loadedWithWarnings"
            text: projectCoordinator.warningSummary
            color: AppTheme.warningText
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "saveErrorState"
            visible: persistenceCoordinator.errorMessage.length > 0
            text: persistenceCoordinator.errorMessage
            color: AppTheme.errorText
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "saveWarningState"
            visible: persistenceCoordinator.warningMessage.length > 0
            text: persistenceCoordinator.warningMessage
            color: AppTheme.warningText
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "errorState"
            visible: projectCoordinator.errorMessage.length > 0
            text: projectCoordinator.errorMessage
            color: AppTheme.errorText
            wrapMode: Text.Wrap
        }

        WindowContainer {
            objectName: "previewViewport"
            width: parent.width
            height: AppTheme.trackHeight * 3
            window: previewCoordinator.window
        }

        Row {
            spacing: AppTheme.itemSpacing
            Button {
                objectName: "previousPreviewFrameButton"
                text: qsTr("Previous Frame")
                enabled: previewTransportControls.seekAvailable
                    && previewCoordinator.currentFrame > previewCoordinator.minimumFrame
                    && previewCoordinator.maximumFrame >= previewCoordinator.minimumFrame
                onClicked: previewCoordinator.stepFrame(-1)
            }
            Button {
                objectName: "pausePreviewButton"
                text: qsTr("Pause")
                enabled: previewCoordinator.state === "playing"
                onClicked: previewCoordinator.pause()
            }
            Button {
                objectName: "resumePreviewButton"
                text: qsTr("Resume")
                enabled: previewCoordinator.state === "paused"
                onClicked: previewCoordinator.resume()
            }
            Button {
                objectName: "nextPreviewFrameButton"
                text: qsTr("Next Frame")
                enabled: previewTransportControls.seekAvailable
                    && previewCoordinator.currentFrame < previewCoordinator.maximumFrame
                    && previewCoordinator.maximumFrame >= previewCoordinator.minimumFrame
                onClicked: previewCoordinator.stepFrame(1)
            }
            TextField {
                id: previewFrameField
                objectName: "previewFrameField"
                width: AppTheme.trackHeaderWidth
                inputMethodHints: Qt.ImhDigitsOnly
                onAccepted: {
                    const frame = Number(text)
                    if (Number.isSafeInteger(frame))
                        previewCoordinator.seekToFrame(frame)
                }
                Binding {
                    target: previewFrameField
                    property: "text"
                    value: previewCoordinator.currentFrame.toString()
                    when: !previewFrameField.activeFocus
                }
            }
            Button {
                objectName: "seekPreviewFrameButton"
                text: qsTr("Seek")
                enabled: previewTransportControls.seekAvailable
                    && previewCoordinator.maximumFrame >= previewCoordinator.minimumFrame
                onClicked: {
                    const frame = Number(previewFrameField.text)
                    if (Number.isSafeInteger(frame))
                        previewCoordinator.seekToFrame(frame)
                }
            }
            Label {
                objectName: "previewTransportState"
                text: previewCoordinator.state + " — "
                    + previewCoordinator.currentFrame.toString()
                    + qsTr(" frame")
                color: AppTheme.secondaryText
            }
        }

        Label {
            objectName: "previewErrorState"
            visible: previewCoordinator.errorCode.length > 0
            text: qsTr("Preview is unavailable.")
            color: AppTheme.errorText
        }

        ListView {
            id: timelineTracks
            objectName: "timelineTracks"
            width: parent.width
            height: parent.height - y
            model: projectCoordinator.model
            clip: true
            spacing: AppTheme.borderWidth
            delegate: Rectangle {
                id: trackDelegate
                required property int index
                required property string stableId
                required property string trackType
                required property var clipItems
                width: timelineTracks.width
                height: AppTheme.trackHeight
                color: index % 2 === 0
                    ? AppTheme.surfaceBackground
                    : AppTheme.alternateTrackBackground

                Label {
                    width: AppTheme.trackHeaderWidth
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: trackType + "  [" + stableId + "]"
                    color: AppTheme.secondaryText
                    elide: Text.ElideRight
                }

                Item {
                    id: clipLane
                    clip: true
                    anchors.left: parent.left
                    anchors.leftMargin: AppTheme.trackHeaderWidth
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom

                    Repeater {
                        model: clipItems
                        delegate: Rectangle {
                            required property var modelData
                            x: modelData.offsetRatio * clipLane.width
                            y: AppTheme.clipInset
                            width: Math.min(
                                clipLane.width - x,
                                Math.max(
                                    AppTheme.clipMinimumWidth,
                                    modelData.extentRatio * clipLane.width
                                )
                            )
                            height: clipLane.height - AppTheme.clipInset - AppTheme.clipInset
                            color: AppTheme.clipBackground
                            border.width: AppTheme.borderWidth
                            border.color: window.selectedClipId === modelData.stableId
                                ? AppTheme.warningText
                                : AppTheme.clipBorder

                            MouseArea {
                                anchors.fill: parent
                                enabled: projectCoordinator.presentationReady
                                onClicked: {
                                     window.selectedTrackId = trackDelegate.stableId
                                     window.selectedClipId = modelData.stableId
                                     window.selectedMediaType = modelData.mediaType
                                     window.selectedDurationText = modelData.durationFramesText
                                     window.selectedTrimStartText = modelData.trimStartFrameText
                                     window.selectedTrimEndText = modelData.trimEndFrameText
                                     window.selectedSpeedText = modelData.speedText
                                 }
                            }

                            Label {
                                anchors.fill: parent
                                anchors.margins: AppTheme.clipInset
                                text: modelData.mediaType + "  "
                                    + modelData.startFrameText + "+"
                                    + modelData.durationFramesText
                                color: AppTheme.primaryText
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }
            }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import "."

ApplicationWindow {
    id: window
    property bool shutdownApproved: false
    property bool projectShutdownReady: false
    property bool previewShutdownReady: false
    width: AppTheme.windowWidth
    height: AppTheme.windowHeight
    visible: true
    title: qsTr("Palmier Pro — Read-only project shell")
    color: AppTheme.windowBackground

    onClosing: function(closeEvent) {
        if (!shutdownApproved) {
            projectShutdownReady = projectCoordinator.requestShutdown()
            previewShutdownReady = previewCoordinator.requestShutdown()
            shutdownApproved = projectShutdownReady && previewShutdownReady
            closeEvent.accepted = shutdownApproved
        }
    }

    function finishShutdownIfReady() {
        if (projectShutdownReady && previewShutdownReady && !shutdownApproved) {
            shutdownApproved = true
            window.close()
        }
    }

    Connections {
        target: projectCoordinator
        function onShutdownReady() {
            window.projectShutdownReady = true
            window.finishShutdownIfReady()
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

    header: ToolBar {
        Row {
            spacing: AppTheme.itemSpacing
            anchors.verticalCenter: parent.verticalCenter
            Button { text: qsTr("Open Project…"); onClicked: projectDialog.open() }
            Button {
                text: qsTr("Cancel")
                visible: projectCoordinator.loading
                onClicked: projectCoordinator.cancelLoading()
            }
            Label { text: projectCoordinator.loading ? qsTr("Opening…") : qsTr("Read only") }
        }
    }

    Column {
        anchors.fill: parent
        anchors.margins: AppTheme.pageMargin
        spacing: AppTheme.itemSpacing

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
                + projectCoordinator.model.durationFramesText + qsTr(" frames — read only")
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
                            border.color: AppTheme.clipBorder
                            border.width: AppTheme.borderWidth

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

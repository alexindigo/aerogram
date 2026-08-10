import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import Aerogram

// Email messages panel — the right panel. Renders the controller's
// activeMessages list: one message today, a thread later (the contract
// is a list on purpose). Independent component: reads controller
// properties, emits attachmentSaveRequested; knows nothing about the
// conversations panel or the sidebar.
Item {
    id: messagesPanel

    signal attachmentSaveRequested(string messageId, int partIndex, string path)

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        visible: accountController.activeMessages.length === 0
        text: "Inbox zero"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8
        visible: accountController.activeMessages.length > 0

        Repeater {
            model: accountController.activeMessages

            delegate: ColumnLayout {
                id: messageDelegate
                // Capture for the attachment chips: the inner Repeater's
                // modelData shadows the message's modelData.
                property string messageId: modelData.messageId || ""
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 8

                // -- sticky header --
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    IdentityBlock {
                        sender: modelData.sender || ""
                    }

                    Label {
                        text: modelData.sender || ""
                        font.pixelSize: 13
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }

                    Label {
                        text: modelData.date
                              ? Qt.formatDateTime(modelData.date, "MMM d, hh:mm")
                              : ""
                        font.pixelSize: 12
                        color: Kirigami.Theme.disabledTextColor
                    }
                }

                Label {
                    text: modelData.subject || ""
                    font.bold: true
                    font.pixelSize: 16
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumHeight: 1
                    color: Kirigami.Theme.disabledTextColor
                    opacity: 0.3
                }

                // -- scrolling body --
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    Label {
                        text: modelData.body || ""
                        wrapMode: Text.Wrap
                        width: messagesPanel.width - 40
                    }
                }

                // -- sticky attachment chips bar (only when present) --
                Rectangle {
                    visible: attachmentBar.visible
                    Layout.fillWidth: true
                    Layout.minimumHeight: 1
                    color: Kirigami.Theme.disabledTextColor
                    opacity: 0.3
                }

                Flow {
                    id: attachmentBar
                    visible: (modelData.attachments || []).length > 0
                    Layout.fillWidth: true
                    spacing: 8

                    Repeater {
                        model: modelData.attachments || []

                        delegate: Rectangle {
                            height: 30
                            width: chipRow.implicitWidth + 20
                            radius: 15
                            color: Kirigami.Theme.alternateBackgroundColor
                            border.width: 1
                            border.color: Kirigami.Theme.disabledTextColor

                            RowLayout {
                                id: chipRow
                                anchors.centerIn: parent
                                spacing: 6

                                AeroIcon {
                                    name: "paperclip"
                                    implicitWidth: 14
                                    implicitHeight: 14
                                }

                                Label {
                                    text: modelData.filename + "  " +
                                          Math.max(1, Math.round(modelData.size / 1024)) + " KB"
                                    font.pixelSize: 11
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    saveDialog.pendingMessageId = messageDelegate.messageId
                                    saveDialog.pendingIndex = modelData.index
                                    saveDialog.currentFile = modelData.filename
                                    saveDialog.open()
                                }
                            }
                        }
                    }
                }
            }
        }

        // Save-status line (bound to the controller property — no
        // imperative push from main.qml).
        Label {
            visible: text.length > 0
            text: accountController.attachmentSaveStatus
            font.pixelSize: 11
            color: Kirigami.Theme.disabledTextColor
        }
    }

    FileDialog {
        id: saveDialog
        fileMode: FileDialog.SaveFile
        property string pendingMessageId: ""
        property int pendingIndex: -1
        onAccepted: {
            // strip file:// AND decode percent-encoding (%20 etc.)
            const path = decodeURIComponent(String(selectedFile).replace(/^file:\/\//, ""))
            messagesPanel.attachmentSaveRequested(pendingMessageId, pendingIndex, path)
        }
    }
}

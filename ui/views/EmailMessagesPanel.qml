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

    // The pane is a dumb projection of controller state: body fields
    // (body / readerHtml / sanitizedHtml / rawText via controller) are
    // always FULL documents when set. No chunk protocol, no stream API.

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
            id: messageRepeater
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

                // -- header/body separator with centered view-mode pill --
                // Modes: 0=Raw (.eml), 1=Text (plain), 2=Reader (default),
                //        3=HTML (full sanitized). The pane binds full
                //        documents only; the controller decides how state
                //        got there.
                Item {
                    id: modeSeparator
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(modePill.implicitHeight, 12)

                    property int viewMode: 2   // 0 raw, 1 text, 2 reader, 3 html

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        height: 1
                        color: Kirigami.Theme.disabledTextColor
                        opacity: 0.3
                    }

                    Rectangle {
                        id: modePill
                        anchors.centerIn: parent
                        implicitWidth: pillRow.implicitWidth + 8
                        implicitHeight: pillRow.implicitHeight + 6
                        radius: height / 2
                        color: Kirigami.Theme.backgroundColor
                        border.color: Kirigami.Theme.disabledTextColor
                        border.width: 1

                        Row {
                            id: pillRow
                            anchors.centerIn: parent
                            spacing: 0

                            Repeater {
                                model: [
                                    { label: "Raw",    mode: 0 },
                                    { label: "Text",   mode: 1 },
                                    { label: "Reader", mode: 2 },
                                    { label: "HTML",   mode: 3 }
                                ]
                                delegate: Item {
                                    required property var modelData
                                    width: segLabel.implicitWidth + 16
                                    height: segLabel.implicitHeight + 6

                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 2
                                        radius: height / 2
                                        color: modeSeparator.viewMode === modelData.mode
                                               ? Kirigami.Theme.highlightColor
                                               : "transparent"
                                    }
                                    Label {
                                        id: segLabel
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        font.pixelSize: 11
                                        font.bold: modeSeparator.viewMode === modelData.mode
                                        color: modeSeparator.viewMode === modelData.mode
                                               ? Kirigami.Theme.highlightedTextColor
                                               : Kirigami.Theme.textColor
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: modeSeparator.viewMode = modelData.mode
                                    }
                                }
                            }
                        }
                    }

                    Connections {
                        target: messageDelegate
                        function onMessageIdChanged() {
                            modeSeparator.viewMode = 2   // Reader default
                            bodyArea.rawText = ""
                            bodyArea.htmlPainted = false
                            htmlEdit.text = ""
                        }
                    }
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: (modelData.remoteContentBlocked === true)
                             && (modeSeparator.viewMode === 2 || modeSeparator.viewMode === 3)
                    text: "Remote content blocked (tracking protection)."
                    type: Kirigami.MessageType.Information
                }

                // -- body: four views over full documents in state ----
                Item {
                    id: bodyArea
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    property string rawText: ""
                    property bool htmlPainted: false
                    readonly property bool expectingHtml: modelData.hasHtml === true
                    readonly property string readerDoc: modelData.readerHtml || ""
                    readonly property string fullHtml: modelData.sanitizedHtml || ""
                    readonly property string plainDoc: modelData.body || ""
                    property bool bodyLoaded: {
                        if (modeSeparator.viewMode === 0)
                            return rawText.length > 0
                        if (modeSeparator.viewMode === 1)
                            return plainDoc.length > 0
                        if (modeSeparator.viewMode === 3)
                            return expectingHtml || plainDoc.length > 0
                        // Reader (default)
                        return readerDoc.length > 0
                               || (!expectingHtml && plainDoc.length > 0)
                    }

                    function ensureRaw() {
                        if (rawText.length > 0)
                            return
                        const src = accountController.rawMessageSource(messageDelegate.messageId)
                        rawText = src && src.length > 0
                                  ? src
                                  : "(no raw source stored for this message)"
                    }

                    Connections {
                        target: modeSeparator
                        function onViewModeChanged() {
                            if (modeSeparator.viewMode === 0)
                                bodyArea.ensureRaw()
                            if (modeSeparator.viewMode === 3 && !bodyArea.htmlPainted
                                    && (bodyArea.fullHtml.length > 0 || bodyArea.plainDoc.length > 0)) {
                                // Lazy: layout cost only when the tab shows.
                                htmlEdit.text = bodyArea.fullHtml.length > 0
                                                ? bodyArea.fullHtml
                                                : bodyArea.plainDoc
                                htmlEdit.textFormat = bodyArea.fullHtml.length > 0
                                                      ? TextEdit.RichText
                                                      : TextEdit.PlainText
                                bodyArea.htmlPainted = true
                            }
                        }
                    }

                    // Chrome for the rich documents (Reader + HTML):
                    // margins, UI font, theme-aware stylesheet.
                    Component.onCompleted: {
                        textDocumentChrome.applyReaderChrome(
                            readerEdit, Kirigami.Theme.linkColor,
                            Kirigami.Theme.disabledTextColor)
                        textDocumentChrome.applyReaderChrome(
                            htmlEdit, Kirigami.Theme.linkColor,
                            Kirigami.Theme.disabledTextColor)
                    }

                    // ---- Reader mode (default): calm one-shot doc ----
                    ScrollView {
                        anchors.fill: parent
                        visible: modeSeparator.viewMode === 2
                        clip: true
                        TextEdit {
                            id: readerEdit
                            width: bodyArea.width - 16
                            textFormat: bodyArea.readerDoc.length > 0
                                        ? TextEdit.RichText
                                        : TextEdit.PlainText
                            text: bodyArea.readerDoc.length > 0
                                  ? bodyArea.readerDoc
                                  : bodyArea.plainDoc
                            wrapMode: TextEdit.Wrap
                            readOnly: true
                            selectByMouse: true
                            color: Kirigami.Theme.textColor
                            onLinkActivated: (url) => Qt.openUrlExternally(url)
                        }
                    }

                    // ---- HTML mode: full sanitized doc (lazy paint) ----
                    ScrollView {
                        anchors.fill: parent
                        visible: modeSeparator.viewMode === 3
                        clip: true
                        TextEdit {
                            id: htmlEdit
                            width: bodyArea.width - 16
                            textFormat: TextEdit.PlainText
                            text: ""
                            wrapMode: TextEdit.Wrap
                            readOnly: true
                            selectByMouse: true
                            color: Kirigami.Theme.textColor
                            onLinkActivated: (url) => Qt.openUrlExternally(url)
                        }
                    }

                    // ---- Text mode: proportional reading face ----
                    ScrollView {
                        anchors.fill: parent
                        visible: modeSeparator.viewMode === 1
                        clip: true
                        TextEdit {
                            width: bodyArea.width - 16
                            textFormat: TextEdit.PlainText
                            text: bodyArea.plainDoc
                            wrapMode: TextEdit.Wrap
                            readOnly: true
                            selectByMouse: true
                            color: Kirigami.Theme.textColor
                            font.pixelSize: 13
                        }
                    }

                    // ---- Raw mode: .eml source ----
                    ScrollView {
                        anchors.fill: parent
                        visible: modeSeparator.viewMode === 0
                        clip: true
                        TextEdit {
                            width: bodyArea.width - 16
                            textFormat: TextEdit.PlainText
                            text: bodyArea.rawText
                            wrapMode: TextEdit.Wrap
                            readOnly: true
                            selectByMouse: true
                            color: Kirigami.Theme.textColor
                            font.family: "monospace"
                            font.pixelSize: 11
                        }
                    }

                    Column {
                        anchors.centerIn: parent
                        visible: !bodyArea.bodyLoaded
                        spacing: 10
                        BusyIndicator {
                            anchors.horizontalCenter: parent.horizontalCenter
                            running: true
                        }
                        Label {
                            text: modeSeparator.viewMode === 0
                                  ? "Loading raw source…"
                                  : "Loading message…"
                            color: Kirigami.Theme.disabledTextColor
                        }
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

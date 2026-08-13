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

    /// Chunk subscription lives ON the panel (Connections dies with it).
    /// The previous design — main.qml imperatively connecting the
    /// controller's signal to a captured `item` — leaked a connection
    /// per panel-layout rebuild, each holding a destroyed delegate
    /// (198 "is not a function" TypeErrors in one session).
    Connections {
        target: accountController
        function onMessageBodyChunkReady(convId, mid, chunk, lastChunk, blocked) {
            messagesPanel.appendBodyChunk(mid, chunk, lastChunk, blocked)
        }
    }

    /// Progressive render entry point. Appends a sanitized html
    /// chunk into the open message's body — imperative, so no model
    /// rebuild can wipe the stream.
    function appendBodyChunk(messageId, chunk, lastChunk, blocked) {
        // v1: one open message — the repeater's only delegate.
        const d = messageRepeater.itemAt(0)
        if (d && d.messageId === messageId)
            d.appendChunk(chunk, lastChunk)
    }

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

                // Panel-facing entry point: appendBodyChunk calls THIS on
                // the delegate root (ids don't resolve across instances).
                function appendChunk(chunk, lastChunk) {
                    bodyArea.appendChunkImpl(chunk, lastChunk)
                }
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
                // Modes: 0=HTML (single document, progressive append),
                //        1=Text (plain), 2=Raw (.eml).
                Item {
                    id: modeSeparator
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(modePill.implicitHeight, 12)

                    property int viewMode: 0   // 0 html, 1 text, 2 raw

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
                                    { label: "HTML", mode: 0 },
                                    { label: "Text", mode: 1 },
                                    { label: "Raw",  mode: 2 }
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
                            modeSeparator.viewMode = 0
                            bodyArea.resetHtml()
                            bodyArea.rawText = ""
                        }
                    }
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: (modelData.remoteContentBlocked === true)
                             && modeSeparator.viewMode === 0
                    text: "Remote content blocked (tracking protection)."
                    type: Kirigami.MessageType.Information
                }

                // -- body: one document for HTML (progressive append),
                //    separate plain/raw TextEdits for the other pill modes.
                Item {
                    id: bodyArea
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    property string rawText: ""
                    property bool htmlStarted: false
                    property int chunkIndex: 0
                    readonly property bool expectingHtml: modelData.hasHtml === true
                    property bool bodyLoaded: {
                        if (modeSeparator.viewMode === 0)
                            return htmlStarted
                                    || (!expectingHtml && (modelData.body || "").length > 0)
                        if (modeSeparator.viewMode === 1)
                            return (modelData.body || "").length > 0
                        return rawText.length > 0
                    }

                    function resetHtml() {
                        htmlStarted = false
                        chunkIndex = 0
                        htmlEdit.textFormat = TextEdit.PlainText
                        htmlEdit.text = ""
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
                            if (modeSeparator.viewMode === 2)
                                bodyArea.ensureRaw()
                        }
                    }

                    // Plain-only mail: one-shot into the HTML-mode editor
                    // as plain text (no chunk stream).
                    property string plainBody: modelData.body || ""
                    onPlainBodyChanged: {
                        if (expectingHtml || plainBody.length === 0)
                            return
                        if (htmlStarted)
                            return
                        htmlEdit.textFormat = TextEdit.PlainText
                        htmlEdit.text = plainBody
                        htmlStarted = true
                    }

                    // Progressive HTML: append sanitized fragments into
                    // one QTextDocument (no ListView / no fake blocks).
                    function appendChunkImpl(chunk, lastChunk) {
                        if (!htmlStarted) {
                            console.log("PERF first-chunk msg=" + modelData.messageId
                                        + " abs=" + Date.now() + " len=" + chunk.length)
                            htmlEdit.textFormat = TextEdit.RichText
                            htmlEdit.text = ""
                            htmlStarted = true
                        }
                        const t0 = Date.now()
                        htmlTextAppender.appendHtml(htmlEdit, chunk)
                        console.log("PERF chunk-paint msg=" + modelData.messageId
                                    + " dur=" + (Date.now() - t0)
                                    + " len=" + chunk.length
                                    + " i=" + chunkIndex)
                        chunkIndex += 1
                        if (lastChunk)
                            console.log("PERF last-chunk msg=" + modelData.messageId
                                        + " abs=" + Date.now()
                                        + " chunks=" + chunkIndex)
                    }

                    // ---- HTML mode: single progressive TextEdit ----
                    ScrollView {
                        anchors.fill: parent
                        visible: modeSeparator.viewMode === 0
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

                    // ---- Text mode ----
                    ScrollView {
                        anchors.fill: parent
                        visible: modeSeparator.viewMode === 1
                        clip: true
                        TextEdit {
                            width: bodyArea.width - 16
                            textFormat: TextEdit.PlainText
                            text: modelData.body || ""
                            wrapMode: TextEdit.Wrap
                            readOnly: true
                            selectByMouse: true
                            color: Kirigami.Theme.textColor
                            font.family: "monospace"
                            font.pixelSize: 12
                        }
                    }

                    // ---- Raw mode ----
                    ScrollView {
                        anchors.fill: parent
                        visible: modeSeparator.viewMode === 2
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
                            text: modeSeparator.viewMode === 2
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

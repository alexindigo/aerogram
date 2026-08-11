import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import "../components"

// Add-account panel: pick a backend from the registry-advertised list,
// then fill the credential fields that backend declares. The panel is
// schema-driven — it knows nothing about concrete backends.
Item {
    id: root

    // Semantic signal: carries the assembled credentials map up to the
    // orchestrator. The panel itself never touches the controller.
    signal accountSubmitted(var credentials)
    signal cancelled()

    // Index into accountController.availableBackends; -1 = picker shown.
    property int selectedBackend: -1
    // Field values keyed by field key; reset on backend switch.
    property var values: ({})

    function selectBackend(idx) {
        selectedBackend = idx
        values = {}
        // Pre-fill bool defaults (unchecked = false is implicit).
        const fields = accountController.availableBackends[idx].fields
        const v = {}
        for (const f of fields) {
            if (f.kind === "bool")
                v[f.key] = true  // e.g. TLS defaults on
        }
        values = v
    }

    function requiredFilled() {
        if (selectedBackend < 0)
            return false
        for (const f of accountController.availableBackends[selectedBackend].fields) {
            if (!f.required)
                continue
            const val = values[f.key]
            if (val === undefined || String(val).trim().length === 0)
                return false
        }
        return true
    }

    function submit() {
        const backend = accountController.availableBackends[selectedBackend]
        const creds = { "type": backend.type }
        for (const f of backend.fields) {
            let val = values[f.key]
            if (val === undefined)
                continue
            if (f.kind === "int")
                val = parseInt(val) || 0
            else if (f.kind !== "bool")
                val = String(val).trim()
            creds[f.key] = val
        }
        accountSubmitted(creds)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Kirigami.Units.largeSpacing * 2
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Heading {
            text: root.selectedBackend < 0
                  ? "Add account"
                  : "Add " + accountController.availableBackends[root.selectedBackend].displayName
            level: 2
        }

        // --- Backend picker -------------------------------------------------
        ListView {
            visible: root.selectedBackend < 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: accountController.availableBackends
            spacing: Kirigami.Units.smallSpacing

            delegate: Rectangle {
                required property int index
                required property var modelData
                width: ListView.view.width
                height: pickRow.implicitHeight + Kirigami.Units.largeSpacing
                radius: 6
                color: pickHover.hovered
                       ? Kirigami.Theme.activeBackgroundColor
                       : Kirigami.Theme.backgroundColor
                border.color: Kirigami.Theme.disabledTextColor
                border.width: 1

                HoverHandler { id: pickHover }
                TapHandler {
                    onTapped: root.selectBackend(index)
                }

                RowLayout {
                    id: pickRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: Kirigami.Units.largeSpacing
                    spacing: Kirigami.Units.largeSpacing

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: modelData.displayName
                            font.bold: true
                        }
                        Label {
                            text: modelData.description
                            font.pixelSize: 11
                            color: Kirigami.Theme.disabledTextColor
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }

                    Kirigami.Icon {
                        source: "go-next"
                        implicitWidth: Kirigami.Units.iconSizes.small
                        implicitHeight: Kirigami.Units.iconSizes.small
                    }
                }
            }
        }

        // --- Dynamic credential form ----------------------------------------
        ScrollView {
            visible: root.selectedBackend >= 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            enabled: !accountController.accountAddInProgress

            ColumnLayout {
                width: parent.width
                spacing: Kirigami.Units.largeSpacing

                Repeater {
                    model: root.selectedBackend >= 0
                           ? accountController.availableBackends[root.selectedBackend].fields
                           : []

                    delegate: Loader {
                        id: fieldLoader
                        required property var modelData
                        Layout.fillWidth: true
                        sourceComponent: modelData.kind === "bool" ? boolField
                                       : modelData.kind === "qr" ? qrField
                                       : textField

                        Component {
                            id: textField
                            ColumnLayout {
                                spacing: 4
                                Label {
                                    text: fieldLoader.modelData.label
                                          + (fieldLoader.modelData.required ? " *" : " (optional)")
                                    font.pixelSize: 12
                                }
                                TextField {
                                    Layout.fillWidth: true
                                    placeholderText: fieldLoader.modelData.placeholder
                                    echoMode: fieldLoader.modelData.kind === "password"
                                              ? TextInput.Password : TextInput.Normal
                                    inputMethodHints: fieldLoader.modelData.kind === "int"
                                                      ? Qt.ImhDigitsOnly : Qt.ImhNone
                                    text: root.values[fieldLoader.modelData.key] !== undefined
                                          ? String(root.values[fieldLoader.modelData.key]) : ""
                                    onTextChanged: {
                                        const v = root.values
                                        v[fieldLoader.modelData.key] = text
                                        root.values = Object.assign({}, v)
                                    }
                                }
                            }
                        }

                        // "qr" kind: text field that can also be filled
                        // by scanning (camera or image file).
                        Component {
                            id: qrField
                            ColumnLayout {
                                spacing: 4
                                Label {
                                    text: fieldLoader.modelData.label
                                          + (fieldLoader.modelData.required ? " *" : " (optional)")
                                    font.pixelSize: 12
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Kirigami.Units.smallSpacing

                                    TextField {
                                        id: qrTextField
                                        Layout.fillWidth: true
                                        placeholderText: fieldLoader.modelData.placeholder
                                        text: root.values[fieldLoader.modelData.key] !== undefined
                                              ? String(root.values[fieldLoader.modelData.key]) : ""
                                        onTextChanged: {
                                            const v = root.values
                                            v[fieldLoader.modelData.key] = text
                                            root.values = Object.assign({}, v)
                                        }
                                    }

                                    Button {
                                        text: "Scan…"
                                        icon.name: "view-barcode"
                                        onClicked: qrScanner.open()
                                    }
                                }

                                QrScannerDialog {
                                    id: qrScanner
                                    onDecoded: (text) => {
                                        const v = root.values
                                        v[fieldLoader.modelData.key] = text
                                        root.values = Object.assign({}, v)
                                        qrTextField.text = text
                                    }
                                }
                            }
                        }

                        Component {
                            id: boolField
                            CheckBox {
                                text: fieldLoader.modelData.label
                                checked: root.values[fieldLoader.modelData.key] === true
                                onCheckedChanged: {
                                    const v = root.values
                                    v[fieldLoader.modelData.key] = checked
                                    root.values = Object.assign({}, v)
                                }
                            }
                        }
                    }
                }
            }
        }

        // --- Actions ---------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Button {
                visible: root.selectedBackend >= 0
                text: "Back"
                onClicked: root.selectedBackend = -1
            }

            Item { Layout.fillWidth: true }

            // While a connection attempt is in flight: spinner replaces
            // the submit button; the status message narrates the stage.
            BusyIndicator {
                visible: accountController.accountAddInProgress
                running: visible
            }

            Button {
                text: "Add account"
                visible: root.selectedBackend >= 0
                         && !accountController.accountAddInProgress
                enabled: root.requiredFilled()
                onClicked: root.submit()
            }
        }

        // Result feedback: an InlineMessage (severity-colored, hard to
        // miss) instead of faint grey text.
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: text.length > 0
            text: accountController.configStatus
            type: {
                const t = accountController.configStatus.toLowerCase()
                if (t.includes("fail") || t.includes("error"))
                    return Kirigami.MessageType.Error
                if (t.includes("added") || t.includes("connected"))
                    return Kirigami.MessageType.Positive
                return Kirigami.MessageType.Information
            }
        }
    }
}

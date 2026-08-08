import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Full-window lock overlay shown while the vault is locked. Emits
// domain-named signals only; main.qml wires them to controller slots.
// Form selection is driven by controller booleans (vaultExists /
// vaultNeedsRecovery), never by parsing status strings.
Rectangle {
    id: lockOverlay
    anchors.fill: parent
    color: Kirigami.Theme.backgroundColor
    visible: accountController.isLocked
    z: 100

    signal createVaultRequested(string password, string phrase)
    signal unlockRequested(string password)
    signal recoveryRequested(string password, string phrase)

    // Which form: first-run (no vault) / recovery (damaged) / daily.
    readonly property bool firstRun: !accountController.vaultExists
    readonly property bool recovery: accountController.vaultExists
                                   && accountController.vaultNeedsRecovery

    // Local UI state (transient, allowed): confirm fields, the
    // show-recovery toggle, and a local mismatch note.
    property bool showRecovery: false
    property string mismatchNote: ""

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 16
        width: 340

        Kirigami.Icon {
            source: "lock"
            implicitWidth: 48
            implicitHeight: 48
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            text: lockOverlay.firstRun ? "Create your vault"
                : lockOverlay.showRecovery || lockOverlay.recovery ? "Recover your vault"
                : "Aerogram is locked"
            font.pixelSize: 20
            font.bold: true
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            text: accountController.lockStatusText
            color: Kirigami.Theme.disabledTextColor
            Layout.alignment: Qt.AlignHCenter
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
        }

        // ---- master password ----
        TextField {
            id: passInput
            Layout.fillWidth: true
            placeholderText: "Master password"
            echoMode: TextInput.Password
        }

        TextField {
            id: passConfirm
            Layout.fillWidth: true
            placeholderText: "Confirm master password"
            echoMode: TextInput.Password
            visible: lockOverlay.firstRun
        }

        // ---- secret key phrase (first-run + recovery) ----
        TextField {
            id: phraseInput
            Layout.fillWidth: true
            placeholderText: "Secret Key phrase"
            visible: lockOverlay.firstRun || lockOverlay.showRecovery || lockOverlay.recovery
        }

        TextField {
            id: phraseConfirm
            Layout.fillWidth: true
            placeholderText: "Confirm Secret Key phrase"
            visible: lockOverlay.firstRun
        }

        Label {
            visible: lockOverlay.firstRun
            text: "Three or more words you'll remember. Store it in your password manager."
            font.pixelSize: 11
            color: Kirigami.Theme.disabledTextColor
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        Label {
            visible: lockOverlay.mismatchNote.length > 0
            text: lockOverlay.mismatchNote
            color: Kirigami.Theme.negativeTextColor
            font.pixelSize: 11
            Layout.alignment: Qt.AlignHCenter
        }

        Button {
            id: primaryButton
            Layout.fillWidth: true
            text: lockOverlay.firstRun ? "Create vault"
                : lockOverlay.showRecovery || lockOverlay.recovery ? "Recover"
                : "Unlock"
            enabled: passInput.text.length > 0
            onClicked: {
                lockOverlay.mismatchNote = ""

                if (lockOverlay.firstRun) {
                    if (passInput.text !== passConfirm.text) {
                        lockOverlay.mismatchNote = "Passwords don't match"
                        return
                    }
                    if (phraseInput.text !== phraseConfirm.text) {
                        lockOverlay.mismatchNote = "Secret Key phrases don't match"
                        return
                    }
                    lockOverlay.createVaultRequested(passInput.text, phraseInput.text)
                } else if (lockOverlay.showRecovery || lockOverlay.recovery) {
                    lockOverlay.recoveryRequested(passInput.text, phraseInput.text)
                } else {
                    lockOverlay.unlockRequested(passInput.text)
                }

                passInput.clear()
                passConfirm.clear()
                phraseInput.clear()
                phraseConfirm.clear()
            }
        }

        // Recovery link — daily form only, when the vault isn't already
        // known to be damaged.
        Label {
            visible: !lockOverlay.firstRun && !lockOverlay.recovery
            text: lockOverlay.showRecovery
                ? '<a href="#back">Back to unlock</a>'
                : '<a href="#recover">Vault damaged? Recover with Secret Key</a>'
            font.pixelSize: 11
            Layout.alignment: Qt.AlignHCenter
            onLinkActivated: lockOverlay.showRecovery = !lockOverlay.showRecovery
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Full-window lock/welcome overlay. Emits domain-named signals only;
// main.qml wires them to controller slots. Form selection is driven by
// controller booleans (showLockOverlay / vaultExists /
// vaultNeedsRecovery), never by parsing status strings.
//
// Layout follows the Aerogram Init sketch: welcome header, master
// password field, secret phrase field (first run / recovery), Continue.
Rectangle {
    id: lockOverlay
    anchors.fill: parent
    color: Kirigami.Theme.backgroundColor
    visible: accountController.showLockOverlay
    z: 100

    signal createVaultRequested(string password, string phrase)
    signal unlockRequested(string password)
    signal recoveryRequested(string password, string phrase)

    // Which form: first-run (no vault) / recovery (damaged) / daily.
    readonly property bool firstRun: !accountController.vaultExists
    readonly property bool recovery: accountController.vaultExists
                                   && (accountController.vaultNeedsRecovery || showRecovery)

    // Local UI state (transient, allowed): the show-recovery toggle.
    property bool showRecovery: false

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 16
        width: 340

        Label {
            text: lockOverlay.firstRun ? "Welcome to Aerogram"
                : lockOverlay.recovery ? "Vault recovery"
                : "Welcome back"
            font.pixelSize: 20
            font.bold: true
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            visible: text.length > 0
            text: lockOverlay.firstRun
                ? "Three or more words you'll remember for the Secret Key. Store it in your password manager."
                : accountController.lockStatusText
            color: Kirigami.Theme.disabledTextColor
            Layout.alignment: Qt.AlignHCenter
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
        }

        TextField {
            id: passInput
            Layout.fillWidth: true
            placeholderText: "master password"
            echoMode: TextInput.Password
            onAccepted: continueButton.clicked()
        }

        TextField {
            id: phraseInput
            Layout.fillWidth: true
            placeholderText: "secret phrase"
            visible: lockOverlay.firstRun || lockOverlay.recovery
            onAccepted: continueButton.clicked()
        }

        Button {
            id: continueButton
            Layout.fillWidth: true
            text: "Continue"
            enabled: passInput.text.length > 0
                  && (!lockOverlay.firstRun && !lockOverlay.recovery
                      || phraseInput.text.length > 0)
            onClicked: {
                if (lockOverlay.firstRun) {
                    lockOverlay.createVaultRequested(passInput.text, phraseInput.text)
                } else if (lockOverlay.recovery) {
                    lockOverlay.recoveryRequested(passInput.text, phraseInput.text)
                } else {
                    lockOverlay.unlockRequested(passInput.text)
                }

                passInput.clear()
                phraseInput.clear()
            }
        }

        // Recovery link — daily form only, when the vault isn't already
        // known to be damaged.
        Label {
            visible: !lockOverlay.firstRun && !accountController.vaultNeedsRecovery
            text: lockOverlay.showRecovery
                ? '<a href="#back">Back to unlock</a>'
                : '<a href="#recover">Vault damaged? Recover with Secret Key</a>'
            font.pixelSize: 11
            Layout.alignment: Qt.AlignHCenter
            onLinkActivated: lockOverlay.showRecovery = !lockOverlay.showRecovery
        }
    }
}

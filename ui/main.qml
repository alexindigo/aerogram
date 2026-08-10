import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Aerogram

Kirigami.ApplicationWindow {
    id: root
    width: 1024
    height: 768
    title: "Aerogram"

    // The controller owns panel position and size (panelLayout model);
    // this window only binds to it. Panels are independent components —
    // no panel references another, and a future second window is just
    // another host binding the same model.
    readonly property var panelSources: ({
        "sidebar": "components/Sidebar.qml",
        "email-conversations": "views/EmailConversationsPanel.qml",
        "email-messages": "views/EmailMessagesPanel.qml",
        "chat-conversations": "views/ChatView.qml",
        "settings": "views/SettingsView.qml"
    })

    onWidthChanged: accountController.setWindowSize(width, height)
    onHeightChanged: accountController.setWindowSize(width, height)
    Component.onCompleted: accountController.setWindowSize(width, height)

    function wirePanel(id, item) {
        if (id === "sidebar") {
            item.settingsRequested.connect(function() {
                accountController.setActiveView("settings")
            })
            item.addAccountRequested.connect(function() { addAccountDialog.open() })
            item.accountSelected.connect(function(accId) {
                accountController.selectAccount(accId)
            })
        } else if (id === "email-conversations") {
            item.messageSelected.connect(function(mid) {
                accountController.selectMessage(mid)
            })
        } else if (id === "email-messages") {
            item.attachmentSaveRequested.connect(function(mid, idx, path) {
                accountController.saveAttachment(mid, idx, path)
            })
        } else if (id === "chat-conversations") {
            item.chatSelected.connect(function(cid) {
                accountController.fetchMessages(cid)
                accountController.setActiveView("email")
            })
        } else if (id === "settings") {
            item.setupFromQrRequested.connect(function(qr) {
                accountController.setupFromQr(qr)
            })
            item.getBackupFromQrRequested.connect(function(qr) {
                accountController.getBackupFromQr(qr)
            })
            item.resetApplicationRequested.connect(function() {
                accountController.resetApp()
            })
        }
    }

    Repeater {
        model: accountController.panelLayout
        delegate: Item {
            x: modelData.x
            y: modelData.y
            width: modelData.width
            height: modelData.height
            visible: modelData.visible

            // Panel content (Loader) or a separator divider.
            Loader {
                anchors.fill: parent
                active: modelData.type === "panel"
                source: modelData.type === "panel"
                        ? root.panelSources[modelData.id] : ""
                onLoaded: root.wirePanel(modelData.id, item)
            }

            Rectangle {
                anchors.fill: parent
                visible: modelData.type === "separator"
                color: Kirigami.Theme.disabledTextColor
                opacity: 0.3
            }
        }
    }

    AddAccountDialog {
        id: addAccountDialog
        onAccountSubmitted: (credentials) => {
            accountController.addAccount(credentials)
        }
    }

    LockOverlay {
        onUnlockRequested: (pass) => {
            accountController.unlockWithPassphrase(pass)
        }
        onCreateVaultRequested: (pass, phrase) => {
            accountController.createVault(pass, phrase)
        }
        onRecoveryRequested: (pass, phrase) => {
            accountController.recoverVault(pass, phrase)
        }
    }
}

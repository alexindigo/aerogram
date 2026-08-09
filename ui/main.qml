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

    readonly property var viewMap: ({ "email": 0, "chats": 1, "settings": 2 })

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Sidebar {
            Layout.fillHeight: true

            onSettingsRequested: accountController.setActiveView("settings")
            onAddAccountRequested: addAccountDialog.open()
            onAccountSelected: (id) => accountController.selectAccount(id)
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.viewMap[accountController.activeView] ?? 0

            EmailInboxView {
                id: emailInboxView
                onMessageSelected: (messageId) => {
                    accountController.selectMessage(messageId)
                }
                onAttachmentSaveRequested: (messageId, partIndex, path) => {
                    accountController.saveAttachment(messageId, partIndex, path)
                }
            }

            ChatView {
                onChatSelected: (conversationId) => {
                    accountController.fetchMessages(conversationId)
                    accountController.setActiveView("email")
                }
            }

            SettingsView {
                onSetupFromQrRequested: (qrContent) => {
                    accountController.setupFromQr(qrContent)
                }
                onGetBackupFromQrRequested: (qrText) => {
                    accountController.getBackupFromQr(qrText)
                }
                onResetApplicationRequested: accountController.resetApp()
            }
        }
    }

    AddAccountDialog {
        id: addAccountDialog
        onAccountSubmitted: (credentials) => {
            accountController.addAccount(credentials)
        }
    }

    Connections {
        target: accountController
        function onAttachmentSaved(ok, messageId, path) {
            emailInboxView.saveStatusText = ok ? "Saved to " + path : "Save failed"
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

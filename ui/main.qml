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
            currentSection: accountController.activeView

            onInboxRequested: accountController.setActiveView("email")
            onChatsRequested: accountController.setActiveView("chats")
            onSettingsRequested: accountController.setActiveView("settings")
            onResetApplicationRequested: accountController.resetApp()
            onAddAccountRequested: addAccountDialog.open()
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.viewMap[accountController.activeView] ?? 0

            EmailInboxView {
                onMessageSelected: (messageId) => {
                    accountController.selectMessage(messageId)
                }
            }

            ChatView {
                onChatSelected: (conversationId) => {
                    accountController.fetchMessages(conversationId)
                }
            }

            SettingsView {
                onSetupFromQrRequested: (qrContent) => {
                    accountController.setupFromQr(qrContent)
                }
                onGetBackupFromQrRequested: (qrText) => {
                    accountController.getBackupFromQr(qrText)
                }
            }
        }
    }

    AddAccountDialog {
        id: addAccountDialog
        onAccountSubmitted: (credentials) => {
            accountController.addAccount(credentials)
        }
    }
}

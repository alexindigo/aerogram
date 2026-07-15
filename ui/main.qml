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

    property string activeView: "email"
    readonly property var viewMap: { "email": 0, "chats": 1, "settings": 2 }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Sidebar {
            Layout.fillHeight: true
            currentSection: root.activeView

            onInboxRequested: root.activeView = "email"
            onChatsRequested: root.activeView = "chats"
            onSettingsRequested: root.activeView = "settings"
            onResetApplicationRequested: accountController.resetApp()
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.viewMap[root.activeView] ?? 0

            EmailInboxView {
                onMessageDetailsRequested: (messageId) => {
                    accountController.fetchMessages(messageId)
                }
                onChatThreadRequested: (emailAddress) => {
                    console.log("chat thread requested for", emailAddress)
                }
                onAccountSyncRequested: {
                    accountController.triggerSync()
                }
            }

            ChatView {
                onChatSelected: (chatId) => {
                    accountController.fetchMessages(chatId)
                }
                onComposeMessageRequested: {
                    accountController.sendMessage(accountController.chatListModel.chatIdAtRow(0), "hello")
                }
                onGroupInfoRequested: (chatId) => {
                    console.log("group info requested for", chatId)
                }
            }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Aerogram

// Account rail: account chips grouped by type (Email / Chat), with
// add-account and settings at the bottom. Chips show address-derived
// initials on a deterministic color; the active account gets a border.
Rectangle {
    id: sidebar
    width: 70
    color: Kirigami.Theme.backgroundColor

    signal addAccountRequested()
    signal settingsRequested()
    signal accountSelected(string accountId)
    /// Right-click on a pill → "Remove account" (with confirmation).
    signal accountRemoveRequested(string accountId)

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        spacing: 10

        ListView {
            id: accountList
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: accountController.accountsModel
            spacing: 10
            clip: true

            section.property: "type"
            section.delegate: Label {
                text: section === "email" ? "Email" : "Chat"
                font.pixelSize: 10
                color: Kirigami.Theme.disabledTextColor
                horizontalAlignment: Text.AlignHCenter
                width: accountList.width
                topPadding: 6
            }

            delegate: Item {
                width: accountList.width
                height: 44

                Rectangle {
                    width: 44
                    height: 44
                    radius: 8
                    anchors.centerIn: parent
                    color: model.color
                    border.width: accountController.activeAccountId === model.accountId ? 2 : 0
                    border.color: Kirigami.Theme.highlightColor

                    Label {
                        anchors.centerIn: parent
                        text: model.chipText
                        color: "white"
                        font.bold: true
                        font.pixelSize: 14
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.RightButton)
                                removeMenu.popup()
                            else
                                sidebar.accountSelected(model.accountId)
                        }
                    }

                    Menu {
                        id: removeMenu
                        MenuItem {
                            text: "Remove account"
                            icon.name: "list-remove"
                            onTriggered: removeConfirm.open()
                        }
                    }

                    Kirigami.PromptDialog {
                        id: removeConfirm
                        title: "Remove account"
                        subtitle: "Remove " + model.accountId + "? Its stored "
                                  + "credentials and locally cached mail will "
                                  + "be deleted."
                        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel
                        onAccepted: sidebar.accountRemoveRequested(model.accountId)
                    }
                }
            }
        }

        // Bottom actions: add account, settings
        AeroIcon {
            name: "plus"
            implicitWidth: 24
            implicitHeight: 24
            Layout.alignment: Qt.AlignHCenter

            MouseArea {
                anchors.fill: parent
                onClicked: sidebar.addAccountRequested()
            }
        }

        AeroIcon {
            name: "settings"
            implicitWidth: 24
            implicitHeight: 24
            Layout.alignment: Qt.AlignHCenter

            MouseArea {
                anchors.fill: parent
                onClicked: sidebar.settingsRequested()
            }
        }
    }
}

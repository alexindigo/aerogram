import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: settingsView
    title: "Settings"

    signal setupFromQrRequested(string qrContent)
    signal getBackupFromQrRequested(string qrText)

    ColumnLayout {
        anchors.fill: parent
        spacing: 20
        anchors.margins: 16

        Rectangle {
            Layout.fillWidth: true
            Layout.minimumHeight: 1
            color: Kirigami.Theme.disabledTextColor
            opacity: 0.3
        }

        Label {
            text: "Second Device Backup"
            font.bold: true
            font.pixelSize: 14
        }

        Label {
            text: "Paste QR code from your existing Delta Chat app:\nSettings \u2192 Add Second Device \u2192 Show QR code / Copy to clipboard.\nBoth devices must be on the same network."
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            color: Kirigami.Theme.disabledTextColor
            font.pixelSize: 12
        }

        TextField {
            id: backupField
            Layout.fillWidth: true
            placeholderText: "DCBACKUP2:..."
        }

        Button {
            text: "Receive Backup"
            enabled: backupField.text.length > 0
            onClicked: settingsView.getBackupFromQrRequested(backupField.text)
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.minimumHeight: 1
            color: Kirigami.Theme.disabledTextColor
            opacity: 0.3
        }

        Label {
            text: "Account Setup via QR"
            font.bold: true
            font.pixelSize: 14
        }

        Label {
            text: "Paste a DCACCOUNT or DCLOGIN setup string to configure a new or existing email account directly."
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            color: Kirigami.Theme.disabledTextColor
            font.pixelSize: 12
        }

        TextField {
            id: accountField
            Layout.fillWidth: true
            placeholderText: "DCACCOUNT:... or DCLOGIN:..."
        }

        Button {
            text: "Configure Account"
            enabled: accountField.text.length > 0
            onClicked: settingsView.setupFromQrRequested(accountField.text)
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.minimumHeight: 1
            color: Kirigami.Theme.disabledTextColor
            opacity: 0.3
        }

        Label {
            text: accountController.configStatus
            color: Kirigami.Theme.disabledTextColor
            font.pixelSize: 12
        }
    }
}

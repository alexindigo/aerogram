import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Dialog {
    id: addAccountDialog
    title: "Add email account"
    modal: true
    standardButtons: Dialog.Close
    width: 420

    // Semantic signal: carries the credentials map up to the
    // orchestrator. The dialog itself knows nothing about controllers.
    signal accountSubmitted(var credentials)

    onClosed: {
        hostField.clear()
        userField.clear()
        passField.clear()
    }

    ColumnLayout {
        width: parent.width
        spacing: 10

        TextField {
            id: hostField
            Layout.fillWidth: true
            placeholderText: "imap.example.com"
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            TextField {
                id: portField
                Layout.preferredWidth: 90
                placeholderText: "993"
                inputMethodHints: Qt.ImhDigitsOnly
                text: "993"
            }

            CheckBox {
                id: tlsCheck
                text: "TLS"
                checked: true
            }
        }

        TextField {
            id: userField
            Layout.fillWidth: true
            placeholderText: "user@example.com"
        }

        TextField {
            id: passField
            Layout.fillWidth: true
            placeholderText: "Password or app password"
            echoMode: TextInput.Password
        }

        Button {
            Layout.fillWidth: true
            text: "Add account"
            enabled: hostField.text.trim().length > 0
                  && userField.text.trim().length > 0
            onClicked: {
                addAccountDialog.accountSubmitted({
                    "type": "imap",
                    "host": hostField.text.trim(),
                    "port": parseInt(portField.text) || 993,
                    "user": userField.text.trim(),
                    "pass": passField.text,
                    "tls": tlsCheck.checked
                })
            }
        }

        // Result feedback (account added, or the error) stays visible
        // until the dialog is closed.
        Label {
            visible: text.length > 0
            text: accountController.configStatus
            font.pixelSize: 11
            color: Kirigami.Theme.disabledTextColor
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }
}

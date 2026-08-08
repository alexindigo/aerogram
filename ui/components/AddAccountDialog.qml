import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Dialog {
    id: addAccountDialog
    title: "Add email account"
    modal: true
    standardButtons: Dialog.Ok | Dialog.Cancel
    width: 420

    // Semantic signal: carries the credentials map up to the
    // orchestrator. The dialog itself knows nothing about controllers.
    signal accountSubmitted(var credentials)

    onAccepted: {
        addAccountDialog.accountSubmitted({
            "type": "imap",
            "host": hostField.text.trim(),
            "port": parseInt(portField.text) || 993,
            "user": userField.text.trim(),
            "pass": passField.text,
            "tls": tlsCheck.checked
        })
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
    }
}

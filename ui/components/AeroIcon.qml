import QtQuick
import org.kde.kirigami as Kirigami

// Themed icon from the on-disk icon pack (Tabler default, extracted to
// the user's data dir on first run). Name maps to <pack>/<name>.svg.
// isMask so stroke icons take the theme text color.
Kirigami.Icon {
    property string name: ""
    isMask: true
    source: name.length > 0
        ? "file://" + accountController.iconPackDir + "/" + name + ".svg"
        : ""
}

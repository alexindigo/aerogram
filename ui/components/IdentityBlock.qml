import QtQuick
import QtQuick.Controls
import org.kde.kirigami as Kirigami

// Sender identity block: colored rounded square with initials derived
// from the sender's email ADDRESS (not the display name — anyone can
// forge a name). Deterministic per address, so lookalike addresses
// read differently at a glance (anti-spoofing). Gravatar/libravatar
// layers over this later.
Rectangle {
    id: block
    width: 44
    height: 44
    radius: 8

    property string sender: ""

    readonly property string address: {
        const s = sender || ""
        const lt = s.indexOf("<")
        const gt = s.indexOf(">")
        if (lt >= 0 && gt > lt) return s.substring(lt + 1, gt)
        return s
    }

    readonly property var colors: ["#4c9baf", "#7a5fb5", "#b5546e", "#5f8f4e", "#b58433", "#3f7fa5"]

    color: {
        // djb2 — pure JS, stable across runs (Qt's qHash is not).
        let h = 5381
        const a = block.address
        for (let i = 0; i < a.length; ++i)
            h = ((h << 5) + h + a.charCodeAt(i)) >>> 0
        return colors[h % colors.length]
    }

    readonly property string initials: {
        let local = block.address
        const at = local.indexOf("@")
        let second = ""
        if (at > 0) {
            second = local.charAt(at + 1)   // domain's first letter
            local = local.substring(0, at)
        }
        let s = local.substring(0, 1)
        if (local.length > 1) s += local.charAt(1)
        else if (second) s += second
        return s.toUpperCase()
    }

    Label {
        anchors.centerIn: parent
        text: block.initials
        color: "white"
        font.bold: true
        font.pixelSize: 14
    }
}

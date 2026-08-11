import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtMultimedia
import org.kde.kirigami as Kirigami

// QR scanner dialog: live camera preview with periodic frame decoding
// (ZXing via the qrCodeHelper context object), plus an "image file"
// path for QR codes displayed on this very machine (screenshots,
// browser invites) where a camera is useless.
//
// Pure UI: one semantic signal out, no controller knowledge.
Dialog {
    id: scanner
    title: "Scan QR code"
    modal: true
    standardButtons: Dialog.Close
    width: 460
    height: 520

    signal decoded(string text)

    property string statusText: "Point the camera at the QR code…"

    onOpened: camera.start()
    onClosed: camera.stop()

    CaptureSession {
        id: capture
        camera: Camera {
            id: camera
            onErrorOccurred: (code, text) => {
                scanner.statusText = "Camera unavailable: " + text
            }
        }
        videoOutput: preview
    }

    // Event-driven decode: every camera frame arrives via the sink's
    // signal. ZXing is a few ms per frame; the 250ms throttle keeps us
    // well under camera framerates.
    VideoSink {
        id: sink
        property double lastScan: 0
        // qmllint flags the (frame) parameter as extra — false positive:
        // qmltypes loses it to the QT6_ONLY macro, but moc sees it and
        // QML passes the frame at runtime.
        onVideoFrameChanged: (frame) => {
            const now = Date.now()
            if (now - lastScan < 250)
                return
            lastScan = now
            if (!frame || !frame.isValid())
                return
            const text = qrCodeHelper.decodeVideoFrame(frame)
            if (text && text.length > 0) {
                scanner.decoded(text)
                scanner.close()
            }
        }
    }
    Component.onCompleted: capture.videoSink = sink

    ColumnLayout {
        width: parent.width
        spacing: Kirigami.Units.largeSpacing

        VideoOutput {
            id: preview
            Layout.fillWidth: true
            Layout.preferredHeight: 320
            fillMode: VideoOutput.PreserveAspectCrop
        }

        Label {
            Layout.fillWidth: true
            text: scanner.statusText
            font.pixelSize: 11
            color: Kirigami.Theme.disabledTextColor
            wrapMode: Text.Wrap
        }

        Button {
            Layout.fillWidth: true
            text: "Choose image instead…"
            onClicked: fileDialog.open()
        }
    }

    FileDialog {
        id: fileDialog
        title: "Open QR image"
        nameFilters: ["Images (*.png *.jpg *.jpeg *.bmp *.webp)"]
        onAccepted: {
            const text = qrCodeHelper.decodeImageFile(fileDialog.selectedFile)
            if (text && text.length > 0) {
                scanner.decoded(text)
                scanner.close()
            } else {
                scanner.statusText = "No QR code found in that image."
            }
        }
    }
}

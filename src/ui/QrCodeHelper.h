#ifndef QRCODEHELPER_H
#define QRCODEHELPER_H

#include <QImage>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVideoFrame>

#include <ZXing/Barcode.h>
#include <ZXing/ImageView.h>
#include <ZXing/ReadBarcode.h>

/// \brief QR/barcode decoding for QML (ZXing-C++). Two input paths:
///        a still image file (screenshot of a QR shown on this machine)
///        and live camera frames handed over from a QML VideoSink.
///        Synchronous: a single-frame decode is a few ms.
///
/// Registered as the QML singleton `Aerogram.QrCodeHelper` in main.cpp.
class QrCodeHelper : public QObject
{
    Q_OBJECT

public:
    explicit QrCodeHelper(QObject *parent = nullptr) : QObject(parent) {}

    /// Decode the first barcode found in an image file. Returns the
    /// decoded text, or an empty string when nothing decodes.
    Q_INVOKABLE QString decodeImageFile(const QUrl &fileUrl) const
    {
        const QImage image(fileUrl.toLocalFile());
        if (image.isNull())
            return {};
        return decode(image);
    }

    /// Decode one camera frame (VideoSink.videoFrame in QML).
    Q_INVOKABLE QString decodeVideoFrame(const QVideoFrame &frame) const
    {
        if (!frame.isValid())
            return {};
        return decode(frame.toImage());
    }

private:
    static QString decode(QImage image)
    {
        image.convertTo(QImage::Format_Grayscale8);  // in-place; detaches from caller's copy
        ZXing::ImageView view(image.constBits(), image.width(), image.height(),
                              ZXing::ImageFormat::Lum,
                              static_cast<int>(image.bytesPerLine()));
        const auto results = ZXing::ReadBarcodes(view);
        for (const auto &r : results) {
            if (r.isValid())
                return QString::fromStdString(r.text());
        }
        return {};
    }
};

#endif

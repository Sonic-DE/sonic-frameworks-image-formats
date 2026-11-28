// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: LGPL-2.0-or-later

#ifndef KIMG_APNG_P_H
#define KIMG_APNG_P_H

#include <QImage>
#include <QImageIOPlugin>
#include <QList>
#include <QRect>

class APNGHandler : public QImageIOHandler {
public:
    bool canRead() const override;
    bool read(QImage* image) override;

    int currentImageNumber() const override;
    int imageCount() const override;
    bool jumpToImage(int imageNumber) override;
    bool jumpToNextImage() override;

    int loopCount() const override;
    int nextImageDelay() const override;

    bool supportsOption(ImageOption option) const override;
    QVariant option(ImageOption option) const override;

    //! True when the device holds a PNG with an animation control chunk before its image data.
    static bool isAnimated(QIODevice* device);

private:
    struct Frame {
        QRect rect;
        int delay = 0; // ms
        quint8 dispose = 0;
        quint8 blend = 0;
        QByteArray data; // zlib stream of the frame, from IDAT or fdAT
    };

    bool ensureParsed() const;
    bool parse();
    QImage decodeFrame(const Frame& frame) const;
    bool composeNext(QImage* output);

    bool m_parsed = false;
    bool m_valid = false;
    bool m_animated = false;
    bool m_highDepth = false;
    QByteArray m_header; // IHDR data
    QByteArray m_sharedChunks; // whole chunks before the image data that every frame needs
    QByteArray m_stillData; // the whole file, only when it isn't animated
    QSize m_size;
    int m_plays = 0;
    QList<Frame> m_frames;

    int m_currentImageNumber = 0;
    int m_nextToCompose = 0;
    QImage m_canvas;
};

class APNGPlugin : public QImageIOPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "apng.json")

public:
    Capabilities capabilities(QIODevice* device, const QByteArray& format) const override;
    QImageIOHandler* create(QIODevice* device, const QByteArray& format = QByteArray()) const override;
};

#endif // KIMG_APNG_P_H

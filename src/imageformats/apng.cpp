// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include "apng_p.h"
#include "util_p.h"

#include <QColorSpace>
#include <QIODevice>
#include <QPainter>
#include <QVariant>

#include <Sonic/Logging/Log>

#include <array>
#include <limits>

SONIC_LOGGING_COMPONENT("org.sonicde.frameworks.imageformats.apng")

namespace {
const QByteArray kSignature = QByteArrayLiteral("\x89PNG\r\n\x1a\n");
// acTL must come before the image data; a still PNG with more than this in front of it is left to Qt.
constexpr qint64 kPeekLimit = 64 * 1024;

enum : quint8 {
    DisposeNone = 0,
    DisposeBackground = 1,
    DisposePrevious = 2,
};

enum : quint8 {
    BlendSource = 0,
    BlendOver = 1,
};

quint32 readU32(const char* bytes)
{
    const auto* u = reinterpret_cast<const uchar*>(bytes);
    return (quint32(u[0]) << 24) | (quint32(u[1]) << 16) | (quint32(u[2]) << 8) | quint32(u[3]);
}

quint16 readU16(const char* bytes)
{
    const auto* u = reinterpret_cast<const uchar*>(bytes);
    return quint16((u[0] << 8) | u[1]);
}

void writeU32(char* bytes, quint32 value)
{
    bytes[0] = char(value >> 24);
    bytes[1] = char(value >> 16);
    bytes[2] = char(value >> 8);
    bytes[3] = char(value);
}

quint32 crcUpdate(quint32 crc, const char* bytes, qsizetype size)
{
    static const auto table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            t[n] = c;
        }
        return t;
    }();
    for (qsizetype i = 0; i < size; ++i) {
        crc = table[(crc ^ quint8(bytes[i])) & 0xff] ^ (crc >> 8);
    }
    return crc;
}

void appendChunk(QByteArray& png, const char* type, const QByteArray& data)
{
    char length[4];
    writeU32(length, quint32(data.size()));
    png.append(length, 4);
    png.append(type, 4);
    png.append(data);
    char crc[4];
    writeU32(crc, crcUpdate(crcUpdate(0xffffffffu, type, 4), data.constData(), data.size()) ^ 0xffffffffu);
    png.append(crc, 4);
}
}

bool APNGHandler::isAnimated(QIODevice* device)
{
    if (device == nullptr || !device->isReadable()) {
        return false;
    }
    const QByteArray head = device->peek(kPeekLimit);
    if (!head.startsWith(kSignature)) {
        return false;
    }
    qsizetype pos = kSignature.size();
    while (pos + 8 <= head.size()) {
        const quint32 length = readU32(head.constData() + pos);
        const QByteArrayView type(head.constData() + pos + 4, 4);
        if (type == "acTL") {
            return true;
        }
        if (type == "IDAT" || type == "IEND" || length > quint32(head.size())) {
            return false;
        }
        pos += 12 + qsizetype(length);
    }
    return false;
}

bool APNGHandler::ensureParsed() const
{
    if (!m_parsed) {
        auto* self = const_cast<APNGHandler*>(this);
        self->m_valid = self->parse();
        self->m_parsed = true;
    }
    return m_valid;
}

bool APNGHandler::parse()
{
    if (device() == nullptr) {
        return false;
    }
    const QByteArray data = deviceRead(device(), kMaxQVectorSize);
    if (!data.startsWith(kSignature)) {
        sonicWarning() << "Not a PNG file";
        return false;
    }

    // The file without its animation chunks: what Qt's PNG reader shows when nothing plays.
    QByteArray still = kSignature;
    quint32 frameCount = 0;
    quint32 nextSequence = 0;
    bool seenImageData = false;
    bool imageDataIsFrame = false;
    bool stop = false;

    const char* bytes = data.constData();
    qsizetype pos = kSignature.size();
    while (!stop && pos + 12 <= data.size()) {
        const quint32 length = readU32(bytes + pos);
        if (length > quint32(std::numeric_limits<qint32>::max()) || qint64(length) > data.size() - pos - 12) {
            sonicWarning() << "PNG chunk runs past the end of the file";
            break;
        }
        const QByteArrayView type(bytes + pos + 4, 4);
        const char* body = bytes + pos + 8;
        const qsizetype chunkSize = 12 + qsizetype(length);

        if (type == "acTL") {
            if (!seenImageData && length == 8 && !m_header.isEmpty()) {
                frameCount = readU32(body);
                m_plays = int(std::min(readU32(body + 4), quint32(std::numeric_limits<int>::max())));
                m_animated = frameCount > 0;
            }
        } else if (type == "fcTL") {
            if (m_animated) {
                if (length != 26 || readU32(body) != nextSequence) {
                    sonicWarning() << "APNG frame control chunk is out of order or malformed";
                    stop = true;
                    continue;
                }
                ++nextSequence;
                Frame frame;
                const qint64 width = readU32(body + 4);
                const qint64 height = readU32(body + 8);
                const qint64 x = readU32(body + 12);
                const qint64 y = readU32(body + 16);
                const int numerator = readU16(body + 20);
                const int denominator = readU16(body + 22) ? readU16(body + 22) : 100;
                frame.dispose = quint8(body[24]);
                frame.blend = quint8(body[25]);
                if (width <= 0 || height <= 0 || x + width > m_size.width() || y + height > m_size.height() || frame.dispose > DisposePrevious
                    || frame.blend > BlendOver) {
                    sonicWarning() << "APNG frame" << m_frames.size() << "lies outside the image or has unknown options";
                    stop = true;
                    continue;
                }
                frame.rect = QRect(int(x), int(y), int(width), int(height));
                frame.delay = (numerator * 1000 + denominator / 2) / denominator;
                if (m_frames.isEmpty() && frame.dispose == DisposePrevious) {
                    frame.dispose = DisposeBackground; // what the specification asks for on the first frame
                }
                m_frames.append(frame);
                imageDataIsFrame = !seenImageData;
            }
        } else if (type == "fdAT") {
            const bool ownsFrame = m_animated && !m_frames.isEmpty() && !(imageDataIsFrame && m_frames.size() == 1);
            if (ownsFrame) {
                if (length < 4 || readU32(body) != nextSequence) {
                    sonicWarning() << "APNG frame data chunk is out of order or malformed";
                    stop = true;
                    continue;
                }
                ++nextSequence;
                m_frames.last().data.append(body + 4, length - 4);
            }
        } else {
            if (type == "IHDR") {
                if (length != 13 || !m_header.isEmpty()) {
                    sonicWarning() << "PNG header chunk is malformed";
                    return false;
                }
                m_header = QByteArray(body, 13);
                m_size = QSize(int(std::min(readU32(body), quint32(std::numeric_limits<int>::max()))),
                    int(std::min(readU32(body + 4), quint32(std::numeric_limits<int>::max()))));
                m_highDepth = quint8(body[8]) == 16;
            } else if (type == "IDAT") {
                seenImageData = true;
                if (m_animated && imageDataIsFrame && m_frames.size() == 1) {
                    m_frames.first().data.append(body, length);
                }
            } else if (!seenImageData && type != "IEND") {
                m_sharedChunks.append(bytes + pos, chunkSize);
            }
            still.append(bytes + pos, chunkSize);
            if (type == "IEND") {
                break;
            }
        }
        pos += chunkSize;
    }

    if (m_header.isEmpty() || m_size.isEmpty()) {
        sonicWarning() << "PNG file has no usable header";
        return false;
    }
    if (m_animated) {
        while (!m_frames.isEmpty() && m_frames.last().data.isEmpty()) {
            m_frames.removeLast();
        }
        if (m_frames.size() > qsizetype(frameCount)) {
            m_frames.resize(frameCount);
        }
        m_animated = !m_frames.isEmpty();
    }
    if (!m_animated) {
        m_frames.clear();
        m_stillData = still;
    }
    return true;
}

QImage APNGHandler::decodeFrame(const Frame& frame) const
{
    QByteArray header = m_header;
    writeU32(header.data(), quint32(frame.rect.width()));
    writeU32(header.data() + 4, quint32(frame.rect.height()));

    QByteArray png = kSignature;
    appendChunk(png, "IHDR", header);
    png.append(m_sharedChunks);
    appendChunk(png, "IDAT", frame.data);
    appendChunk(png, "IEND", QByteArray());

    QImage image;
    image.loadFromData(png, "PNG");
    return image;
}

bool APNGHandler::composeNext(QImage* output)
{
    const Frame& frame = m_frames.at(m_nextToCompose);
    if (m_canvas.isNull()) {
        const auto format = m_highDepth ? QImage::Format_RGBA64_Premultiplied : QImage::Format_ARGB32_Premultiplied;
        if (!QImageIOHandler::allocateImage(m_size, format, &m_canvas)) {
            sonicWarning() << "APNG image is too large:" << m_size;
            return false;
        }
        m_canvas.fill(Qt::transparent);
    }

    const QImage image = decodeFrame(frame);
    if (image.isNull() || image.size() != frame.rect.size()) {
        sonicWarning() << "APNG frame" << m_nextToCompose << "can't be decoded";
        return false;
    }
    if (!m_canvas.colorSpace().isValid() && image.colorSpace().isValid()) {
        m_canvas.setColorSpace(image.colorSpace());
    }

    const QImage previous = frame.dispose == DisposePrevious ? m_canvas.copy(frame.rect) : QImage();
    {
        QPainter painter(&m_canvas);
        painter.setCompositionMode(frame.blend == BlendOver ? QPainter::CompositionMode_SourceOver : QPainter::CompositionMode_Source);
        painter.drawImage(frame.rect.topLeft(), image);
    }
    if (output != nullptr) {
        *output = m_canvas.convertToFormat(m_highDepth ? QImage::Format_RGBA64 : QImage::Format_ARGB32);
    }
    if (frame.dispose != DisposeNone) {
        QPainter painter(&m_canvas);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        if (frame.dispose == DisposeBackground) {
            painter.fillRect(frame.rect, Qt::transparent);
        } else {
            painter.drawImage(frame.rect.topLeft(), previous);
        }
    }
    ++m_nextToCompose;
    return true;
}

bool APNGHandler::canRead() const
{
    if (!m_parsed) {
        if (isAnimated(device()) || (format() == "apng" && device() != nullptr && device()->peek(kSignature.size()) == kSignature)) {
            setFormat("apng");
            return true;
        }
        return false;
    }
    return m_valid && m_currentImageNumber < imageCount();
}

bool APNGHandler::read(QImage* image)
{
    if (!ensureParsed() || m_currentImageNumber >= imageCount()) {
        return false;
    }
    if (!m_animated) {
        QImage still;
        if (!still.loadFromData(m_stillData, "PNG")) {
            return false;
        }
        *image = still;
        ++m_currentImageNumber;
        return true;
    }

    if (m_nextToCompose > m_currentImageNumber) {
        m_canvas = QImage();
        m_nextToCompose = 0;
    }
    while (m_nextToCompose < m_currentImageNumber) {
        if (!composeNext(nullptr)) {
            return false;
        }
    }
    if (!composeNext(image)) {
        return false;
    }
    ++m_currentImageNumber;
    return true;
}

int APNGHandler::currentImageNumber() const
{
    return ensureParsed() ? m_currentImageNumber : 0;
}

int APNGHandler::imageCount() const
{
    if (!ensureParsed()) {
        return 0;
    }
    return m_animated ? int(m_frames.size()) : 1;
}

bool APNGHandler::jumpToImage(int imageNumber)
{
    if (imageNumber < 0 || imageNumber >= imageCount()) {
        return false;
    }
    m_currentImageNumber = imageNumber;
    return true;
}

bool APNGHandler::jumpToNextImage()
{
    return jumpToImage(m_currentImageNumber + 1);
}

int APNGHandler::loopCount() const
{
    if (!ensureParsed() || !m_animated) {
        return 0;
    }
    // APNG counts plays (0 = forever); Qt counts repeats after the first play (-1 = forever).
    return m_plays == 0 ? -1 : m_plays - 1;
}

int APNGHandler::nextImageDelay() const
{
    if (!ensureParsed() || !m_animated) {
        return 0;
    }
    // The delay of the image read last, as QMovie expects.
    return m_frames.at(std::clamp(m_currentImageNumber - 1, 0, int(m_frames.size()) - 1)).delay;
}

bool APNGHandler::supportsOption(ImageOption option) const
{
    return option == Size || option == ImageFormat || option == Animation;
}

QVariant APNGHandler::option(ImageOption option) const
{
    if (!supportsOption(option) || !ensureParsed()) {
        return QVariant();
    }
    switch (option) {
    case Size:
        return m_size;
    case ImageFormat:
        return m_highDepth ? QImage::Format_RGBA64 : QImage::Format_ARGB32;
    case Animation:
        return m_animated;
    default:
        return QVariant();
    }
}

QImageIOPlugin::Capabilities APNGPlugin::capabilities(QIODevice* device, const QByteArray& format) const
{
    if (format == "apng") {
        return Capabilities(CanRead);
    }
    // Registered for "png" too, but only animated files are claimed; Qt reads every still PNG as before.
    if (!format.isEmpty() && format != "png") {
        return {};
    }
    if (device == nullptr || !device->isOpen()) {
        return {};
    }
    return APNGHandler::isAnimated(device) ? Capabilities(CanRead) : Capabilities();
}

QImageIOHandler* APNGPlugin::create(QIODevice* device, const QByteArray& format) const
{
    QImageIOHandler* handler = new APNGHandler;
    handler->setDevice(device);
    handler->setFormat(format);
    return handler;
}

#include "moc_apng_p.cpp"

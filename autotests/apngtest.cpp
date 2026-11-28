// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QMovie>
#include <QTemporaryDir>
#include <QTest>

namespace {
// QPainter and ffmpeg may round half-transparent blends one step apart.
bool fuzzyEqual(const QImage& actual, const QImage& expected)
{
    const QImage a = actual.convertToFormat(QImage::Format_ARGB32);
    const QImage e = expected.convertToFormat(QImage::Format_ARGB32);
    if (a.size() != e.size()) {
        return false;
    }
    for (int y = 0; y < a.height(); ++y) {
        const auto* la = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* le = reinterpret_cast<const QRgb*>(e.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (qAbs(qRed(la[x]) - qRed(le[x])) > 1 || qAbs(qGreen(la[x]) - qGreen(le[x])) > 1 || qAbs(qBlue(la[x]) - qBlue(le[x])) > 1
                || qAbs(qAlpha(la[x]) - qAlpha(le[x])) > 1) {
                qWarning() << "pixel" << x << y << Qt::hex << la[x] << "expected" << le[x];
                return false;
            }
        }
    }
    return true;
}

QString fixture(const QString& name)
{
    return QFINDTESTDATA(QStringLiteral("apng/") + name);
}

QImage reference(const QString& name, int frame)
{
    return QImage(fixture(QStringLiteral("%1_%2.png").arg(name).arg(frame)));
}
}

class APNGTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QCoreApplication::removeLibraryPath(QStringLiteral(PLUGIN_DIR));
        QCoreApplication::addLibraryPath(QStringLiteral(PLUGIN_DIR));
    }

    void testFrames_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("frames");
        QTest::addColumn<int>("loops");

        QTest::newRow("basic") << QStringLiteral("basic") << 3 << -1;
        QTest::newRow("still fallback outside the animation") << QStringLiteral("fallback") << 2 << -1;
        QTest::newRow("dispose to background") << QStringLiteral("dispose_background") << 3 << -1;
        QTest::newRow("dispose to previous") << QStringLiteral("dispose_previous") << 3 << -1;
        QTest::newRow("blend over") << QStringLiteral("blend_over") << 3 << -1;
        QTest::newRow("palette, three plays") << QStringLiteral("palette_loop3") << 3 << 2;
    }

    void testFrames()
    {
        QFETCH(QString, name);
        QFETCH(int, frames);
        QFETCH(int, loops);

        QImageReader reader(fixture(name + QStringLiteral(".apng")));
        QVERIFY(reader.canRead());
        QCOMPARE(reader.format(), QByteArrayLiteral("apng"));
        QVERIFY(reader.supportsAnimation());
        QCOMPARE(reader.imageCount(), frames);
        QCOMPARE(reader.loopCount(), loops);
        QCOMPARE(reader.size(), QSize(32, 32));

        for (int i = 0; i < frames; ++i) {
            QCOMPARE(reader.currentImageNumber(), i);
            QImage frame;
            QVERIFY2(reader.read(&frame), qPrintable(reader.errorString()));
            QVERIFY2(fuzzyEqual(frame, reference(name, i)), qPrintable(QStringLiteral("frame %1").arg(i)));
        }
        QImage past;
        QVERIFY(!reader.read(&past));
    }

    void testDelays()
    {
        QImageReader reader(fixture(QStringLiteral("basic.apng")));
        const QList<int> expected{100, 200, 300};
        for (int delay : expected) {
            QImage frame;
            QVERIFY(reader.read(&frame));
            QCOMPARE(reader.nextImageDelay(), delay);
        }
    }

    void testJumpBack()
    {
        // Going back recomposes from the first frame, so earlier disposal is redone.
        QImageReader reader(fixture(QStringLiteral("dispose_previous.apng")));
        QImage frame;
        for (int i = 0; i < 3; ++i) {
            QVERIFY(reader.read(&frame));
        }
        QVERIFY(reader.jumpToImage(1));
        QVERIFY(reader.read(&frame));
        QVERIFY(fuzzyEqual(frame, reference(QStringLiteral("dispose_previous"), 1)));
    }

    void testPngSuffix()
    {
        // Animated PNGs usually keep the .png suffix; they still play.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString copy = dir.filePath(QStringLiteral("basic.png"));
        QVERIFY(QFile::copy(fixture(QStringLiteral("basic.apng")), copy));

        QImageReader reader(copy);
        QCOMPARE(reader.format(), QByteArrayLiteral("apng"));
        QCOMPARE(reader.imageCount(), 3);
    }

    void testStillPngLeftToQt()
    {
        QImageReader reader(fixture(QStringLiteral("basic_0.png")));
        QCOMPARE(reader.format(), QByteArrayLiteral("png"));
        QImage image;
        QVERIFY(reader.read(&image));
        QVERIFY(fuzzyEqual(image, reference(QStringLiteral("basic"), 0)));
    }

    void testMovie()
    {
        QMovie movie(fixture(QStringLiteral("dispose_background.apng")));
        QVERIFY(movie.isValid());
        QCOMPARE(movie.frameCount(), 3);
        QVERIFY(movie.jumpToFrame(2));
        QVERIFY(fuzzyEqual(movie.currentImage(), reference(QStringLiteral("dispose_background"), 2)));
    }

    void testDamagedFiles_data()
    {
        QTest::addColumn<int>("cut");

        QFile file(fixture(QStringLiteral("basic.apng")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const qint64 size = file.size();
        for (qint64 cut = 8; cut < size; cut += 13) {
            QTest::addRow("cut at %lld", cut) << int(cut);
        }
    }

    void testDamagedFiles()
    {
        // A truncated file must never crash or report more frames than it holds.
        QFETCH(int, cut);
        QFile file(fixture(QStringLiteral("basic.apng")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray data = file.readAll().left(cut);

        QBuffer buffer(&data);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer, "apng");
        QVERIFY(reader.imageCount() <= 3);
        QImage frame;
        int read = 0;
        while (read < 4 && reader.read(&frame)) {
            ++read;
        }
        QVERIFY(read <= 3);
    }

    void testCorruptFrameData()
    {
        QFile file(fixture(QStringLiteral("basic.apng")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray data = file.readAll();
        const qsizetype fdat = data.indexOf("fdAT");
        QVERIFY(fdat > 0);
        for (qsizetype i = fdat + 8; i < fdat + 20; ++i) {
            data[i] = char(~data[i]);
        }

        QBuffer buffer(&data);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer, "apng");
        QImage frame;
        QVERIFY(reader.read(&frame));
        QVERIFY(!reader.read(&frame));
    }
};

QTEST_MAIN(APNGTests)

#include "apngtest.moc"

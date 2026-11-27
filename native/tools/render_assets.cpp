#include <QGuiApplication>
#include <QSvgRenderer>
#include <QPainter>
#include <QDir>
#include <QFile>
#include <QBuffer>
#include <QtEndian>
#include <iostream>

static void le16(QByteArray &a, quint16 v) { char b[2]; qToLittleEndian(v,b); a.append(b,2); }
static void le32(QByteArray &a, quint32 v) { char b[4]; qToLittleEndian(v,b); a.append(b,4); }
static void be32(QByteArray &a, quint32 v) { char b[4]; qToBigEndian(v,b); a.append(b,4); }
static bool write(const QString &path, const QByteArray &data) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size(); }
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--preview") {
        QSvgRenderer document(QString::fromLocal8Bit(argv[2]));
        if (!document.isValid()) return 1;
        const auto size = document.defaultSize().scaled(1200, 900, Qt::KeepAspectRatio);
        QImage image(size, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        QPainter painter(&image); document.render(&painter); painter.end();
        return image.save(QString::fromLocal8Bit(argv[3]), "PNG") ? 0 : 1;
    }
    if (argc != 3) return 2;
    QSvgRenderer svg(QString::fromLocal8Bit(argv[1]));
    if (!svg.isValid()) return 1;
    QDir dir(QString::fromLocal8Bit(argv[2])); QDir().mkpath(dir.path());
    QList<QByteArray> pngs;
    const QList<int> sizes{16,32,48,64,128,256,512,1024};
    for (int size : sizes) {
        QImage image(size,size,QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        QPainter p(&image); svg.render(&p); p.end();
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer,"PNG") || !write(dir.filePath(QString("uav-studio-%1.png").arg(size)),png)) return 1;
        pngs.append(png);
    }
    // ICO 使用原生支持的多尺寸 PNG 帧，ICNS 使用标准 PNG 类型块。
    QByteArray ico; le16(ico,0); le16(ico,1); le16(ico,6);
    quint32 offset=6+16*6;
    for (int i=0;i<6;++i) {
        ico.append(char(sizes[i] == 256 ? 0 : sizes[i])); ico.append(ico.back());
        ico.append(char(0)); ico.append(char(0)); le16(ico,1); le16(ico,32);
        le32(ico,pngs[i].size()); le32(ico,offset); offset+=pngs[i].size();
    }
    for (int i=0;i<6;++i) ico.append(pngs[i]);
    QByteArray blocks;
    const QList<QByteArray> types{"icp4","icp5","", "icp6","ic07","ic08","ic09","ic10"};
    for (int i=0;i<sizes.size();++i) if (!types[i].isEmpty()) { blocks.append(types[i]); be32(blocks,8+pngs[i].size()); blocks.append(pngs[i]); }
    QByteArray icns("icns"); be32(icns,8+blocks.size()); icns.append(blocks);
    return write(dir.filePath("uav-studio.ico"),ico) && write(dir.filePath("uav-studio.icns"),icns) ? 0 : 1;
}

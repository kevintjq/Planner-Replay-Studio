#include "canvas_layer_state.h"
#include "canvas_scientific_item.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QPdfWriter>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSvgGenerator>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QThread>
#include <qqml.h>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
bool close(QColor a,QColor b){return std::abs(a.red()-b.red())<=3&&std::abs(a.green()-b.green())<=3&&std::abs(a.blue()-b.blue())<=3&&std::abs(a.alpha()-b.alpha())<=3;}
QImage capture(QQuickItem *item)
{
    const auto grab=item->grabToImage();check(bool(grab),"Cannot request actual QtQuick scientific item capture");QElapsedTimer timer;timer.start();QImage image;
    while(timer.elapsed()<3000&&image.isNull()){QCoreApplication::processEvents();QThread::msleep(5);image=grab->image();}
    check(!image.isNull(),"Actual QtQuick scientific item did not render");return image;
}
QImage paintText(const QVariantMap &element,const QSize &size,double opacity=1.)
{
    QImage image(size,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);QPainter painter(&image);painter.setOpacity(opacity);
    check(replay::paintCanvasElement(painter,element,size),"Cannot paint real shared scientific text");return image;
}
double coverageDifference(const QImage &a,const QImage &b)
{
    check(a.size()==b.size(),"Cannot compare different image dimensions");double difference=0.,total=0.;
    for(int y=0;y<a.height();++y)for(int x=0;x<a.width();++x){const auto aa=a.pixelColor(x,y).alpha(),bb=b.pixelColor(x,y).alpha();difference+=std::abs(aa-bb);total+=bb;}
    return difference/std::max(1.,total);
}
double coverageOutside(const QImage &a,const QImage &b)
{
    double outside=0.,total=0.;for(int y=0;y<a.height();++y)for(int x=0;x<a.width();++x){
        const auto alpha=a.pixelColor(x,y).alpha();if(!alpha)continue;int neighbor=0;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)if(b.valid(x+dx,y+dy))neighbor=std::max(neighbor,b.pixelColor(x+dx,y+dy).alpha());
        outside+=std::max(0,alpha-neighbor);total+=alpha;
    }return outside/std::max(1.,total);
}
QByteArray bytes(const QString &path){QFile file(path);check(file.open(QIODevice::ReadOnly),"Cannot inspect actual text export file");return file.readAll();}
}
int main(int argc,char **argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);QQuickWindow::setDefaultAlphaBuffer(true);QGuiApplication app(argc,argv);QTemporaryDir temporary;
    try{
#ifdef Q_OS_WIN
        check(QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf")>=0,"Cannot load actual UI font");
        check(QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc")>=0,"Cannot load actual Chinese font");
#endif
        const auto output=argc>1?QString::fromLocal8Bit(argv[1]):temporary.path();check(temporary.isValid()&&QDir().mkpath(output),"Cannot create appearance artifact directory");
        qmlRegisterType<CanvasScientificItem>("UAVStudio",1,0,"CanvasScientificItem");
        QQmlEngine engine;
        const auto sourceDirectory=QFileInfo(QString::fromLocal8Bit(__FILE__)).absoluteDir();
        for(const auto &name:{"ScientificElementInspector.qml","ScientificLayers.qml"}){
            QQmlComponent source(&engine,QUrl::fromLocalFile(sourceDirectory.filePath(QString("../qml/")+name)));
            check(source.isReady(),qPrintable(source.errorString()));
        }
        QQmlComponent component(&engine);component.setData(R"qml(
import QtQuick
Item {
    width: 200; height: 120
    property color frameBorderColor: "#224466"
    property color frameColor: "#cc3355"
    property real frameBorderWidth: 6
    property real frameOpacity: .4
    Item {
        anchors.fill: parent; opacity: .7
        Rectangle {
            anchors.fill: parent; color: parent.parent.frameColor; opacity: parent.parent.frameOpacity
            radius: 20; border.width: parent.parent.frameBorderWidth; border.color: parent.parent.frameBorderColor
        }
        Rectangle { x: 80; y: 50; width: 40; height: 20; color: "white" }
    }
})qml",QUrl());
        check(component.isReady(),qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());auto *item=qobject_cast<QQuickItem *>(object.get());check(item,"Cannot instantiate actual QtQuick Rectangle appearance fixture");
        QQuickWindow window;window.setColor(Qt::transparent);window.setGeometry(0,0,200,120);item->setParentItem(window.contentItem());window.show();
        QCoreApplication::processEvents();const auto grab=item->grabToImage();check(bool(grab),"Cannot request actual QtQuick item capture");
        QElapsedTimer timer;timer.start();QImage qml;
        while(timer.elapsed()<3000&&qml.isNull()){QCoreApplication::processEvents();QThread::msleep(5);qml=grab->image();}
        check(!qml.isNull()&&qml.size()==QSize(200,120),"Actual QtQuick software renderer did not produce appearance pixels");
        QImage cpu(200,120,QImage::Format_ARGB32_Premultiplied);cpu.fill(Qt::transparent);
        const QVariantMap settings{{"opacity",.7},{"backgroundMode","solid"},{"backgroundColor","#cc3355"},{"backgroundOpacity",.4},{"cornerRadius",20.},{"borderWidth",6.},{"borderColor","#224466"}};
        {QPainter painter(&cpu);replay::applyCanvasItemTransform(painter,settings,QRectF(0,0,200,120));replay::paintCanvasItemAppearance(painter,settings,cpu.size());painter.fillRect(80,50,40,20,Qt::white);}
        for(const QPoint point:{QPoint(0,0),QPoint(2,60),QPoint(100,2),QPoint(197,60),QPoint(30,30),QPoint(100,100),QPoint(100,60)}){
            if(!close(qml.pixelColor(point),cpu.pixelColor(point))){std::cerr<<"Appearance mismatch at "<<point.x()<<','<<point.y()<<": QML "<<qml.pixelColor(point).name(QColor::HexArgb).toStdString()<<" CPU "<<cpu.pixelColor(point).name(QColor::HexArgb).toStdString()<<'\n';throw std::runtime_error("Canvas frame alpha/border/rounding differs between QML and CPU");}
        }
        check(cpu.pixelColor(0,0).alpha()==0&&std::abs(cpu.pixelColor(30,30).alpha()-71)<=2&&std::abs(cpu.pixelColor(100,60).alpha()-200)<=2,"Background opacity affected independent content opacity or corner transparency");
        check(qml.save(QDir(output).filePath("appearance-qml.png"))&&cpu.save(QDir(output).filePath("appearance-cpu.png")),"Cannot save actual QML and painter comparison images");
        for(const auto &variant:QVariantList{QVariantMap{{"borderColor","transparent"}},QVariantMap{{"borderWidth",0.}},QVariantMap{{"backgroundOpacity",0.}},QVariantMap{{"backgroundMode","transparent"}}}){
            auto changed=settings;const auto patch=variant.toMap();for(auto i=patch.cbegin();i!=patch.cend();++i)changed[i.key()]=i.value();
            item->setProperty("frameBorderColor",changed.value("borderColor"));item->setProperty("frameBorderWidth",changed.value("borderWidth"));
            item->setProperty("frameOpacity",changed.value("backgroundOpacity"));item->setProperty("frameColor",changed.value("backgroundMode")=="transparent"?QVariant("transparent"):changed.value("backgroundColor"));
            const auto another=item->grabToImage();check(bool(another),"Cannot capture another actual QtQuick appearance state");timer.restart();QImage actual;
            while(timer.elapsed()<3000&&actual.isNull()){QCoreApplication::processEvents();QThread::msleep(5);actual=another->image();}check(!actual.isNull(),"Changed QtQuick appearance did not render");
            QImage expected(200,120,QImage::Format_ARGB32_Premultiplied);expected.fill(Qt::transparent);{QPainter painter(&expected);replay::applyCanvasItemTransform(painter,changed,QRectF(0,0,200,120));replay::paintCanvasItemAppearance(painter,changed,expected.size());painter.fillRect(80,50,40,20,Qt::white);}
            for(const QPoint point:{QPoint(0,0),QPoint(2,60),QPoint(100,2),QPoint(30,30),QPoint(100,60)})if(!close(actual.pixelColor(point),expected.pixelColor(point))){
                std::cerr<<"Variant "<<patch.firstKey().toStdString()<<" at "<<point.x()<<','<<point.y()<<": QML "<<actual.pixelColor(point).name(QColor::HexArgb).toStdString()<<" CPU "<<expected.pixelColor(point).name(QColor::HexArgb).toStdString()<<'\n';throw std::runtime_error("Transparent/absent border or background differs between QML and CPU");}
        }
        const auto svgPath=QDir(output).filePath("appearance-vector.svg");QSvgGenerator svg;svg.setFileName(svgPath);svg.setSize({200,120});svg.setViewBox(QRect(0,0,200,120));
        {QPainter painter(&svg);replay::applyCanvasItemTransform(painter,settings,QRectF(0,0,200,120));replay::paintCanvasItemAppearance(painter,settings,{200,120});}
        QFile svgFile(svgPath);check(svgFile.open(QIODevice::ReadOnly),"Cannot inspect appearance SVG");const auto xml=svgFile.readAll();check(!xml.contains("<image")&&xml.contains("<path"),"Appearance SVG was rasterized instead of native vector paths");
        QImage empty(200,120,QImage::Format_ARGB32_Premultiplied);empty.fill(Qt::transparent);{QPainter painter(&empty);replay::paintCanvasItemAppearance(painter,{},empty.size());}
        check(empty.pixelColor(100,60).alpha()==0,"Default appearance added an opaque panel or watermark");
        item->setParentItem(nullptr);
        const QSize textSize(600,260);auto text=replay::canvasElementDefaults("text");text["width"]=.6;text["height"]=.26;text["designWidth"]=1000.;
        text["text"]=QString::fromUtf8("Flight ffi 42 · 实验\n轨迹对比 · Καλημέρα");auto textStyle=text.value("style").toMap();
        textStyle["fontSize"]=40.;textStyle["alignment"]="center";textStyle["color"]="#e64c42";text["style"]=textStyle;
        const auto plain=paintText(text,textSize);QImage legacy(textSize,QImage::Format_ARGB32_Premultiplied);legacy.fill(Qt::transparent);
        QFont font("Segoe UI");font.setPixelSize(40);font.setWeight(QFont::Normal);const auto textFlags=int(Qt::TextWordWrap|Qt::AlignHCenter|Qt::AlignVCenter);
        {QPainter painter(&legacy);painter.setRenderHints(QPainter::Antialiasing|QPainter::TextAntialiasing|QPainter::SmoothPixmapTransform);painter.setFont(font);painter.setPen(QColor("#e64c42"));painter.drawText(QRectF(QPointF(),textSize),textFlags,text.value("text").toString());}
        check(plain==legacy,"Transparent default text outline/shadow changed old text pixels");
        // The glyph outline must track Qt's shaped/wrapped/bilingual drawText,
        // not a per-character approximation which loses fallback or ligatures.
        auto hard=text;auto hardStyle=textStyle;hardStyle["color"]="transparent";hardStyle["textShadowColor"]="#224466";hardStyle["textShadowOpacity"]=.6;hardStyle["textShadowOffsetX"]=12.;hardStyle["textShadowOffsetY"]=10.;hardStyle["textShadowBlur"]=0.;hard["style"]=hardStyle;
        const auto hardImage=paintText(hard,textSize);QImage expectedHard(textSize,QImage::Format_ARGB32_Premultiplied);expectedHard.fill(Qt::transparent);
        {QPainter painter(&expectedHard);painter.setRenderHint(QPainter::TextAntialiasing);painter.setFont(font);auto tint=QColor("#224466");tint.setAlphaF(.6);painter.setPen(tint);painter.drawText(QRectF(12,10,588,250),textFlags,text.value("text").toString());}
        const auto hardDifference=coverageDifference(hardImage,expectedHard);std::cout<<"Shaped glyph shadow versus actual Qt drawText alpha difference: "<<hardDifference<<'\n';
        check(hardImage.save(QDir(output).filePath("text-hard-shadow.png"))&&expectedHard.save(QDir(output).filePath("text-hard-expected.png")),"Cannot inspect shaped glyph shadow alignment");
        // Outline paths are unhinted while drawText uses raster font hinting.
        // Allow that one-pixel edge difference, while still detecting shifted
        // baselines, broken shaping, wrong fallback or lost multiline layout.
        const auto hardOutside=std::max(coverageOutside(hardImage,expectedHard),coverageOutside(expectedHard,hardImage));std::cout<<"Glyph coverage outside one-pixel raster hinting tolerance: "<<hardOutside<<'\n';
        check(hardDifference<.35&&hardOutside<.03,"Vector shadow glyphs do not align with actual shaped, wrapped Qt text");
        hardStyle["textShadowOffsetX"]=-12.;hardStyle["textShadowOffsetY"]=-10.;hard["style"]=hardStyle;const auto negativeShadow=paintText(hard,textSize);expectedHard.fill(Qt::transparent);
        {QPainter painter(&expectedHard);painter.setRenderHint(QPainter::TextAntialiasing);painter.setFont(font);auto tint=QColor("#224466");tint.setAlphaF(.6);painter.setPen(tint);painter.drawText(QRectF(0,0,588,250),textFlags,text.value("text").toString());}
        check(std::max(coverageOutside(negativeShadow,expectedHard),coverageOutside(expectedHard,negativeShadow))<.03,"Negative shadow offsets changed text layout or clipped the editable effect");
        auto styled=text;auto styledStyle=textStyle;styledStyle["textStrokeColor"]="#164dcc";styledStyle["textStrokeWidth"]=6.;styledStyle["textShadowColor"]="#101828";styledStyle["textShadowOpacity"]=.55;styledStyle["textShadowOffsetX"]=8.;styledStyle["textShadowOffsetY"]=10.;styledStyle["textShadowBlur"]=6.;styled["style"]=styledStyle;
        CanvasScientificItem preview;preview.setParentItem(window.contentItem());preview.setWidth(textSize.width());preview.setHeight(textSize.height());preview.setDataMap(styled);
        window.setGeometry(0,0,textSize.width(),textSize.height());QCoreApplication::processEvents();
        for(const double opacity:{1.,.55}){
            preview.setContentOpacity(opacity);const auto actual=capture(&preview),expected=paintText(styled,textSize,opacity);check(actual.size()==expected.size(),"Actual scientific preview size changed");
            for(int y=0;y<actual.height();++y)for(int x=0;x<actual.width();++x)check(close(actual.pixelColor(x,y),expected.pixelColor(x,y)),"Actual CanvasScientificItem outline/shadow overlap differs from CPU export");
            check(actual.pixelColor(0,0).alpha()==0&&actual.pixelColor(599,259).alpha()==0,"Text effects added a canvas panel or watermark");
            if(opacity<1.)check(actual.save(QDir(output).filePath("text-effects-preview.png"))&&expected.save(QDir(output).filePath("text-effects-cpu.png")),"Cannot save real scientific preview and CPU comparison");
        }
        const auto softImage=paintText(styled,textSize,.55);
        for(const double scale:{.5,2.,4.}){
            const auto scaled=paintText(styled,QSize(std::lround(textSize.width()*scale),std::lround(textSize.height()*scale)),.55);
            check(scaled.save(QDir(output).filePath(QString("text-effects-%1x.png").arg(scale))),"Cannot inspect scaled text effect output");
            const auto reference=scale<1.?softImage.scaled(scaled.size(),Qt::IgnoreAspectRatio,Qt::SmoothTransformation):softImage;
            const auto actual=scale<1.?scaled:scaled.scaled(textSize,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
            const auto difference=coverageDifference(actual,reference);
            std::cout<<"Text outline/shadow scale "<<scale<<" alpha difference: "<<difference<<'\n';
            check(difference<.16,"High-resolution text outline/shadow scale is not proportional");
        }
        // A transparent text fill exposes the soft shadow. Its kernel must
        // contain nonzero partial coverage and respect inherited opacity.
        auto shadowOnly=styled;auto shadowOnlyStyle=styledStyle;shadowOnlyStyle["color"]="transparent";shadowOnlyStyle["textStrokeWidth"]=0.;shadowOnly["style"]=shadowOnlyStyle;
        const auto shadowPixels=paintText(shadowOnly,textSize,.55);int maximumAlpha=0,partialPixels=0;
        for(int y=0;y<shadowPixels.height();++y)for(int x=0;x<shadowPixels.width();++x){const auto alpha=shadowPixels.pixelColor(x,y).alpha();maximumAlpha=std::max(maximumAlpha,alpha);if(alpha>0&&alpha<60)++partialPixels;}
        check(maximumAlpha>55&&maximumAlpha<=80&&partialPixels>100,"Soft vector shadow lacks bounded partial alpha or ignores overall layer opacity");
        const auto textSvgPath=QDir(output).filePath("text-effects-vector.svg");QSvgGenerator textSvg;textSvg.setFileName(textSvgPath);textSvg.setSize(textSize);textSvg.setViewBox(QRect(QPoint(),textSize));
        {QPainter painter(&textSvg);painter.setOpacity(.55);check(replay::paintCanvasElement(painter,styled,textSize),"Cannot write styled text SVG");}
        const auto textXml=bytes(textSvgPath);check(!textXml.contains("<image")&&textXml.contains("<path"),"Text effects SVG lost native glyph paths or embedded a screenshot");
        QSvgRenderer svgRenderer(textXml);check(svgRenderer.isValid(),"Generated text SVG cannot be reopened");QImage svgPixels(textSize,QImage::Format_ARGB32_Premultiplied);svgPixels.fill(Qt::transparent);{QPainter painter(&svgPixels);svgRenderer.render(&painter);}
        const auto svgDifference=coverageDifference(svgPixels,softImage);std::cout<<"Reopened vector SVG versus CPU alpha difference: "<<svgDifference<<'\n';check(svgDifference<.16,"Actual reopened SVG text effect differs from shared CPU geometry/opacity");
        check(svgPixels.save(QDir(output).filePath("text-effects-svg-rendered.png")),"Cannot inspect reopened SVG pixels");
        const auto textPdfPath=QDir(output).filePath("text-effects-vector.pdf");
        {QPdfWriter pdf(textPdfPath);pdf.setResolution(300);pdf.setPageSize(QPageSize(QSizeF(600.*25.4/300.,260.*25.4/300.),QPageSize::Millimeter));pdf.setPageMargins(QMarginsF());QPainter painter(&pdf);painter.setOpacity(.55);check(replay::paintCanvasElement(painter,styled,textSize),"Cannot write styled text PDF");}
        const auto pdfBytes=bytes(textPdfPath);check(pdfBytes.startsWith("%PDF-")&&!pdfBytes.contains("/Subtype /Image"),"Text effects PDF was flattened to an image");
        preview.setParentItem(nullptr);window.hide();
        std::cout<<"Actual QtQuick background/border and scientific text pixels match shared QPainter, including layer opacity, glyph outline/soft shadow, defaults and high-resolution scale; PNG/SVG/PDF files inspected, vectors retained.\n";return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}

#include "canvas_layer_model.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImageReader>
#include <QPdfWriter>
#include <QSvgGenerator>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
bool near(double a,double b){return std::abs(a-b)<1e-8;}
QByteArray bytes(const QString &path){QFile file(path);check(file.open(QIODevice::ReadOnly),"Cannot inspect exported artifact");return file.readAll();}
struct Host {
    struct Snapshot {QVariantMap state;QVariantList resources;bool operator==(const Snapshot &)const=default;};
    QVariantMap state;
    QVariantList resources;
    CanvasLayerModel *canvas=nullptr;
    Snapshot before;
    bool editing=false,loading=false;
    int starts=0,finishes=0,undoCount=0;
    QStringList removed;
    Snapshot snapshot()const{return {state,resources};}
    CanvasLayerModel::Binding binding(){
        CanvasLayerModel::Binding value;
        value.getState=[this]{return state;};
        value.storeState=[this](const QVariantMap &next){if(next==state)return;if(!editing)++undoCount;state=next;};
        value.rawLayers=[this]{return resources;};
        value.exists=[this](const QString &id){for(const auto &v:resources)if(v.toMap().value("id")==id)return true;return false;};
        value.remove=[this](const QStringList &ids){
            // Emulate synchronous controller signals which used to invalidate
            // removeSelected's reference to its own selection.
            if(canvas)canvas->clearSelection();removed=ids;
            resources.removeIf([&](const QVariant &v){return ids.contains(v.toMap().value("id").toString());});
        };
        value.duplicate=[this](const QString &id){for(const auto &v:resources){auto row=v.toMap();if(row.value("id")!=id)continue;
            const auto copy=id+"-copy";row["id"]=copy;resources.append(row);return copy;}return QString();};
        value.beginEdit=[this]{check(!editing,"Common model opened duplicate host transactions");editing=true;before=snapshot();++starts;};
        value.endEdit=[this](bool cancel){check(editing,"Common model ended missing host transaction");editing=false;++finishes;
            if(cancel){state=before.state;resources=before.resources;}else if(snapshot()!=before)++undoCount;};
        value.duration=[]{return 12.;};value.loading=[this]{return loading;};return value;
    }
};
void writeSvg(const QString &path,const QVariantMap &state)
{
    QSvgGenerator svg;svg.setFileName(path);svg.setSize({1920,1080});svg.setViewBox(QRect(0,0,1920,1080));
    QPainter painter(&svg);replay::paintCanvasElements(painter,state,{1920,1080},2.);check(painter.end(),"SVG painter did not finish");
}
void writePdf(const QString &path,const QVariantMap &state)
{
    QPdfWriter pdf(path);pdf.setResolution(300);pdf.setPageSize(QPageSize(QSizeF(1920.*25.4/300.,1080.*25.4/300.),QPageSize::Millimeter));pdf.setPageMargins(QMarginsF(0,0,0,0));
    QPainter painter(&pdf);replay::paintCanvasElements(painter,state,{1920,1080},2.);check(painter.end(),"PDF painter did not finish");
}
}
int main(int argc,char **argv)
{
    QGuiApplication app(argc,argv);QTemporaryDir temporary;
    try{
#ifdef Q_OS_WIN
        // Qt's offscreen plugin otherwise uses its synthetic fixed-width font.
        check(QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf")>=0,"Cannot load actual Windows UI font for painter verification");
        check(QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc")>=0,"Cannot load actual Chinese font for painter verification");
#endif
        check(temporary.isValid(),"Cannot create binding fixture directory");
        const QString output=argc>1?QString::fromLocal8Bit(argv[1]):temporary.path();check(QDir().mkpath(output),"Cannot create artifact directory");
        Host host;host.resources={QVariantMap{{"id","custom"},{"name","Real arbitrary host kind"},{"kind","custom-host"},{"source","host://real"},
            {"x",.13},{"y",.17},{"width",.4},{"height",.3},{"opacity",.65},{"order",4},{"style",QVariantMap{{"preserve",true}}}}};
        CanvasLayerModel model(host.binding());host.canvas=&model;
        check(model.layers().size()==1&&model.settings("custom").value("source")=="host://real"&&model.active("custom",0.),"Binding lost arbitrary resource kind or raw fields");
        check(model.geometry("custom",{},QSizeF(1000,500),0.)==QRectF(130,85,400,150),"Raw normalized placement did not reach common geometry");
        const auto untouched=host.snapshot();model.edit("missing",{{"x",.1}});model.select("missing");model.reset("missing");model.removeElements({"missing"});
        check(host.snapshot()==untouched&&model.selection().isEmpty()&&host.starts==0,"Unknown edits created state or history");
        model.edit("custom",{{"rotation",32.},{"group","original"},{"visibilityRangesEnabled",true},{"visibilityRanges",QVariantList{QVariantList{1.,4.},QVariantList{6.,9.}}}});
        model.addKeyframe("custom",1.,{{"x",.2},{"opacity",.3}});model.addKeyframe("custom",3.,{{"x",.8},{"opacity",.9}});
        model.addKeyframe("custom",2.,{{"y",.5}});
        const auto original=model.settings("custom");const auto beforeCopy=host.undoCount;
        const auto copy=model.duplicate("custom");check(copy=="custom-copy"&&host.undoCount==beforeCopy+1&&host.starts==1&&host.finishes==1,"Host duplicate did not form exactly one transaction");
        const auto cloned=model.settings(copy);check(near(cloned.value("x").toDouble(),.155)&&near(cloned.value("y").toDouble(),.195)&&cloned.value("rotation")==32.&&cloned.value("keyframes")==original.value("keyframes")&&cloned.value("visibilityRanges")==original.value("visibilityRanges"),"Duplicate lost placement, animation or ranges");
        check(!cloned.contains("group")&&!host.state.value("items").toMap().value(copy).toMap().contains("source"),"Duplicate leaked runtime source/default identity into overrides");
        for(int i=0;i<200;++i){const double time=((i*7919)%12001)/1000.;const auto settings=model.settings("custom",time);
            check(settings==replay::canvasItemSettings(host.state,"custom",time,host.resources[0].toMap()),"Shared seek evaluation diverged from common model");
            if(time>=1.&&time<=3.)check(near(settings.value("x").toDouble(),.2+.3*(time-1.)),"Seek interpolation used wrong experiment time");
        }
        check(!model.active("custom",.999)&&model.active("custom",1.)&&!model.active("custom",4.)&&model.active("custom",6.)&&!model.active("custom",9.),"Half-open visibility ranges failed");
        const auto beforeInvalid=host.snapshot();model.addKeyframe("custom",std::numeric_limits<double>::infinity(),{{"x",.5}});model.edit("custom",{{"z",std::numeric_limits<double>::quiet_NaN()}});
        check(host.snapshot()==beforeInvalid,"Nonfinite edit corrupted canvas state");
        const auto beforeCancel=host.snapshot();const auto starts=host.starts;model.beginEdit();model.beginEdit();model.edit("custom",{{"x",.7}});model.endEdit(true);model.endEdit();
        check(host.snapshot()==beforeCancel&&host.starts==starts+1,"Nested canceled edit did not restore host state in one transaction");
        QImage transformImage(100,100,QImage::Format_ARGB32_Premultiplied);transformImage.fill(Qt::transparent);QPainter transformPainter(&transformImage);transformPainter.setOpacity(.6);
        replay::applyCanvasItemTransform(transformPainter,{{"rotation",90.},{"anchorX",0.},{"anchorY",0.},{"flipH",true},{"opacity",.5}},QRectF(10,20,40,20));
        const auto origin=transformPainter.worldTransform().map(QPointF(0,0));check(near(origin.x(),10.)&&near(origin.y(),60.)&&near(transformPainter.opacity(),.3),"Painter rotation/reflection/anchor/opacity differ from canvas semantics");transformPainter.end();
        const auto unsupported=host.snapshot();check(model.addElement("colorbar").isEmpty()&&host.snapshot()==unsupported,"Unsupported research element was presented as implemented");
        const auto text=model.addElement("text",{{"text","Experiment 2.000 s · 实验"},{"x",.07},{"y",.05},{"style",QVariantMap{{"color","#26313e"},{"fontSize",48.}}}});
        const auto rectangle=model.addElement("rectangle",{{"x",.1},{"y",.3},{"width",.3},{"height",.3},{"style",QVariantMap{{"fill","#e6edf7"},{"stroke","#2563eb"},{"strokeWidth",8.}}}});
        const auto circle=model.addElement("circle",{{"x",.55},{"y",.3},{"style",QVariantMap{{"fill","#e4f4ea"}}}});
        const auto arrow=model.addElement("arrow",{{"x",.4},{"y",.7},{"style",QVariantMap{{"stroke","#b45309"}}}});
        check(!text.isEmpty()&&!rectangle.isEmpty()&&!circle.isEmpty()&&!arrow.isEmpty()&&model.element(text).value("scientific").toBool(),"Supported scientific elements were not editable model instances");
        model.edit(text,{{"rotation",7.},{"name","Label A"}});model.editElement(text,{{"text","Updated experiment 2.000 s · 实验"},{"style",QVariantMap{{"fontWeight",600}}}});
        check(model.element(text).value("name")=="Label A"&&model.element(text).value("rotation")==7.&&model.element(text).value("style").toMap().value("fontSize")==48.&&model.element(text).value("style").toMap().value("fontWeight")==600,"Element edit lost common overrides or unmodified style fields");
        model.select(text);model.select(rectangle,true);model.groupSelection();const auto portable=model.copySelected();check(portable.size()==2,"Portable copy omitted shared editable elements");
        Host second;CanvasLayerModel destination(second.binding());second.canvas=&destination;const auto undoBeforePaste=second.undoCount;
        const auto pasted=destination.pasteElements(portable);check(pasted.size()==2&&second.undoCount==undoBeforePaste+1&&second.starts==1&&second.finishes==1,"Portable multi-paste did not create one Undo transaction");
        check(pasted[0]!=text&&destination.element(pasted[0]).value("text")==model.element(text).value("text")&&destination.element(pasted[0]).value("group")!=model.element(text).value("group"),"Cross-host copy lost editable content or reused source IDs/group");
        const auto beforeMixed=host.snapshot();const auto undoBeforeRemove=host.undoCount;model.select(text);model.select("custom-copy",true);model.removeSelected();
        check(host.removed==QStringList{"custom-copy"}&&model.element(text).isEmpty()&&!model.active(copy,2.)&&host.undoCount==undoBeforeRemove+1,"Synchronous mixed remove lost IDs or generated multiple Undo operations");
        host.state=beforeMixed.state;host.resources=beforeMixed.resources;model.refresh();check(model.element(text).value("text")=="Updated experiment 2.000 s · 实验","Host Undo did not restore editable element content");
        const auto vectorState=host.state;writeSvg(QDir(output).filePath("scientific-vector.svg"),vectorState);writePdf(QDir(output).filePath("scientific-vector.pdf"),vectorState);
        const auto svg=bytes(QDir(output).filePath("scientific-vector.svg")),pdf=bytes(QDir(output).filePath("scientific-vector.pdf"));
        check(!svg.contains("<image")&&(svg.contains("<text")||svg.contains("<path"))&&svg.contains("Updated")&&svg.contains("experiment"),"Vector scientific SVG rasterized text/shapes or lost text");
        check(pdf.startsWith("%PDF-")&&!pdf.contains("/Subtype /Image"),"Vector scientific PDF contains raster screenshots");
        QImage picture(40,20,QImage::Format_ARGB32);picture.fill(QColor("#d26b8b"));const auto source=QDir(temporary.path()).filePath("real image 资料.png");check(picture.save(source),"Cannot create real editable image fixture");
        const auto pictureId=model.addElement("image",{{"source",QUrl::fromLocalFile(source)},{"x",.6},{"y",.7},{"width",.2}});check(!pictureId.isEmpty(),"File URL image add failed");
        model.editElement(pictureId,{{"style",QVariantMap{{"imageFit","cover"}}}});model.select(pictureId);const auto imageCopy=destination.pasteElements(model.copySelected());
        check(imageCopy.size()==1&&destination.element(imageCopy[0]).value("source").toString()==source,"Absolute Windows image path failed edit/revalidation/cross-workspace paste");
        writeSvg(QDir(output).filePath("scientific-mixed.svg"),host.state);writePdf(QDir(output).filePath("scientific-mixed.pdf"),host.state);
        check(bytes(QDir(output).filePath("scientific-mixed.svg")).contains("<image")&&bytes(QDir(output).filePath("scientific-mixed.pdf")).contains("/Subtype /Image"),"Mixed output omitted its honest embedded bitmap image");
        QImage outputImage(1920,1080,QImage::Format_ARGB32_Premultiplied);outputImage.fill(Qt::transparent);QPainter painter(&outputImage);replay::paintCanvasElements(painter,host.state,outputImage.size(),2.);painter.end();
        check(qAlpha(outputImage.pixel(1,1))==0&&QColor(outputImage.pixel(1250,850)).red()>150,"Scientific raster export lost transparency or local source image");
        check(outputImage.save(QDir(output).filePath("scientific-preview.png")),"Cannot save rendered scientific preview");
        // Actual EXIF orientation 6: a 30x20 coded JPEG is displayed as 20x30.
        QImage coded(30,20,QImage::Format_RGB32);coded.fill(Qt::blue);
        {QPainter p(&coded);p.fillRect(0,0,15,20,Qt::red);}
        QByteArray jpeg;{QBuffer buffer(&jpeg);buffer.open(QIODevice::WriteOnly);check(coded.save(&buffer,"JPG",100),"Cannot encode EXIF fixture JPEG");}
        const auto exif=QByteArray::fromHex("ffe1002245786966000049492a0008000000010012010300010000000600000000000000");jpeg.insert(2,exif);
        const auto oriented=QDir(temporary.path()).filePath("portrait-from-exif.jpg");{QFile file(oriented);check(file.open(QIODevice::WriteOnly)&&file.write(jpeg)==jpeg.size(),"Cannot write EXIF orientation fixture");}
        QString imageError;const auto displayed=replay::loadCanvasElementImage(oriented,&imageError);
        check(imageError.isEmpty()&&displayed.size()==QSize(20,30)&&replay::canvasElementImageSize(oriented)==displayed.size()&&displayed.pixelColor(10,3).red()>200&&displayed.pixelColor(10,27).blue()>200,"Shared image loader ignored EXIF display orientation");
        const auto orientedId=destination.addElement("image",{{"source",oriented},{"width",.1}});
        check(!orientedId.isEmpty()&&near(destination.element(orientedId).value("height").toDouble(),.1*1920.*30./(1080.*20.)),"New image geometry used coded aspect instead of EXIF display aspect");
        const QVariantMap exifCanvas{{"outputWidth",20},{"elements",QVariantList{QVariantMap{{"id","exif"},{"kind","image"},{"source",oriented},{"x",0.},{"y",0.},{"width",1.},{"height",1.}}}}};
        QImage rotatedOutput(20,30,QImage::Format_ARGB32_Premultiplied);rotatedOutput.fill(Qt::transparent);{QPainter p(&rotatedOutput);replay::paintCanvasElements(p,exifCanvas,rotatedOutput.size(),0.);}
        check(rotatedOutput.pixelColor(10,3).red()>200&&rotatedOutput.pixelColor(10,27).blue()>200&&bytes(oriented)==jpeg,"CPU image export differed from oriented display or modified source bytes");
        check(replay::loadCanvasElementImage("https://invalid.example/image.jpg",&imageError).isNull()&&!imageError.isEmpty(),"Image loader accepted a nonlocal source silently");
        host.loading=true;const auto loaded=host.snapshot();check(model.addElement("text").isEmpty()&&model.pasteElements(portable).isEmpty(),"Loading host accepted scientific mutations");model.removeElements({text});check(host.snapshot()==loaded,"Loading host removal corrupted active project");
        std::cout<<"Pure Binding/Canvas checks passed without ReplayController: arbitrary resource defaults, nested/canceled transactions, raw duplication, synchronous selection removal, exact seek/ranges, transform parity, shared scientific CRUD/copy/paste, EXIF display geometry and true vector versus mixed bitmap PDF/SVG.\n";return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}

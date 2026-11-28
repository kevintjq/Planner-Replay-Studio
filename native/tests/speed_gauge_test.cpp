#include "speed_gauge_item.h"
#include "appearance_manager.h"

#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImageReader>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <iostream>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace {
void check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
QByteArray read(const QString &path){QFile file(path);check(file.open(QIODevice::ReadOnly),"Cannot read generated element");return file.readAll();}
QImage render(const QVariantMap &data,const QVariantMap &style,const QSize &size={600,520})
{
    QImage image(size,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);
    QPainter painter(&image);SpeedGaugeItem::drawGauge(painter,size,data,style);painter.end();return image;
}
QVariant evaluate(QQmlEngine &engine,QObject *object,const QString &source)
{
    QQmlExpression expression(engine.rootContext(),object,source);const auto value=expression.evaluate();
    if(expression.hasError())throw std::runtime_error(expression.error().toString().toStdString());return value;
}
void checkTickLabels(const QString &directory)
{
    struct Case {QString name;double minimum,maximum;QVariantMap style;QStringList labels;int fixedPlaces;QString unit="m/s";};
    const QList<Case> cases{
        {"auto-0-2",0.,2.,{{"majorTickCount",5}},{"0.0","0.4","0.8","1.2","1.6","2.0"},1},
        {"negative",-1.,1.,{{"majorTickCount",4}},{"-1.0","-0.5","0.0","0.5","1.0"},1},
        {"small-signed",-.003,.003,{{"majorTickCount",6}},{"-0.003","-0.002","-0.001","0.000","0.001","0.002","0.003"},3},
        {"kmh",0.,7.2,{{"majorTickCount",5}},{"0.00","1.44","2.88","4.32","5.76","7.20"},2,"km/h"},
        {"offset",.125,2.125,{{"majorTickCount",4}},{"0.125","0.625","1.125","1.625","2.125"},3},
        {"fractional-interval",0.,.0125,{{"majorTickInterval",.0025}},{"0.0000","0.0025","0.0050","0.0075","0.0100","0.0125"},4},
        {"signed-interval",-.3,.3,{{"majorTickInterval",.1}},{"-0.3","-0.2","-0.1","0.0","0.1","0.2","0.3"},1},
        {"tiny",0.,5e-9,{{"majorTickCount",5}},{"0","1e-09","2e-09","3e-09","4e-09","5e-09"},-1},
        {"tiny-interval",0.,5e-9,{{"majorTickInterval",1e-9}},{"0","1e-09","2e-09","3e-09","4e-09","5e-09"},-1},
        {"explicit-precision",0.,2.,{{"majorTickCount",5},{"tickDecimals",2}},{"0.00","0.40","0.80","1.20","1.60","2.00"},2}
    };
    for(const auto &test:cases){
        const QVariantMap data{{"currentValid",true},{"current",(test.minimum+test.maximum)/2},
            {"rangeMinimum",test.minimum},{"rangeMaximum",test.maximum},{"unit",test.unit}};
        auto style=SpeedGaugeItem::defaultStyle("publication");for(auto it=test.style.begin();it!=test.style.end();++it)style[it.key()]=it.value();
        const auto path=directory+"/ticks-"+test.name+".svg";
        const auto result=SpeedGaugeItem::writeElement(path,{600,520},300,"svg",data,style);
        const auto actual=result.value("majorTickLabels").toStringList();
        check(result.value("success").toBool()&&actual==test.labels,
            qPrintable("Major tick labels differ for "+test.name+": "+actual.join(", ")));
        if(test.fixedPlaces>=0){
            auto explicitStyle=style;explicitStyle["tickDecimals"]=test.fixedPlaces;
            check(render(data,style)==render(data,explicitStyle),"Automatic labels do not paint the expected precision");
        }
        check(!read(path).contains("<image"),"Tick-label vector output contains a raster fallback");
        if(test.name=="auto-0-2"){
            auto rounded=style;rounded["tickDecimals"]=0;
            check(render(data,style)!=render(data,rounded),"Automatic 0.4-step labels still paint repeated integers");
            check(render(data,style).save(directory+"/ticks-auto-0-2.png"),"Cannot save corrected automatic-scale fixture");
        }
        std::cout<<test.name.toStdString()<<": "<<actual.join(", ").toStdString()<<'\n';
    }
}
}
int main(int argc,char **argv)
{
    QGuiApplication app(argc,argv);QTemporaryDir temporary;
#ifdef Q_OS_WIN
    // The offscreen platform may supply a dummy font database. Load the actual
    // OS font for deterministic glyph geometry; production uses Windows fonts.
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR")+"/Fonts/segoeui.ttf");
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR")+"/Fonts/msyh.ttc");
#endif
    try{
        const QString directory=app.arguments().size()>1?app.arguments()[1]:temporary.path();
        check(QDir().mkpath(directory),"Cannot create element test directory");
        checkTickLabels(directory);
        QVariantMap data{{"currentValid",true},{"current",5.},{"maximum",8.5},{"average",3.24},
                         {"rangeMinimum",0.},{"rangeMaximum",10.},{"unit","m/s"},{"stampNs","1787070460123456789"}};
        auto style=SpeedGaugeItem::defaultStyle();const auto image=render(data,style);
        check(image.pixelColor(0,0).alpha()==0&&image.pixelColor(300,375).alpha()==0,"Default transparent gauge has a hidden panel");
        int painted=0,transparent=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){if(qAlpha(image.pixel(x,y)))++painted;else ++transparent;}
        check(painted>10000&&transparent>painted,"Gauge is empty or background is opaque");
        int currentInk=0;for(int y=215;y<285;++y)for(int x=120;x<480;++x)if(qAlpha(image.pixel(x,y)))++currentInk;
        check(currentInk>1000,"Current numeric glyphs are missing or outside the canvas");
        auto noData=data;noData.remove("current");noData["currentValid"]=false;
        auto zero=data;zero["current"]=0.;check(render(noData,style)!=render(zero,style),"Missing data was drawn as zero");
        auto withoutText=style;withoutText["showCurrent"]=false;withoutText["showMaximum"]=false;withoutText["showAverage"]=false;
        withoutText["showUnit"]=false;withoutText["title"]="";withoutText["showTickLabels"]=false;
        auto km=data;km["unit"]="km/h";for(const auto &key:{"current","maximum","average","rangeMinimum","rangeMaximum"})km[key]=data.value(key).toDouble()*3.6;
        check(render(data,withoutText)==render(km,withoutText),"Unit conversion changed the physical indicator position");
        auto physical=withoutText;physical["gradientStops"]=QVariantList{QVariantMap{{"valueSI",0.},{"color","#0000ff"}},QVariantMap{{"valueSI",10.},{"color","#ff0000"}}};
        check(render(data,physical)==render(km,physical),"Physical gradient stops moved when units changed");
        auto invisible=style;invisible["overallOpacity"]=0.;check(render(data,invisible).pixelColor(100,100).alpha()==0,"Overall alpha was ignored");
        auto solid=style;solid["backgroundMode"]="solid";check(render(data,solid).pixelColor(300,375).alpha()==255,"Solid background is not opaque");
        auto translucent=style;translucent["backgroundMode"]="translucent";translucent["backgroundOpacity"]=.4;
        check(std::abs(render(data,translucent).pixelColor(300,375).alpha()-102)<=1,"Panel opacity was not independent");
        auto half=style;half["overallOpacity"]=.5;const auto halfImage=render(data,half);
        int highest=0;for(int y=0;y<halfImage.height();++y)for(int x=0;x<halfImage.width();++x)highest=std::max(highest,qAlpha(halfImage.pixel(x,y)));
        // Overlapping vector strokes can composite, but the item opacity applies to
        // the whole paint layer in the app. The standalone path uses painter opacity.
        check(highest>0&&highest<255,"Overall opacity does not affect standalone output");
        for(const auto &format:{QStringLiteral("png"),QStringLiteral("tiff"),QStringLiteral("svg"),QStringLiteral("pdf")}){
            const auto path=directory+"/gauge."+format;const auto result=SpeedGaugeItem::writeElement(path,{1200,1040},600,format,data,style);
            if(!result.value("success").toBool())throw std::runtime_error(result.value("error").toString().toStdString());
            check(result.value("targetNs").toString()==data.value("stampNs").toString(),"Export lost exact experiment timestamp");
            if(format=="png"||format=="tiff"){
                QImageReader reader(path);const auto actual=reader.read();check(!actual.isNull()&&actual.size()==QSize(1200,1040),"Raster output has wrong size");
                check(actual.hasAlphaChannel()&&actual.pixelColor(0,0).alpha()==0,"Raster alpha was lost");
                check(std::abs(actual.dotsPerMeterX()*.0254-600)<1,"Raster DPI was lost");
            }else if(format=="svg"){
                const auto xml=read(path);check(xml.contains("<path")&&!xml.contains("<image")&&!xml.contains("base64"),"SVG contains a raster fallback");
                QSvgRenderer svg(xml);check(svg.isValid()&&svg.viewBox().size()==QSize(1200,1040),"SVG view box is wrong");
                QImage decoded({1200,1040},QImage::Format_ARGB32_Premultiplied);decoded.fill(Qt::transparent);QPainter painter(&decoded);svg.render(&painter);painter.end();
                check(decoded.pixelColor(0,0).alpha()==0,"SVG lost transparent background");
                check(decoded.save(directory+"/gauge-svg-rendered.png"),"Cannot inspect rendered SVG");
            }else{
                const auto pdf=read(path);check(pdf.startsWith("%PDF")&&!pdf.contains("/Subtype /Image"),"PDF contains a raster fallback");
            }
        }
        for(const auto &size:{QSize(3840,3328),QSize(7680,6656)}){
            const auto result=SpeedGaugeItem::writeElement(directory+QString("/gauge-%1.png").arg(size.width()),size,1200,"png",data,style);
            check(result.value("success").toBool(),"High resolution gauge export failed");
            QImageReader reader(result.value("path").toString());check(reader.size()==size,"High resolution file dimensions are wrong");
        }
        check(!SpeedGaugeItem::writeElement(directory+"/invalid.png",{0,0},300,"png",data,style).value("success").toBool(),"Invalid size was accepted");
        check(!SpeedGaugeItem::writeElement(directory+"/source.bag",{600,520},300,"png",data,style).value("success").toBool(),"A mismatched extension could overwrite a non-image source");
        for(const auto &preset:{"gradient-gauge","minimal-gauge","digital-hud","speed-curve","publication","custom"}){
            const auto values=SpeedGaugeItem::defaultStyle(QLatin1String(preset));
            check(!values.contains("topic")&&!values.contains("unit")&&!values.contains("statisticsMode")&&!values.contains("x"),"Visual preset overwrites data or transform");
            check(render(data,values).save(directory+"/preset-"+preset+".png"),"Cannot write preset sample");
        }
        SpeedGaugeItem item;item.setDataMap(data);item.setStyleMap(style);bool finished=false;QString completed;bool success=false;QVariantMap completedMetadata;
        QObject::connect(&item,&SpeedGaugeItem::exportFinished,[&](const QString &id,bool ok,const QVariantMap&metadata,const QString&){completed=id;success=ok;completedMetadata=metadata;finished=true;});
        const auto job=item.exportElement(QUrl::fromLocalFile(directory+"/async.svg"),{600,520},300,"svg");
        QElapsedTimer deadline;deadline.start();while(!finished&&deadline.elapsed()<10000){QCoreApplication::processEvents();QThread::msleep(2);}
        check(finished&&success&&completed==job&&!item.exporting(),"Asynchronous export did not finish correctly");
        finished=false;int heartbeats=0;QTimer heartbeat;heartbeat.setInterval(1);QObject::connect(&heartbeat,&QTimer::timeout,[&]{++heartbeats;});heartbeat.start();
        const auto largeJob=item.exportElement(QUrl::fromLocalFile(directory+"/async-8k.png"),{7680,6656},1200,"png");
        item.setDataMap(km);deadline.restart();while(!finished&&deadline.elapsed()<20000){QCoreApplication::processEvents();QThread::msleep(2);}
        heartbeat.stop();check(finished&&success&&completed==largeJob&&heartbeats>3,"8K export blocked the UI event loop");
        check(completedMetadata.value("unit")=="m/s","Export snapshot changed when live settings changed");
        finished=false;const auto invalidJob=item.exportElement(QUrl("https://invalid/element.png"),{600,520},300,"png");
        check(!finished,"Invalid export completed synchronously before caller stored job ID");QCoreApplication::processEvents();
        check(finished&&!success&&completed==invalidJob,"Invalid URL did not report failure");
        AppearanceManager appearance(nullptr,temporary.filePath("appearance.ini"));QQmlEngine engine;
        engine.rootContext()->setContextProperty("appearance",&appearance);QStringList warnings;
        QObject::connect(&engine,&QQmlEngine::warnings,[&](const QList<QQmlError>&errors){for(const auto &error:errors)warnings.append(error.toString());});
        QQmlComponent component(&engine);component.setData(R"QML(
import QtQuick
import QtQuick.Layouts
import "."
SpeedMeterInspector {
    width: 360
    controller: QtObject {}
    meterId: "fixture"
    model: QtObject {
        property bool exporting: false
        signal changed()
        function sourceState(id) { return {ready:true,working:false} }
    }
    topicInspector: QtObject { property var catalog: [{name:"/actual-style-fixture",type:"test/Velocity",numericFields:["velocity.x","velocity.y","velocity.z","speed"]}] }
    sourceMap: ({topic:"/actual-style-fixture",type:"test/Velocity",fields:["velocity.x","velocity.y","velocity.z"],timeBasis:"header",inputUnit:"m/s"})
    settings: ({unit:"m/s",statisticsMode:"multiple",includeRanges:[{beginNs:"1787070460123456789",endNs:"1787070461123456789"}]})
    property var latestSettings: ({})
    property var latestSource: ({})
    onSettingsEdited: changes => latestSettings=changes
    onSourceEdited: changes => latestSource=changes
}
)QML",QUrl::fromLocalFile(QStringLiteral(NATIVE_QML_DIRECTORY "/GaugeInspectorFixture.qml")));
        if(component.isError())std::cerr<<component.errorString().toStdString();check(component.isReady(),"Speed Inspector does not compile");
        std::unique_ptr<QObject> inspector(component.create());if(!inspector)std::cerr<<component.errorString().toStdString();check(bool(inspector),"Speed Inspector does not instantiate");
        for(int mode:{AppearanceManager::Light,AppearanceManager::Dark}){
            appearance.setMode(mode);QCoreApplication::processEvents();
            auto *quick=qobject_cast<QQuickItem*>(inspector.get());check(quick&&quick->implicitHeight()>0,"Inspector lost layout after theme switch");
        }
        check(evaluate(engine,inspector.get(),"compareNs('1787070460123456789','1787070460123456790')").toInt()==-1,"Inspector converted precise ns to lossy Number");
        evaluate(engine,inspector.get(),"editSetting('unit','km/h')");check(inspector->property("latestSettings").toMap().value("unit")=="km/h","Inspector unit patch not emitted");
        evaluate(engine,inspector.get(),"applySource()");check(inspector->property("latestSource").toMap().value("fields").toList().size()==3,"Inspector lost actual field choices");
        if(!warnings.isEmpty())std::cerr<<warnings.join('\n').toStdString()<<'\n';check(warnings.isEmpty(),"Inspector has runtime QML warnings");
        std::cout<<"Speed gauge fixture checks passed: true SVG/PDF paths, PNG/TIFF alpha/DPI, 4K/8K, exact ns, unit invariant indicator, asynchronous export and themed inline Inspector.\n";
        std::cout<<"Artifacts: "<<directory.toStdString()<<"\nThese are deterministic test fixtures, not real flight GUI/reference acceptance.\n";
        return 0;
    }catch(const std::exception&error){std::cerr<<error.what()<<'\n';return 1;}
}

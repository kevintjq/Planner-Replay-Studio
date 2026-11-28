#include "speed_gauge_item.h"

#include <QFileInfo>
#include <QFontMetricsF>
#include <QImageWriter>
#include <QPageSize>
#include <QPainterPath>
#include <QPdfWriter>
#include <QSaveFile>
#include <QStringList>
#include <QSvgGenerator>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>

namespace {
constexpr qreal designWidth = 600, designHeight = 520;
qreal number(const QVariantMap &m, const char *key, qreal fallback, qreal low = -1e9, qreal high = 1e9)
{
    bool ok = false; const qreal value = m.value(QLatin1String(key)).toDouble(&ok);
    return ok && std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
QColor color(const QVariantMap &m, const char *key, const QColor &fallback)
{
    const auto value = m.value(QLatin1String(key));
    QColor result = value.canConvert<QColor>() ? value.value<QColor>() : QColor(value.toString());
    return result.isValid() ? result : fallback;
}
QColor faded(QColor c, qreal opacity) { c.setAlphaF(c.alphaF() * std::clamp(opacity, 0., 1.)); return c; }
bool flag(const QVariantMap &m, const char *key, bool fallback = true) { return m.value(QLatin1String(key), fallback).toBool(); }
QPointF polar(QPointF center, qreal radius, qreal degrees)
{
    const auto angle = degrees * std::numbers::pi / 180.;
    return center + QPointF(std::cos(angle) * radius, std::sin(angle) * radius);
}
QPainterPath arc(QPointF c, qreal radius, qreal start, qreal sweep)
{
    QPainterPath path; const QRectF rect(c.x()-radius,c.y()-radius,2*radius,2*radius);
    path.arcMoveTo(rect,-start); path.arcTo(rect,-start,-sweep); return path;
}
using Stops = QList<QPair<qreal,QColor>>;
Stops stops(const QVariantMap &style, qreal minimum, qreal maximum)
{
    Stops result;
    for (const auto &v : style.value("gradientStops").toList()) {
        const auto m=v.toMap(); const auto c=color(m,"color",{}); if (!c.isValid()) continue;
        qreal p=number(m,"position",0.,0.,1.);
        // A physical SI stop stays at the same speed when m/s becomes km/h.
        if (m.contains("valueSI")) {
            const auto unitScale=number(style,"_unitScale",1.);
            p=(number(m,"valueSI",0.)*unitScale-minimum)/(maximum-minimum);
        }
        result.append({std::clamp(p,0.,1.),c});
    }
    if(result.isEmpty()) result={{0.,QColor("#3588ff")},{.2,QColor("#20d2ee")},{.4,QColor("#29dc97")},
                                 {.6,QColor("#eee66b")},{.8,QColor("#ff9d48")},{1.,QColor("#ff535d")}};
    std::stable_sort(result.begin(),result.end(),[](const auto&a,const auto&b){return a.first<b.first;});
    return result;
}
QColor gradientColor(const Stops &values,qreal t)
{
    if(t<=values.first().first)return values.first().second;
    for(qsizetype i=1;i<values.size();++i) if(t<=values[i].first){
        const auto &a=values[i-1],&b=values[i]; const auto f=(t-a.first)/std::max(1e-12,b.first-a.first);
        return QColor::fromRgbF(a.second.redF()*(1-f)+b.second.redF()*f,
            a.second.greenF()*(1-f)+b.second.greenF()*f,a.second.blueF()*(1-f)+b.second.blueF()*f,
            a.second.alphaF()*(1-f)+b.second.alphaF()*f);
    }
    return values.last().second;
}
// Solid-colour vector arc segments avoid a raster-only conical gradient fallback
// in SVG/PDF. Node positions follow the actual numeric range, never screen X.
void gradientArc(QPainter &p,QPointF center,qreal radius,qreal start,qreal sweep,qreal width,
                 const Stops &colors,qreal opacity,qreal fraction=1.)
{
    fraction=std::clamp(fraction,0.,1.); const int count=std::max(1,int(std::ceil(std::abs(sweep)*fraction*2.)));
    const auto step=sweep*fraction/count;
    for(int i=0;i<count;++i){
        QPen pen(faded(gradientColor(colors,(i+.5)*fraction/count),opacity),width,Qt::SolidLine,Qt::FlatCap);
        p.setPen(pen);p.setBrush(Qt::NoBrush);p.drawPath(arc(center,radius,start+step*i,step+std::copysign(.035,step)));
    }
    p.setPen(Qt::NoPen);p.setBrush(faded(gradientColor(colors,0.),opacity));
    p.drawEllipse(polar(center,radius,start),width/2,width/2);
    p.setBrush(faded(gradientColor(colors,fraction),opacity));
    p.drawEllipse(polar(center,radius,start+sweep*fraction),width/2,width/2);
}
void text(QPainter &p,const QString &value,const QRectF &rect,const QVariantMap &s,QColor fallback,qreal size,
          int weight=QFont::Normal,Qt::Alignment alignment=Qt::AlignCenter)
{
    if(value.isEmpty())return;
    QFont font(s.value("fontFamily","Segoe UI").toString());
    font.setPixelSize(qRound(number(s,"fontSize",size,4.,160.)));
    font.setWeight(static_cast<QFont::Weight>(int(number(s,"fontWeight",weight,100.,900.))));
    QPainterPath path;path.addText(QPointF(0,0),font,value);
    const auto bounds=path.boundingRect();
    qreal x=rect.left()-bounds.left();if(alignment&Qt::AlignHCenter)x+=(rect.width()-bounds.width())/2;
    else if(alignment&Qt::AlignRight)x+=rect.width()-bounds.width();
    path.translate(x,rect.center().y()-bounds.center().y());
    const auto opacity=number(s,"opacity",1.,0.,1.);
    if(flag(s,"shadow",false)){
        p.save();p.translate(number(s,"shadowX",0.),number(s,"shadowY",2.));
        p.setPen(Qt::NoPen);p.setBrush(faded(color(s,"shadowColor",QColor(0,0,0,160)),opacity));p.drawPath(path);p.restore();
    }
    const auto outline=number(s,"outlineWidth",0.,0.,12.);
    p.setPen(outline>0?QPen(faded(color(s,"outlineColor",QColor("#18222c")),opacity),outline,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin):QPen(Qt::NoPen));
    p.setBrush(faded(color(s,"color",fallback),opacity));p.drawPath(path);
}
QString valueLabel(const QVariantMap &data,const QString &key,int places)
{
    bool ok=false;const auto value=data.value(key).toDouble(&ok);
    if(!ok||!std::isfinite(value)||(key=="current"&&!data.value("currentValid").toBool()))return QStringLiteral("N/A");
    return QString::number(value,'f',places);
}
QVariantMap merged(const QVariantMap &base,const QVariantMap &patch){auto result=base;for(auto it=patch.begin();it!=patch.end();++it)result[it.key()]=it.value();return result;}
QVariantMap textStyle(const QVariantMap &style,const char *key)
{
    QVariantMap result{{"fontFamily",style.value("fontFamily")},{"outlineWidth",style.value("outlineWidth")},
                       {"outlineColor",style.value("outlineColor")},{"shadow",style.value("textShadow")},
                       {"shadowColor",style.value("shadowColor")}};
    return merged(result,style.value(QLatin1String(key)).toMap());
}
QVariantMap failure(const QString &message){return {{"success",false},{"error",message}};}
QList<qreal> majorTickValues(qreal minimum,qreal maximum,const QVariantMap &style)
{
    QList<qreal> values;if(maximum<=minimum)return values;
    const auto interval=number(style,"majorTickInterval",0.,0.,1e9);
    if(interval>0.){
        const auto tolerance=std::max(interval*1e-7,std::numeric_limits<qreal>::epsilon()*8*std::max(std::abs(minimum),std::abs(maximum)));
        const auto first=std::ceil((minimum-tolerance)/interval)*interval;
        for(int i=0;i<101;++i){const auto value=first+i*interval;if(value>maximum+tolerance)break;values.append(value);}
    }else{
        const int count=int(number(style,"majorTickCount",5.,1.,20.));
        for(int i=0;i<=count;++i)values.append(minimum+(maximum-minimum)*i/count);
    }
    return values;
}
QStringList majorTickLabels(const QList<qreal> &values,const QVariantMap &style)
{
    if(values.isEmpty())return {};
    auto spacing=std::numeric_limits<qreal>::max();
    for(qsizetype i=1;i<values.size();++i)if(values[i]>values[i-1])spacing=std::min(spacing,values[i]-values[i-1]);
    if(spacing==std::numeric_limits<qreal>::max())spacing=1.;
    const auto tolerance=std::max(spacing*1e-7,std::numeric_limits<qreal>::epsilon()*8*
        std::max(std::abs(values.first()),std::abs(values.last())));
    const auto normalize=[](QString label){if(label.startsWith('-')&&label.mid(1).toDouble()==0.)label.remove(0,1);return label;};
    const auto fixed=[&](int places){QStringList labels;for(const auto value:values)labels.append(normalize(QString::number(value,'f',places)));return labels;};
    if(style.contains("tickDecimals"))return fixed(int(number(style,"tickDecimals",0.,0.,6.)));
    // Use the actual ticks, including an offset first tick. A 0–2 range with
    // five divisions needs 0.4 steps; km/h conversion can need two decimals.
    QStringList labels;
    for(int places=0;places<=6;++places){
        labels=fixed(places);bool accurate=true;
        for(qsizetype i=0;i<values.size();++i)if(std::abs(labels[i].toDouble()-values[i])>tolerance){accurate=false;break;}
        if(accurate)break;
    }
    // Fixed six-place labels cannot distinguish very small scientific ranges.
    // Fall back to significant digits rather than repeat misleading zeroes.
    const auto distinct=[&]{for(qsizetype i=1;i<labels.size();++i)if(labels[i]==labels[i-1])return false;return true;};
    if(!distinct())for(int digits=6;digits<=17;++digits){
        labels.clear();for(const auto value:values)labels.append(normalize(QString::number(std::abs(value)<=tolerance?0.:value,'g',digits)));
        if(distinct())break;
    }
    return labels;
}
}

SpeedGaugeItem::SpeedGaugeItem(QQuickItem *parent):QQuickPaintedItem(parent)
{
    setAntialiasing(true);setOpaquePainting(false);setImplicitWidth(300);setImplicitHeight(260);
    connect(&watcher_,&QFutureWatcher<QVariantMap>::finished,this,[this]{
        const auto result=watcher_.result();const auto id=jobId_;exporting_=false;emit exportingChanged();
        emit exportFinished(id,result.value("success").toBool(),result,result.value("error").toString());
    });
}
SpeedGaugeItem::~SpeedGaugeItem(){watcher_.waitForFinished();}
void SpeedGaugeItem::setDataMap(const QVariantMap &value){if(data_==value)return;data_=value;update();emit dataMapChanged();}
void SpeedGaugeItem::setStyleMap(const QVariantMap &value){if(style_==value)return;style_=value;update();emit styleMapChanged();}
void SpeedGaugeItem::paint(QPainter *painter){drawGauge(*painter,QSizeF(width(),height()),data_,style_);}
QVariantMap SpeedGaugeItem::presetStyle(const QString &name)const{return defaultStyle(name);}

QVariantMap SpeedGaugeItem::defaultStyle(const QString &preset)
{
    QVariantMap s{{"preset",preset},{"startAngle",145.},{"sweepAngle",250.},{"radius",202.},{"arcWidth",12.},
        {"arcOpacity",1.},{"overallOpacity",1.},{"indicatorOpacity",1.},{"indicatorStyle","marker"},
        {"indicatorColor","#f5fbff"},{"indicatorWidth",4.},{"showArc",true},{"showTicks",true},
        {"showTickLabels",true},{"showInnerRing",false},{"showOuterRing",true},
        {"tickColor","#c3d0db"},{"tickLabelColor","#cbd5df"},{"tickLabelSize",20.},
        {"majorTickCount",5},{"minorTickCount",4},{"majorTickLength",13.},{"minorTickLength",6.},
        {"tickWidth",2.},{"title","Exp. Traj."},{"subtitle",""},{"showUnit",true},{"decimalPlaces",2},
        {"fontFamily","Segoe UI"},{"textColor","#f7fafc"},{"maxColor","#ff8269"},{"averageColor","#61cfe4"},
        {"titleColor","#d7e1e9"},{"textOpacity",1.},{"outlineWidth",0.},{"outlineColor","#18222c"},
        {"textShadow",true},{"shadowColor","#80081724"},{"backgroundMode","transparent"},
        {"backgroundColor","#202831"},{"backgroundOpacity",.65},{"borderWidth",0.},{"borderColor","#8193a2"},
        {"cornerRadius",28.},{"padding",6.},{"panelShadow",false},{"showMaximum",true},{"showAverage",true},
        {"showCurrent",true},{"currentText",QVariantMap{{"fontSize",70.},{"fontWeight",600}}},
        {"maxText",QVariantMap{{"fontSize",27.},{"fontWeight",500}}},
        {"averageText",QVariantMap{{"fontSize",27.},{"fontWeight",500}}},
        {"titleText",QVariantMap{{"fontSize",27.},{"fontWeight",500}}}};
    if(preset=="minimal-gauge"){
        s["arcWidth"]=5.;s["showOuterRing"]=false;s["showTickLabels"]=false;s["showTicks"]=false;
        s["indicatorStyle"]="dot";s["showMaximum"]=false;s["showAverage"]=false;
    }else if(preset=="digital-hud"){
        s["showArc"]=false;s["showOuterRing"]=false;s["showTicks"]=false;s["showTickLabels"]=false;
        s["indicatorStyle"]="none";s["currentText"]=QVariantMap{{"fontSize",92.},{"fontWeight",600}};
    }else if(preset=="speed-curve"){
        s["showArc"]=false;s["showOuterRing"]=false;s["showTicks"]=false;s["showTickLabels"]=false;
        s["indicatorStyle"]="none";s["curveColor"]="#61cfe4";s["showCurve"]=true;
    }else if(preset=="publication"){
        s["textColor"]="#243443";s["titleColor"]="#243443";s["tickColor"]="#526574";
        s["tickLabelColor"]="#526574";s["indicatorColor"]="#233d56";s["maxColor"]="#c45038";
        s["averageColor"]="#087f96";s["textShadow"]=false;s["arcWidth"]=8.;
    }
    return s;
}

void SpeedGaugeItem::drawGauge(QPainter &p,const QSizeF &size,const QVariantMap &data,const QVariantMap &input)
{
    if(size.isEmpty())return;
    auto s=merged(defaultStyle(input.value("preset","gradient-gauge").toString()),input);
    s["_unitScale"]=data.value("unit").toString()=="km/h"?3.6:1.;
    p.save();p.setRenderHints(QPainter::Antialiasing|QPainter::TextAntialiasing|QPainter::SmoothPixmapTransform);
    const auto scale=std::min(size.width()/designWidth,size.height()/designHeight);
    p.translate((size.width()-designWidth*scale)/2,(size.height()-designHeight*scale)/2);p.scale(scale,scale);
    p.setOpacity(p.opacity()*number(s,"overallOpacity",1.,0.,1.));
    const qreal padding=number(s,"padding",6.,0.,100.);const QRectF panel(padding,padding,designWidth-2*padding,designHeight-2*padding);
    const auto background=s.value("backgroundMode").toString();const auto corner=number(s,"cornerRadius",28.,0.,100.);
    if(background!="transparent"){
        if(flag(s,"panelShadow",false)){
            p.setPen(Qt::NoPen);p.setBrush(color(s,"shadowColor",QColor(0,0,0,100)));p.drawRoundedRect(panel.translated(0,4),corner,corner);
        }
        p.setPen(Qt::NoPen);p.setBrush(faded(color(s,"backgroundColor",QColor("#202831")),
            background=="solid"?1.:number(s,"backgroundOpacity",.65,0.,1.)));p.drawRoundedRect(panel,corner,corner);
    }
    const auto border=number(s,"borderWidth",0.,0.,20.);
    if(border>0){p.setPen(QPen(color(s,"borderColor",QColor("#8193a2")),border));p.setBrush(Qt::NoBrush);p.drawRoundedRect(panel,corner,corner);}
    const QPointF center(number(s,"centerX",300.,100.,500.),number(s,"centerY",265.,100.,420.));
    const auto radius=number(s,"radius",202.,40.,225.),start=number(s,"startAngle",145.,-720.,720.);
    const auto sweep=number(s,"sweepAngle",250.,1.,360.),width=number(s,"arcWidth",12.,1.,50.);
    const auto arcOpacity=number(s,"arcOpacity",1.,0.,1.);
    const auto minimum=number(data,"rangeMinimum",0.),maximum=number(data,"rangeMaximum",10.);
    const bool validRange=maximum>minimum;
    const auto colors=stops(s,minimum,validRange?maximum:minimum+1);
    if(flag(s,"showOuterRing")){
        p.setPen(QPen(faded(color(s,"tickColor",QColor("#c3d0db")),arcOpacity*.22),1.));p.setBrush(Qt::NoBrush);p.drawPath(arc(center,radius+7,start,sweep));
    }
    if(flag(s,"showInnerRing",false)){
        p.setPen(QPen(faded(color(s,"tickColor",QColor("#c3d0db")),arcOpacity*.22),1.));p.setBrush(Qt::NoBrush);p.drawPath(arc(center,radius-12,start,sweep));
    }
    if(flag(s,"showArc"))gradientArc(p,center,radius,start,sweep,width,colors,arcOpacity);
    const int minor=int(number(s,"minorTickCount",4.,1.,10.));
    const auto majorValues=majorTickValues(minimum,maximum,s);
    const auto tickLabels=majorTickLabels(majorValues,s);
    if(validRange) for(qsizetype i=0;i<majorValues.size();++i){
        const qreal fraction=(majorValues[i]-minimum)/(maximum-minimum),angle=start+sweep*fraction;
        if(flag(s,"showTicks")){
            p.setPen(QPen(faded(color(s,"tickColor",QColor("#c3d0db")),arcOpacity),number(s,"tickWidth",2.,.5,8.),Qt::SolidLine,Qt::RoundCap));
            p.drawLine(polar(center,radius+15,angle),polar(center,radius+15+number(s,"majorTickLength",13.,0.,30.),angle));
            if(i+1<majorValues.size())for(int j=1;j<minor;++j){
                const auto next=(majorValues[i+1]-minimum)/(maximum-minimum);
                const auto a=start+sweep*(fraction+(next-fraction)*j/minor);
                p.drawLine(polar(center,radius+15,a),polar(center,radius+15+number(s,"minorTickLength",6.,0.,20.),a));
            }
        }
        if(flag(s,"showTickLabels")){
            auto ts=textStyle(s,"tickText");ts["fontSize"]=number(s,"tickLabelSize",20.,6.,40.);ts["opacity"]=arcOpacity;
            const auto pos=polar(center,radius+46,angle);
            text(p,tickLabels[i],QRectF(pos.x()-43,pos.y()-16,86,32),ts,color(s,"tickLabelColor",QColor("#cbd5df")),20.);
        }
    }
    bool ok=false;const auto current=data.value("current").toDouble(&ok);
    const bool valid=ok&&std::isfinite(current)&&data.value("currentValid").toBool()&&validRange;
    const auto fraction=valid?std::clamp((current-minimum)/(maximum-minimum),0.,1.):0.;
    const auto indicator=s.value("indicatorStyle","marker").toString();
    if(valid&&indicator!="none"){
        p.save();p.setOpacity(p.opacity()*number(s,"indicatorOpacity",1.,0.,1.));
        const auto c=color(s,"indicatorColor",QColor("#f5fbff"));const auto a=start+sweep*fraction;
        const auto thickness=number(s,"indicatorWidth",4.,1.,20.);
        p.setBrush(c);p.setPen(QPen(c,thickness,Qt::SolidLine,Qt::RoundCap));
        if(indicator=="dot")p.drawEllipse(polar(center,radius,a),width*.65,width*.65);
        else if(indicator=="needle"){
            p.drawLine(polar(center,94,a),polar(center,radius-20,a));p.setPen(Qt::NoPen);p.drawEllipse(polar(center,94,a),thickness,thickness);
        }else if(indicator=="progress")gradientArc(p,center,radius-18,start,sweep,thickness,colors,1.,fraction);
        else{
            p.drawLine(polar(center,radius-13,a),polar(center,radius+11,a));
            p.setPen(QPen(QColor(0,0,0,90),1));p.drawEllipse(polar(center,radius,a),thickness*.85,thickness*.85);
        }
        p.restore();
    }
    p.save();p.setOpacity(p.opacity()*number(s,"textOpacity",1.,0.,1.));
    const int places=int(number(s,"decimalPlaces",2.,0.,8.));
    const QString unit=flag(s,"showUnit")?data.value("unit","m/s").toString():QString();
    auto drawMetric=[&](const char *key,const char *styleKey,const char *labelKey,const QString &label,qreal y,qreal size,QColor c){
        auto ts=textStyle(s,styleKey);const auto h=number(ts,"fontSize",size,4.,160.)*1.3;
        const auto x=number(ts,"x",72.,-500.,800.),top=number(ts,"y",y,-500.,1000.);
        const auto rect=QRectF(x,top,number(ts,"width",456.,10.,1000.),h);
        const auto prefix=s.value(QLatin1String(labelKey),label).toString();
        const QString content=(prefix.isEmpty()?QString():prefix+"  ")+valueLabel(data,QLatin1String(key),places);
        const auto align=ts.value("alignment","center").toString();
        text(p,content,rect,ts,c,size,key==QByteArray("current")?QFont::DemiBold:QFont::Medium,
             align=="left"?Qt::AlignLeft:align=="right"?Qt::AlignRight:Qt::AlignHCenter);
    };
    if(flag(s,"showMaximum"))drawMetric("maximum","maxText","maximumLabel","Max. Vel.",151,27,color(s,"maxColor",QColor("#ff8269")));
    if(flag(s,"showCurrent")){
        drawMetric("current","currentText","currentLabel","",206,70,color(s,"textColor",QColor("#f7fafc")));
        auto ts=textStyle(s,"unitText");text(p,unit,QRectF(110,288,380,28),ts,color(s,"titleColor",QColor("#d7e1e9")),19.);
    }
    if(flag(s,"showAverage"))drawMetric("average","averageText","averageLabel","Avg. Vel.",326,27,color(s,"averageColor",QColor("#61cfe4")));
    const auto title=s.value("title").toString();auto titleStyle=textStyle(s,"titleText");
    text(p,title,QRectF(65,number(titleStyle,"y",439.),470,40),titleStyle,color(s,"titleColor",QColor("#d7e1e9")),27,QFont::Medium);
    text(p,s.value("subtitle").toString(),QRectF(60,480,480,24),textStyle(s,"subtitleText"),color(s,"titleColor",QColor("#d7e1e9")),16.);
    if(data.value("overflow").toBool()){
        auto warning=textStyle(s,"warningText");text(p,QStringLiteral("OUT OF RANGE · 超量程"),QRectF(90,385,420,24),warning,color(s,"maxColor",QColor("#ff8269")),15,QFont::Medium);
    }
    if(!valid){auto warning=textStyle(s,"warningText");text(p,QStringLiteral("No synchronized sample · 无同步数据"),QRectF(65,385,470,24),warning,color(s,"titleColor",QColor("#d7e1e9")),14.);}
    p.restore();
    // Optional data.plot is the same source's peak-preserving display series.
    // A preset alone never invents a curve; missing real samples leave it empty.
    if(flag(s,"showCurve",false)&&validRange){
        const auto points=data.value("plot").toList();QPainterPath path;bool open=false;qint64 first=0,last=0;
        if(!points.isEmpty()){first=points.first().toMap().value("stampNs").toString().toLongLong();last=points.last().toMap().value("stampNs").toString().toLongLong();}
        if(last>first)for(const auto &point:points){const auto m=point.toMap();bool valueOk=false;const auto value=m.value("value").toDouble(&valueOk);
            if(!valueOk||!std::isfinite(value)||(m.contains("valid")&&!m.value("valid").toBool())||m.value("breakBefore").toBool()){
                open=false;if(!valueOk||!std::isfinite(value)||(m.contains("valid")&&!m.value("valid").toBool()))continue;
            }
            const auto stamp=m.value("stampNs").toString().toLongLong();
            const QPointF pos(78+444*double(stamp-first)/double(last-first),427-55*std::clamp((value-minimum)/(maximum-minimum),0.,1.));
            if(open)path.lineTo(pos);else path.moveTo(pos);open=true;}
        p.setPen(QPen(color(s,"curveColor",QColor("#61cfe4")),number(s,"curveWidth",2.,.5,12.)));p.setBrush(Qt::NoBrush);p.drawPath(path);
    }
    p.restore();
}

QVariantMap SpeedGaugeItem::writeElement(const QString &path,const QSize &size,int dpi,const QString &requested,
                                        const QVariantMap &data,const QVariantMap &style)
{
    if(path.isEmpty())return failure(QStringLiteral("Choose a local destination · 请选择本地输出路径"));
    if(size.width()<16||size.height()<16||size.width()>32768||size.height()>32768||qint64(size.width())*size.height()>100000000)
        return failure(QStringLiteral("Element size must be 16–32768 px and at most 100 MP · 尺寸超过资源限制"));
    if(dpi<36||dpi>2400)return failure(QStringLiteral("DPI must be between 36 and 2400"));
    auto format=requested.trimmed().toLower();if(format.isEmpty())format=QFileInfo(path).suffix().toLower();
    if(format=="tif")format="tiff";
    if(format!="png"&&format!="tiff"&&format!="svg"&&format!="pdf")return failure(QStringLiteral("Supported formats: PNG, TIFF, SVG, PDF"));
    auto suffix=QFileInfo(path).suffix().toLower();if(suffix=="tif")suffix="tiff";
    if(suffix!=format)return failure(QStringLiteral("File extension must match the selected image format · 文件扩展名必须与输出格式一致"));
    const auto source=data.value("provenance").toMap().value("source").toString();
    const QUrl sourceUrl(source);const auto sourcePath=sourceUrl.isLocalFile()?sourceUrl.toLocalFile():source;
    if(!sourcePath.isEmpty()&&QFileInfo(sourcePath).absoluteFilePath().compare(QFileInfo(path).absoluteFilePath(),Qt::CaseInsensitive)==0)
        return failure(QStringLiteral("An export cannot overwrite its actual data source · 不可覆盖真实数据源"));
    QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))return failure(file.errorString());
    if(format=="svg"){
        QSvgGenerator svg;svg.setOutputDevice(&file);svg.setSize(size);svg.setViewBox(QRect(QPoint(),size));svg.setResolution(dpi);
        svg.setTitle(QStringLiteral("UAV Studio speed meter"));svg.setDescription(QStringLiteral("Vector paths from the shared speed meter renderer; no embedded raster image."));
        QPainter painter;if(!painter.begin(&svg))return failure(QStringLiteral("Cannot initialize SVG painter"));
        drawGauge(painter,size,data,style);if(!painter.end())return failure(QStringLiteral("Cannot finish SVG painter"));
    }else if(format=="pdf"){
        QPdfWriter pdf(&file);pdf.setResolution(dpi);pdf.setPageMargins(QMarginsF(0,0,0,0));
        pdf.setPageSize(QPageSize(QSizeF(size.width()*25.4/dpi,size.height()*25.4/dpi),QPageSize::Millimeter,
                                   QStringLiteral("Speed meter"),QPageSize::ExactMatch));
        pdf.setTitle(QStringLiteral("Speed meter"));pdf.setCreator(QStringLiteral("UAV Studio"));
        QPainter painter;if(!painter.begin(&pdf))return failure(QStringLiteral("Cannot initialize PDF painter"));
        drawGauge(painter,QSizeF(pdf.width(),pdf.height()),data,style);if(!painter.end())return failure(QStringLiteral("Cannot finish PDF painter"));
    }else{
        QImage image(size,QImage::Format_ARGB32_Premultiplied);if(image.isNull())return failure(QStringLiteral("Not enough memory for requested element resolution"));
        image.fill(Qt::transparent);const int dpm=qRound(dpi/0.0254);image.setDotsPerMeterX(dpm);image.setDotsPerMeterY(dpm);
        QPainter painter(&image);drawGauge(painter,size,data,style);painter.end();
        QImageWriter writer(&file,format.toLatin1());writer.setQuality(100);if(!writer.write(image))return failure(writer.errorString());
    }
    if(!file.commit())return failure(file.errorString());
    const auto ticks=majorTickValues(number(data,"rangeMinimum",0.),number(data,"rangeMaximum",10.),style);
    return {{"success",true},{"path",path},{"width",size.width()},{"height",size.height()},{"dpi",dpi},
            {"format",format},{"vector",format=="svg"||format=="pdf"},{"embeddedRaster",false},
            {"transparentBackground",style.value("backgroundMode","transparent").toString()=="transparent"},
            {"targetNs",data.value("targetNs",data.value("stampNs"))},{"unit",data.value("unit","m/s")},
            {"majorTickLabels",majorTickLabels(ticks,style)}};
}

QString SpeedGaugeItem::exportElement(const QUrl &destination,const QSize &size,int dpi,const QString &format)
{
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(exporting()||!destination.isLocalFile()){
        const auto error=exporting()?QStringLiteral("Another speed meter export is running · 仪表导出正在进行"):
                                    QStringLiteral("Choose a local file URL · 请选择本地文件");
        QMetaObject::invokeMethod(this,[this,id,error]{emit exportFinished(id,false,{},error);},Qt::QueuedConnection);return id;
    }
    jobId_=id;exporting_=true;const auto path=destination.toLocalFile();const auto data=data_,style=style_;
    watcher_.setFuture(QtConcurrent::run([=]{
        try{return writeElement(path,size,dpi,format,data,style);}
        catch(const std::exception &error){return failure(QString::fromUtf8(error.what()));}
        catch(...){return failure(QStringLiteral("Speed meter export failed · 仪表导出失败"));}
    }));emit exportingChanged();return id;
}

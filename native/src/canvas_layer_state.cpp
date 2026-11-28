#include "canvas_layer_state.h"
#include <algorithm>
#include <cmath>
#include <QFileInfo>
#include <QPainterPath>
#include <QUrl>
#include <QDir>
#include <QImageReader>
#include <QImageIOHandler>
#include <QCache>
#include <QDataStream>
#include <QGlyphRun>
#include <QRawFont>
#include <QTextLayout>
namespace replay {
namespace {
QString localImagePath(const QString &source)
{
    const QUrl url(source);return QDir::isAbsolutePath(source)?source:url.isLocalFile()?url.toLocalFile():url.scheme().isEmpty()?source:QString();
}
QPainterPath textOutline(const QString &text,const QFont &font,const QRectF &bounds,int flags)
{
    // Only immutable paths are cached, separately on each render thread. Font
    // fallback, bidirectional text, ligatures and wrapping come from Qt's actual
    // shaped glyph runs, rather than one addText call per Unicode character.
    QByteArray key;QDataStream stream(&key,QIODevice::WriteOnly);stream<<text<<font.toString()<<bounds<<flags;
    thread_local QCache<QByteArray,QPainterPath> cache(128*1024);
    if(const auto *cached=cache.object(key))return *cached;
    auto paragraph=text;paragraph.replace("\r\n","\n");paragraph.replace('\r','\n');paragraph.replace('\n',QChar::LineSeparator);
    QTextLayout layout(paragraph,font);QTextOption option;option.setWrapMode(QTextOption::WordWrap);
    option.setAlignment(Qt::Alignment(flags)&Qt::AlignHorizontal_Mask);layout.setTextOption(option);
    layout.beginLayout();qreal height=0.;
    while(true){auto line=layout.createLine();if(!line.isValid())break;line.setLineWidth(bounds.width());line.setPosition({0.,height});height+=line.height();}
    layout.endLayout();
    const auto offset=bounds.topLeft()+QPointF(0.,flags&Qt::AlignBottom?bounds.height()-height:flags&Qt::AlignVCenter?(bounds.height()-height)/2.:0.);
    QPainterPath path;path.setFillRule(Qt::WindingFill);
    for(const auto &run:layout.glyphRuns()){
        const auto glyphs=run.glyphIndexes();const auto positions=run.positions();const auto raw=run.rawFont();
        for(qsizetype i=0;i<std::min(glyphs.size(),positions.size());++i){auto glyph=raw.pathForGlyph(glyphs[i]);glyph.translate(offset+positions[i]);path.addPath(glyph);}
    }
    cache.insert(key,new QPainterPath(path),std::max(1,path.elementCount()));return path;
}
void paintTextShadow(QPainter &p,const QPainterPath &path,const QColor &color,double opacity,double dx,double dy,double blur)
{
    if(path.isEmpty()||color.alpha()==0||opacity<=0.)return;
    p.save();
    if(blur<=0.){auto tint=color;tint.setAlphaF(tint.alphaF()*opacity);p.fillPath(path.translated(dx,dy),tint);}
    else{
        // A finite, normalized Gaussian approximation keeps the shadow native
        // vector paths on SVG/PDF at every resolution. Account for inherited
        // opacity once, instead of darkening overlapping samples independently.
        constexpr double weights[]{1./16.,4./16.,6./16.,4./16.,1./16.};
        const auto alpha=std::clamp(color.alphaF()*opacity*p.opacity(),0.,1.);p.setOpacity(1.);
        for(int y=0;y<5;++y)for(int x=0;x<5;++x){
            auto tint=color;tint.setAlphaF(1.-std::pow(1.-alpha,weights[x]*weights[y]));
            p.fillPath(path.translated(dx+(x-2)*blur/2.,dy+(y-2)*blur/2.),tint);
        }
    }
    p.restore();
}
}
QVariantMap canvasItemSettings(const QVariantMap &state,const QString &id,double time,const QVariantMap &defaults)
{
    auto result=defaults;const auto saved=state.value("items").toMap().value(id).toMap();
    for(auto i=saved.begin();i!=saved.end();++i)result[i.key()]=i.value();
    auto frames=result.value("keyframes").toList();
    frames.removeIf([](const QVariant &v){bool ok=false;const auto t=v.toMap().value("time").toDouble(&ok);return !ok||!std::isfinite(t)||t<0.;});
    if(time<0.||!std::isfinite(time)||frames.isEmpty())return result;
    std::stable_sort(frames.begin(),frames.end(),[](const auto &a,const auto &b){return a.toMap().value("time").toDouble()<b.toMap().value("time").toDouble();});
    // Each animated property has its own keyframe track. A keyframe which only
    // changes opacity must not reset the ongoing position interpolation.
    for(const auto &key:{"x","y","width","height","rotation","opacity"}){
        QVariantMap left,right;
        for(const auto &frame:frames){const auto current=frame.toMap();if(!current.contains(key))continue;
            bool ok=false;const auto n=current.value(key).toDouble(&ok);if(!ok||!std::isfinite(n))continue;
            if(current.value("time").toDouble()<=time)left=current;else{right=current;break;}}
        if(left.isEmpty())continue;
        const auto first=left.value(key).toDouble(),a=left.value("time").toDouble(),b=right.value("time",a).toDouble();
        const auto fraction=b>a?std::clamp((time-a)/(b-a),0.,1.):0.;result[key]=first+(right.value(key,first).toDouble()-first)*fraction;
    }
    return result;
}
QRectF canvasItemGeometry(const QVariantMap &state,const QString &id,const QRectF &base,const QSizeF &canvas,double time,const QVariantMap &defaults)
{
    const auto item=canvasItemSettings(state,id,time,defaults);
    return {item.contains("x")?item.value("x").toDouble()*canvas.width():base.x(),item.contains("y")?item.value("y").toDouble()*canvas.height():base.y(),
            item.contains("width")?item.value("width").toDouble()*canvas.width():base.width(),item.contains("height")?item.value("height").toDouble()*canvas.height():base.height()};
}
bool canvasItemActive(const QVariantMap &state,const QString &id,double time,bool exists,const QVariantMap &defaults)
{
    if(!exists||!std::isfinite(time))return false;const auto item=canvasItemSettings(state,id,-1.,defaults);
    if(item.value("deleted").toBool()||!item.value("visible",true).toBool())return false;const auto ranges=item.value("visibilityRanges").toList();
    if(ranges.isEmpty())return !item.value("visibilityRangesEnabled").toBool();
    for(const auto &v:ranges){const auto range=v.toList();if(range.size()==2&&time>=range[0].toDouble()&&time<range[1].toDouble())return true;}return false;
}
void applyCanvasItemTransform(QPainter &p,const QVariantMap &s,const QRectF &geometry,double baseOpacity)
{
    p.translate(geometry.x(),geometry.y());const auto ax=s.value("anchorX",.5).toDouble()*geometry.width(),ay=s.value("anchorY",.5).toDouble()*geometry.height();
    p.translate(ax,ay);p.rotate(s.value("rotation",0.).toDouble());p.translate(-ax,-ay);p.translate(geometry.width()/2,geometry.height()/2);
    p.scale(s.value("flipH").toBool()?-1.:1.,s.value("flipV").toBool()?-1.:1.);p.translate(-geometry.width()/2,-geometry.height()/2);
    p.setOpacity(p.opacity()*std::clamp(s.value("opacity",baseOpacity).toDouble(),0.,1.));
}
void paintCanvasItemAppearance(QPainter &p,const QVariantMap &s,const QSizeF &size,double pixelsPerOutputUnit)
{
    if(size.isEmpty()||!std::isfinite(pixelsPerOutputUnit)||pixelsPerOutputUnit<=0.)return;
    const auto number=[&](const QString &key,double fallback){bool ok=false;const auto n=s.value(key,fallback).toDouble(&ok);return ok&&std::isfinite(n)?n:fallback;};
    const auto mode=s.value("backgroundMode").toString();
    QColor fill((mode=="solid"||mode=="translucent")?s.value("backgroundColor","transparent").toString():"transparent");
    if(!fill.isValid())fill=Qt::transparent;QColor border(s.value("borderColor","transparent").toString());if(!border.isValid())border=Qt::transparent;
    // QtQuick treats a fully transparent pen as an absent border.
    const auto width=border.alpha()>0?std::max(0.,number("borderWidth",0.)*pixelsPerOutputUnit):0.,opacity=std::clamp(number("backgroundOpacity",1.),0.,1.);
    if(opacity<=0.||(fill.alpha()==0&&(width<=0.||border.alpha()==0)))return;
    const auto radius=std::clamp(number("cornerRadius",0.)*pixelsPerOutputUnit,0.,std::min(size.width(),size.height())/2.);
    QPainterPath outer;outer.addRoundedRect(QRectF(QPointF(),size),radius,radius);
    p.save();p.setRenderHint(QPainter::Antialiasing);p.setOpacity(p.opacity()*opacity);
    if(width>0.){
        const auto inside=QRectF(QPointF(),size).adjusted(width,width,-width,-width);QPainterPath rim=outer;
        if(!inside.isEmpty()){QPainterPath hole;hole.addRoundedRect(inside,std::max(0.,radius-width),std::max(0.,radius-width));rim=outer.subtracted(hole);p.fillPath(hole,fill);}
        p.fillPath(rim,border);
    }else p.fillPath(outer,fill);
    p.restore();
}
QImage loadCanvasElementImage(const QString &source,QString *error)
{
    const auto path=localImagePath(source);if(path.isEmpty()){if(error)*error=QStringLiteral("Select a local image · 请选择本地图片");return {};}
    QImageReader reader(path);reader.setAutoTransform(true);auto image=reader.read();if(error)*error=image.isNull()?reader.errorString():QString();return image;
}
QSize canvasElementImageSize(const QString &source)
{
    const auto path=localImagePath(source);if(path.isEmpty())return {};QImageReader reader(path);reader.setAutoTransform(true);
    auto size=reader.size();if(reader.transformation()&QImageIOHandler::TransformationRotate90)size.transpose();return size;
}
QStringList canvasElementKinds(){return {"text","image","arrow","circle","rectangle"};}
QVariantMap canvasElementDefaults(const QString &kind)
{
    if(!canvasElementKinds().contains(kind))return {};
    // These are output-content defaults, independent of application appearance.
    QVariantMap style{{"fill","transparent"},{"stroke","#2563eb"},{"strokeWidth",4.},{"color","#1f2937"},
        {"fontFamily","Segoe UI"},{"fontSize",36.},{"fontWeight",400},{"alignment","left"},{"verticalAlignment","middle"},
        {"textStrokeColor","#00000000"},{"textStrokeWidth",0.},{"textShadowColor","#000000"},{"textShadowOpacity",0.},
        {"textShadowOffsetX",3.},{"textShadowOffsetY",3.},{"textShadowBlur",4.},
        {"headSize",20.},{"radius",0.},{"imageFit","contain"}};
    QVariantMap row{{"kind",kind},{"scientific",true},{"name",kind},{"x",.1},{"y",.1},{"width",.25},{"height",.18},
        {"rotation",0.},{"opacity",1.},{"visible",true},{"anchorX",.5},{"anchorY",.5},{"style",style}};
    if(kind=="text"){row["text"]="Text · 文字";row["width"]=.35;row["height"]=.12;}
    else if(kind=="image")row["width"]=.32;
    else if(kind=="arrow"){row["width"]=.24;row["height"]=.12;}
    else if(kind=="circle"){row["width"]=.15;row["height"]=.2666666666666667;}
    return row;
}
bool paintCanvasElement(QPainter &p,const QVariantMap &element,const QSizeF &size,const QImage &image)
{
    const auto kind=element.value("kind").toString();if(!canvasElementKinds().contains(kind)||size.isEmpty())return false;
    const auto defaults=canvasElementDefaults(kind);auto style=defaults.value("style").toMap();const auto edits=element.value("style").toMap();
    for(auto i=edits.cbegin();i!=edits.cend();++i)style[i.key()]=i.value();
    const auto designWidth=element.value("designWidth",1920.).toDouble(),normalizedWidth=element.value("width",defaults.value("width")).toDouble();
    const auto scale=designWidth>0.&&normalizedWidth>0.?size.width()/(designWidth*normalizedWidth):1.;
    const auto color=[&](const QString &key,const QColor &fallback){const QColor value(style.value(key).toString());return value.isValid()?value:fallback;};
    const auto number=[&](const QString &key,double fallback){bool ok=false;const auto n=style.value(key,fallback).toDouble(&ok);return ok&&std::isfinite(n)?n:fallback;};
    p.save();p.setRenderHints(QPainter::Antialiasing|QPainter::TextAntialiasing|QPainter::SmoothPixmapTransform);
    const auto stroke=std::max(0.,number("strokeWidth",4.)*scale);QPen pen(color("stroke",QColor("#2563eb")),stroke,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin);
    p.setPen(stroke>0.?pen:QPen(Qt::NoPen));p.setBrush(color("fill",Qt::transparent));
    const QRectF bounds(QPointF(),size);p.setClipRect(bounds,Qt::IntersectClip);const auto shapeBounds=bounds.adjusted(stroke/2.,stroke/2.,-stroke/2.,-stroke/2.);
    if(kind=="text"){
        QFont font(style.value("fontFamily","Segoe UI").toString());font.setPixelSize(std::max(1,int(std::lround(number("fontSize",36.)*scale))));
        font.setWeight(QFont::Weight(std::clamp(int(number("fontWeight",400.)),100,900)));font.setItalic(style.value("italic").toBool());
        p.setFont(font);p.setPen(color("color",QColor("#1f2937")));
        const auto align=style.value("alignment").toString(),vertical=style.value("verticalAlignment").toString();
        const auto flags=Qt::TextWordWrap|(align=="center"?Qt::AlignHCenter:align=="right"?Qt::AlignRight:Qt::AlignLeft)|
            (vertical=="top"?Qt::AlignTop:vertical=="bottom"?Qt::AlignBottom:Qt::AlignVCenter);
        const auto text=element.value("text").toString();const auto outlineColor=color("textStrokeColor",Qt::transparent),shadowColor=color("textShadowColor",Qt::black);
        const auto outlineWidth=outlineColor.alpha()>0?std::clamp(number("textStrokeWidth",0.),0.,64.)*scale:0.;
        const auto shadowOpacity=shadowColor.alpha()>0?std::clamp(number("textShadowOpacity",0.),0.,1.):0.;
        if(outlineWidth<=0.&&shadowOpacity<=0.)p.drawText(bounds,int(flags),text);
        else{
            // Shape once in the item's design coordinates. Recreating a font
            // and QTextLayout at each output pixel size changes hinted advances,
            // fallback line metrics and wrapping, so high-resolution exports no
            // longer have the same geometry as the preview. Scale the real Qt
            // glyph paths and all effects together on the output device instead.
            font.setPixelSize(std::max(1,int(std::lround(number("fontSize",36.)))));
            const QRectF designBounds(QPointF(),size/scale);
            const auto designOutlineWidth=outlineWidth/scale;
            const auto blur=shadowOpacity>0.?std::clamp(number("textShadowBlur",4.),0.,64.):0.;
            const auto dx=shadowOpacity>0.?std::clamp(number("textShadowOffsetX",3.),-512.,512.):0.,dy=shadowOpacity>0.?std::clamp(number("textShadowOffsetY",3.),-512.,512.):0.;
            // Keep the full effect inside the editable item, so live item
            // textures and export devices share the same unclipped geometry.
            const auto inset=designOutlineWidth/2.;const auto textBounds=designBounds.adjusted(std::max(inset,blur-dx),std::max(inset,blur-dy),-std::max(inset,blur+dx),-std::max(inset,blur+dy));
            if(!textBounds.isEmpty()){
                const auto path=textOutline(text,font,textBounds,int(flags));
                p.scale(scale,scale);
                paintTextShadow(p,path,shadowColor,shadowOpacity,dx,dy,blur);
                if(designOutlineWidth>0.)p.strokePath(path,QPen(outlineColor,designOutlineWidth,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
                // Fill the same shaped outlines, so effects and text cannot
                // drift when the raster font engine applies glyph hinting.
                p.fillPath(path,color("color",QColor("#1f2937")));
            }
        }
    }else if(kind=="image"){
        if(image.isNull()){p.restore();return false;}
        const auto fit=style.value("imageFit","contain").toString();
        if(fit=="stretch")p.drawImage(bounds,image);
        else{const auto factor=fit=="cover"?std::max(size.width()/image.width(),size.height()/image.height()):std::min(size.width()/image.width(),size.height()/image.height());
            const QSizeF drawn(image.width()*factor,image.height()*factor);p.setClipRect(bounds,Qt::IntersectClip);p.drawImage(QRectF(QPointF((size.width()-drawn.width())/2.,(size.height()-drawn.height())/2.),drawn),image);}
    }else if(kind=="circle")p.drawEllipse(shapeBounds);
    else if(kind=="rectangle"){
        const auto radius=std::max(0.,number("radius",0.)*scale);p.drawRoundedRect(shapeBounds,radius,radius);
    }else if(kind=="arrow"){
        const auto head=std::min(std::max(0.,number("headSize",20.)*scale),size.width()/2.);
        const QPointF start(stroke/2.,size.height()/2.),tip(size.width()-stroke/2.,size.height()/2.);
        p.drawLine(start,tip);QPainterPath path;path.moveTo(tip+QPointF(-head,-head*.55));path.lineTo(tip);path.lineTo(tip+QPointF(-head,head*.55));p.drawPath(path);
    }
    p.restore();return true;
}
void paintCanvasElements(QPainter &p,const QVariantMap &state,const QSizeF &size,double time,const QHash<QString,QImage> &images)
{
    auto elements=state.value("elements").toList();std::stable_sort(elements.begin(),elements.end(),[&](const auto &a,const auto &b){
        const auto first=a.toMap(),last=b.toMap();return canvasItemSettings(state,first.value("id").toString(),time,first).value("z",600.).toDouble()<canvasItemSettings(state,last.value("id").toString(),time,last).value("z",600.).toDouble();});
    for(const auto &v:elements){const auto base=v.toMap();const auto id=base.value("id").toString();if(!canvasItemActive(state,id,time,true,base))continue;
        const auto element=canvasItemSettings(state,id,time,base);const auto geometry=canvasItemGeometry(state,id,QRectF(),size,time,base);
        QImage image;if(element.value("kind")=="image"){
            const auto source=element.value("source").toString();image=images.value(id,images.value(source));
            if(image.isNull())image=loadCanvasElementImage(source);
        }
        const auto referenceWidth=state.value("outputWidth",1920.).toDouble();
        p.save();applyCanvasItemTransform(p,element,geometry);paintCanvasItemAppearance(p,element,geometry.size(),referenceWidth>0.?size.width()/referenceWidth:1.);paintCanvasElement(p,element,geometry.size(),image);p.restore();
    }
}
}

#include "canvas_layer_model.h"
#include <QUuid>
#include <QFileInfo>
#include <QImageReader>
#include <QUrl>
#include <QDir>
#include <algorithm>
#include <cmath>

namespace {
QVariantMap normalizedElement(const QString &kind,const QVariantMap &properties,int canvasWidth,int canvasHeight)
{
    auto value=replay::canvasElementDefaults(kind);if(value.isEmpty())return {};
    for(auto i=properties.cbegin();i!=properties.cend();++i)value[i.key()]=i.value();
    value["kind"]=kind;value["scientific"]=true;
    value["designWidth"]=value.value("designWidth",canvasWidth);
    auto style=replay::canvasElementDefaults(kind).value("style").toMap();
    const auto custom=properties.value("style").toMap();for(auto i=custom.cbegin();i!=custom.cend();++i)style[i.key()]=i.value();value["style"]=style;
    for(const auto &key:{"x","y","width","height","rotation","opacity","anchorX","anchorY","z","designWidth"})if(value.contains(key)){
        bool ok=false;const auto n=value.value(key).toDouble(&ok);if(!ok||!std::isfinite(n))return {};
        if((QString(key)=="width"||QString(key)=="height")&&(n<.0001||n>10.))return {};
        if(QString(key)=="opacity"&&(n<0.||n>1.))return {};
        if(QString(key)=="designWidth"&&(n<16.||n>32768.))return {};
    }
    for(const auto &key:{"fill","stroke","color"})if(style.contains(key)&&!QColor(style.value(key).toString()).isValid())return {};
    for(const auto &key:{"fontSize","strokeWidth","headSize","radius"})if(style.contains(key)){
        bool ok=false;const auto n=style.value(key).toDouble(&ok);if(!ok||!std::isfinite(n)||n<0.||n>4096.)return {};
    }
    if(kind=="text"){
        if(value.value("text").toString().trimmed().isEmpty()||value.value("text").toString().size()>100000)return {};
    }else if(kind=="image"){
        const auto source=value.value("source").toString();const QUrl url(source);
        const auto path=QDir::isAbsolutePath(source)?source:url.isLocalFile()?url.toLocalFile():url.scheme().isEmpty()?source:QString();
        if(path.isEmpty()||!QFileInfo(path).isFile())return {};
        QImageReader reader(path);if(!reader.canRead())return {};
        value["source"]=QFileInfo(path).absoluteFilePath();
        const auto displaySize=replay::canvasElementImageSize(path);
        if(!properties.contains("height")&&displaySize.isValid())value["height"]=std::clamp(value.value("width").toDouble()*canvasWidth*displaySize.height()/(canvasHeight*double(displaySize.width())),.0001,10.);
    }else if(kind=="circle"&&!properties.contains("height"))value["height"]=std::clamp(value.value("width").toDouble()*canvasWidth/double(canvasHeight),.0001,10.);
    return value;
}
}

CanvasLayerModel::CanvasLayerModel(Binding binding,QObject *parent):QObject(parent),binding_(std::move(binding)){}
QVariantMap CanvasLayerModel::state()const{return binding_.getState?binding_.getState():QVariantMap{};}
bool CanvasLayerModel::exists(const QString &id)const
{
    if(id.isEmpty())return false;if(scientificElement(id))return true;if(binding_.exists)return binding_.exists(id);
    const auto rows=binding_.rawLayers?binding_.rawLayers():QVariantList{};
    return std::any_of(rows.begin(),rows.end(),[&](const auto &v){return v.toMap().value("id").toString()==id;});
}
int CanvasLayerModel::outputWidth()const{return state().value("outputWidth",1920).toInt();}
int CanvasLayerModel::outputHeight()const{return state().value("outputHeight",1080).toInt();}
void CanvasLayerModel::setOutputSize(int w,int h){if(w<16||h<16||w>32768||h>32768)return;auto s=state();s["outputWidth"]=w;s["outputHeight"]=h;store(s);}
void CanvasLayerModel::setLayoutMode(bool value){if(value!=layoutMode_){layoutMode_=value;emit modeChanged();}}
void CanvasLayerModel::setSnapping(bool value){if(value!=snapping_){snapping_=value;emit modeChanged();}}
QVariantMap CanvasLayerModel::rawLayer(const QString &id)const
{
    for(const auto &v:state().value("elements").toList()){const auto row=v.toMap();if(row.value("id").toString()==id)return row;}
    const auto rows=binding_.rawLayers?binding_.rawLayers():QVariantList{};
    for(const auto &v:rows){const auto row=v.toMap();if(row.value("id").toString()==id)return row;}return {};
}
bool CanvasLayerModel::scientificElement(const QString &id)const
{
    for(const auto &v:state().value("elements").toList())if(v.toMap().value("id").toString()==id)return true;return false;
}
QVariantMap CanvasLayerModel::settings(const QString &id,double time)const{return replay::canvasItemSettings(state(),id,time,rawLayer(id));}
QRectF CanvasLayerModel::geometry(const QString &id,const QRectF &base,const QSizeF &canvas,double time)const{return replay::canvasItemGeometry(state(),id,base,canvas,time,rawLayer(id));}
bool CanvasLayerModel::active(const QString &id,double time)const{return replay::canvasItemActive(state(),id,time,exists(id),rawLayer(id));}
QVariantList CanvasLayerModel::layers()const
{
    QVariantList result;QStringList seen;auto rows=binding_.rawLayers?binding_.rawLayers():QVariantList{};const auto saved=state();rows.append(saved.value("elements").toList());
    for(const auto &v:rows){const auto raw=v.toMap();const auto id=raw.value("id").toString();if(id.isEmpty()||seen.contains(id))continue;seen.append(id);
        auto s=replay::canvasItemSettings(saved,id,-1.,raw);if(s.value("deleted").toBool())continue;s["id"]=id;s["kind"]=raw.value("kind");s["name"]=s.value("name",raw.value("name",id));
        s["order"]=s.value("z",raw.value("order",0));result.append(s);}
    std::stable_sort(result.begin(),result.end(),[](const auto &a,const auto &b){return a.toMap().value("order").toDouble()>b.toMap().value("order").toDouble();});return result;
}
void CanvasLayerModel::select(const QString &id,bool additive)
{
    QStringList next=additive?selection_:QStringList{};
    if(exists(id)&&!settings(id).value("deleted").toBool()){
        if(additive&&next.contains(id))next.removeAll(id);else next.append(id);const auto group=settings(id).value("group").toString();
        if(!group.isEmpty())for(const auto &v:layers()){const auto row=v.toMap();const auto other=row.value("id").toString();if(row.value("group").toString()==group&&!next.contains(other))next.append(other);}}
    if(next!=selection_){selection_=next;emit selectionChanged();}
}
void CanvasLayerModel::clearSelection(){if(!selection_.isEmpty()){selection_.clear();emit selectionChanged();}}
void CanvasLayerModel::beginEdit(){if(editDepth_++==0){editCancelled_=false;if(binding_.beginEdit)binding_.beginEdit();}}
void CanvasLayerModel::endEdit(bool cancel)
{
    if(editDepth_==0)return;editCancelled_|=cancel;if(--editDepth_>0)return;
    if(binding_.endEdit)binding_.endEdit(editCancelled_);if(editCancelled_)refresh();editCancelled_=false;
}
void CanvasLayerModel::store(const QVariantMap &value,bool catalog)
{
    if((binding_.loading&&binding_.loading())||!binding_.storeState||state()==value)return;binding_.storeState(value);refresh(catalog);
}
void CanvasLayerModel::edit(const QString &id,const QVariantMap &changes)
{
    if(!exists(id))return;auto s=state(),items=s.value("items").toMap(),item=items.value(id).toMap();
    for(auto i=changes.begin();i!=changes.end();++i){
        if(i.key()=="x"||i.key()=="y"||i.key()=="width"||i.key()=="height"||i.key()=="rotation"||i.key()=="opacity"||i.key()=="anchorX"||i.key()=="anchorY"||i.key()=="scale"||i.key()=="z"){
            bool ok=false;const auto n=i.value().toDouble(&ok);if(!ok||!std::isfinite(n))return;
            if((i.key()=="width"||i.key()=="height")&&(n<.0001||n>10.))return;if(i.key()=="opacity"&&(n<0.||n>1.))return;if(i.key()=="scale"&&n<=0.)return;}
        item[i.key()]=i.value();}
    items[id]=item;s["items"]=items;store(s,changes.contains("name")||changes.contains("visible")||changes.contains("locked")||changes.contains("z")||changes.contains("deleted")||changes.contains("group"));
}
void CanvasLayerModel::reset(const QString &id)
{
    if(!exists(id))return;auto s=state(),items=s.value("items").toMap(),item=items.value(id).toMap();
    for(const auto &key:{"x","y","width","height","rotation","anchorX","anchorY","flipH","flipV","scale"})item.remove(key);items[id]=item;s["items"]=items;store(s);
}
void CanvasLayerModel::removeSelected()
{
    // The host may synchronously clear selection during removal.
    const auto ids=selection_;removeElements(ids);
}
void CanvasLayerModel::removeElements(const QStringList &ids)
{
    if(ids.isEmpty()||(binding_.loading&&binding_.loading()))return;
    QStringList hostIds;auto s=state(),items=s.value("items").toMap();auto elements=s.value("elements").toList();bool removed=false;
    for(const auto &id:ids){
        if(scientificElement(id)){
            elements.removeIf([&](const QVariant &v){return v.toMap().value("id").toString()==id;});items.remove(id);removed=true;
        }else if(exists(id)&&!hostIds.contains(id))hostIds.append(id);
    }
    if(!removed&&hostIds.isEmpty())return;beginEdit();
    if(removed){s["elements"]=elements;s["items"]=items;store(s,true);}
    if(binding_.remove&&!hostIds.isEmpty())binding_.remove(hostIds);endEdit();refresh();
}
QString CanvasLayerModel::duplicate(const QString &id)
{
    if(scientificElement(id)){
        auto item=element(id);item.remove("id");item.remove("group");item.remove("deleted");item["name"]=item.value("name").toString()+tr(" Copy");
        item["x"]=item.value("x",0.).toDouble()+.025;item["y"]=item.value("y",0.).toDouble()+.025;
        const auto copies=pasteElements({item});return copies.isEmpty()?QString():copies.first();
    }
    if(!exists(id)||!binding_.duplicate||(binding_.loading&&binding_.loading()))return {};const auto original=settings(id);beginEdit();const auto copy=binding_.duplicate(id);
    if(copy.isEmpty()){endEdit(true);return {};}
    auto s=state(),items=s.value("items").toMap(),item=items.value(id).toMap();item.remove("group");item.remove("deleted");QString name=original.value("name").toString();
    if(name.isEmpty())for(const auto &v:layers()){const auto r=v.toMap();if(r.value("id")==id){name=r.value("name").toString();break;}}
    item["name"]=name+tr(" Copy");if(original.contains("x"))item["x"]=original.value("x").toDouble()+.025;if(original.contains("y"))item["y"]=original.value("y").toDouble()+.025;
    items[copy]=item;s["items"]=items;store(s,true);endEdit();refresh();select(copy);return copy;
}
QStringList CanvasLayerModel::supportedElementKinds()const{return replay::canvasElementKinds();}
QVariantMap CanvasLayerModel::element(const QString &id,double time)const{return scientificElement(id)?settings(id,time):QVariantMap{};}
QString CanvasLayerModel::addElement(const QString &kind,const QVariantMap &properties)
{
    if(binding_.loading&&binding_.loading())return {};
    auto value=normalizedElement(kind,properties,outputWidth(),outputHeight());if(value.isEmpty())return {};
    const auto id="element-"+QUuid::createUuid().toString(QUuid::WithoutBraces);value["id"]=id;
    if(!properties.contains("z")){double top=600.;for(const auto &v:layers())top=std::max(top,v.toMap().value("order").toDouble()+1.);value["z"]=top;}
    auto s=state();auto elements=s.value("elements").toList();elements.append(value);s["elements"]=elements;store(s,true);select(id);return id;
}
void CanvasLayerModel::editElement(const QString &id,const QVariantMap &changes)
{
    if(!scientificElement(id)||(binding_.loading&&binding_.loading()))return;
    auto s=state();auto elements=s.value("elements").toList();
    for(auto &entry:elements){auto current=entry.toMap();if(current.value("id").toString()!=id)continue;current=element(id);
        for(auto i=changes.cbegin();i!=changes.cend();++i){if(i.key()=="id"||i.key()=="kind"||i.key()=="scientific")continue;
            if(i.key()=="style"){auto style=current.value("style").toMap();const auto edits=i.value().toMap();for(auto j=edits.cbegin();j!=edits.cend();++j)style[j.key()]=j.value();current["style"]=style;}
            else current[i.key()]=i.value();}
        current=normalizedElement(current.value("kind").toString(),current,outputWidth(),outputHeight());if(current.isEmpty())return;
        entry=current;break;
    }
    auto items=s.value("items").toMap();items.remove(id);s["items"]=items;s["elements"]=elements;store(s,true);
}
QVariantList CanvasLayerModel::copySelected()const
{
    QVariantList result;for(const auto &id:selection_)if(scientificElement(id)){auto value=element(id);value.remove("id");result.append(value);}return result;
}
QStringList CanvasLayerModel::pasteElements(const QVariantList &values)
{
    if(binding_.loading&&binding_.loading())return {};QVariantList accepted;
    for(const auto &v:values){const auto row=v.toMap();const auto value=normalizedElement(row.value("kind").toString(),row,outputWidth(),outputHeight());if(!value.isEmpty())accepted.append(value);}
    if(accepted.isEmpty())return {};beginEdit();auto s=state();auto elements=s.value("elements").toList();QStringList ids;QMap<QString,QString> groups;
    for(auto &entry:accepted){auto row=entry.toMap();const auto id="element-"+QUuid::createUuid().toString(QUuid::WithoutBraces);row["id"]=id;row.remove("deleted");
        const auto group=row.value("group").toString();if(!group.isEmpty()){if(!groups.contains(group))groups[group]=QUuid::createUuid().toString(QUuid::WithoutBraces);row["group"]=groups[group];}
        elements.append(row);ids.append(id);}
    s["elements"]=elements;store(s,true);endEdit();selection_=ids;emit selectionChanged();return ids;
}
void CanvasLayerModel::reorder(const QString &id,int destination)
{
    auto all=layers();int source=-1;for(int i=0;i<all.size();++i)if(all[i].toMap().value("id")==id)source=i;
    if(source<0||all.isEmpty())return;destination=std::clamp(destination,0,int(all.size())-1);if(source==destination)return;const auto row=all.takeAt(source);all.insert(destination,row);beginEdit();
    for(int i=0;i<all.size();++i)edit(all[i].toMap().value("id").toString(),{{"z",1000+all.size()-i}});endEdit();
}
void CanvasLayerModel::moveLayer(const QString &id,int direction){const auto all=layers();for(int i=0;i<all.size();++i)if(all[i].toMap().value("id")==id){reorder(id,direction==-2?0:direction==2?int(all.size())-1:i+direction);return;}}
void CanvasLayerModel::groupSelection(){if(selection_.size()<2)return;const auto group=QUuid::createUuid().toString(QUuid::WithoutBraces);beginEdit();const auto ids=selection_;for(const auto &id:ids)edit(id,{{"group",group}});endEdit();}
void CanvasLayerModel::ungroupSelection(){beginEdit();const auto ids=selection_;for(const auto &id:ids)edit(id,{{"group",QString()}});endEdit();}
void CanvasLayerModel::addKeyframe(const QString &id,double time,const QVariantMap &values)
{
    if(!exists(id)||!std::isfinite(time)||time<0.)return;auto frames=settings(id).value("keyframes").toList();
    for(const auto &key:{"x","y","width","height","rotation","opacity"})if(values.contains(key)){bool ok=false;const auto n=values.value(key).toDouble(&ok);if(!ok||!std::isfinite(n))return;
        if((QString(key)=="width"||QString(key)=="height")&&(n<.0001||n>10.))return;if(QString(key)=="opacity"&&(n<0.||n>1.))return;}
    for(qsizetype i=frames.size();i-->0;)if(std::abs(frames[i].toMap().value("time").toDouble()-time)<1e-9)frames.removeAt(i);auto frame=values;frame["time"]=time;frames.append(frame);edit(id,{{"keyframes",frames}});
}
void CanvasLayerModel::removeKeyframe(const QString &id,double time){auto frames=settings(id).value("keyframes").toList();for(qsizetype i=frames.size();i-->0;)if(std::abs(frames[i].toMap().value("time").toDouble()-time)<1e-9)frames.removeAt(i);edit(id,{{"keyframes",frames}});}
void CanvasLayerModel::removeInterval(const QString &id,double begin,double end)
{
    if(!exists(id)||!std::isfinite(begin)||!std::isfinite(end)||end<=begin)return;auto s=settings(id);auto ranges=s.value("visibilityRanges").toList();
    if(ranges.isEmpty()&&!s.value("visibilityRangesEnabled").toBool())ranges.append(QVariantList{0.,binding_.duration?binding_.duration():0.});
    QVariantList remaining;for(const auto &v:ranges){const auto r=v.toList();if(r.size()!=2)continue;const auto a=r[0].toDouble(),b=r[1].toDouble();
        if(end<=a||begin>=b)remaining.append(r);else{if(a<begin)remaining.append(QVariantList{a,begin});if(end<b)remaining.append(QVariantList{end,b});}}
    edit(id,{{"visibilityRanges",remaining},{"visibilityRangesEnabled",true}});
}
void CanvasLayerModel::refresh(bool catalog)
{
    ++revision_;emit changed();if(!catalog)return;QStringList existing;for(const auto &v:layers())existing.append(v.toMap().value("id").toString());
    auto next=selection_;next.removeIf([&](const QString &id){return !existing.contains(id);});if(next!=selection_){selection_=next;emit selectionChanged();}emit catalogChanged();
}

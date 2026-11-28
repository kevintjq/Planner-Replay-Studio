#include "canvas_scientific_item.h"
#include "canvas_layer_state.h"
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>

CanvasScientificItem::CanvasScientificItem(QQuickItem *parent):QQuickPaintedItem(parent)
{
    setAntialiasing(true);setOpaquePainting(false);
    connect(&watcher_,&QFutureWatcher<QPair<QImage,QString>>::finished,this,[this]{
        const auto result=watcher_.result();
        if(workingSource_==requestedSource_){image_=result.first;error_=result.second;loadedSource_=workingSource_;update();}
        else loadImage();
        emit imageChanged();
    });
}
CanvasScientificItem::~CanvasScientificItem(){watcher_.waitForFinished();}
void CanvasScientificItem::setDataMap(const QVariantMap &value)
{
    if(data_==value)return;
    data_=value;
    const auto source=data_.value("kind")=="image"?data_.value("source").toString():QString();
    if(source!=requestedSource_){requestedSource_=source;image_={};error_.clear();if(!watcher_.isRunning())loadImage();emit imageChanged();}
    emit dataMapChanged();update();
}
void CanvasScientificItem::setContentOpacity(qreal value)
{
    value=std::isfinite(value)?std::clamp(value,0.,1.):1.;if(contentOpacity_==value)return;
    contentOpacity_=value;emit contentOpacityChanged();update();
}
void CanvasScientificItem::loadImage()
{
    if(requestedSource_.isEmpty()){loadedSource_.clear();image_={};error_.clear();return;}
    workingSource_=requestedSource_;
    const auto source=workingSource_;
    watcher_.setFuture(QtConcurrent::run([source]()->QPair<QImage,QString>{
        QString error;
        auto image=replay::loadCanvasElementImage(source,&error);
        return {image,error};
    }));
}
void CanvasScientificItem::paint(QPainter *painter)
{
    if(!painter)return;
    painter->save();painter->setOpacity(painter->opacity()*contentOpacity_);
    painter->setRenderHints(QPainter::Antialiasing|QPainter::TextAntialiasing|QPainter::SmoothPixmapTransform);
    replay::paintCanvasElement(*painter,data_,boundingRect().size(),image_);
    painter->restore();
}

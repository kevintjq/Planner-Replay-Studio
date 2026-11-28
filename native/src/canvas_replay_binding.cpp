#include "canvas_layer_model.h"
#include "replay_controller.h"
#include <QJsonArray>
#include <QUuid>
#include <memory>
#include <optional>

CanvasLayerModel::CanvasLayerModel(ReplayController &replay):CanvasLayerModel(replayBinding(replay),&replay)
{
    for(auto signal:{&ReplayController::videoLayersChanged,&ReplayController::cloudViewsChanged,&ReplayController::sceneLayersChanged,&ReplayController::recordingChanged})
        connect(&replay,signal,this,[this]{refresh();});
}
CanvasLayerModel::Binding CanvasLayerModel::replayBinding(ReplayController &replay)
{
    struct Edit{std::optional<replay::ProjectDocument> before;std::shared_ptr<const replay::Recording> recording;};
    const auto transaction=std::make_shared<Edit>();Binding b;
    b.getState=[&replay]{return replay.project_?replay.project_->canvasState():QVariantMap{};};
    b.loading=[&replay]{return replay.loading();};b.duration=[&replay]{return replay.duration();};
    b.beginEdit=[&replay,transaction]{transaction->before=replay.projectSnapshot();transaction->recording=replay.record_;++replay.projectEditDepth_;};
    b.endEdit=[&replay,transaction](bool cancel){
        if(!transaction->before)return;
        --replay.projectEditDepth_;
        // A recording switch invalidates an unfinished drag transaction.
        if(transaction->recording==replay.record_){if(cancel){replay.project_=*transaction->before;replay.refreshProjectModels();}else replay.recordProjectEdit(*transaction->before);}
        transaction->before.reset();transaction->recording.reset();
    };
    b.storeState=[&replay,transaction](const QVariantMap &s){
        if(replay.loading())return;const auto before=replay.projectSnapshot();if(!replay.project_)replay.project_=before;
        if(replay.project_->canvasState()==s)return;replay.project_->setCanvasState(s);
        if(!transaction->before||transaction->recording!=replay.record_)replay.recordProjectEdit(before);
    };
    b.rawLayers=[&replay]{
        QVariantList result;const auto append=[&](const QString &id,const QString &name,const QString &kind,int order){result.append(QVariantMap{{"id",id},{"name",name},{"kind",kind},{"order",order}});};
        if(!replay.project_||replay.project_->composition().value("has_main_view").toBool(true))append("main",tr("Main 3D · 主三维视角"),"main",100);
        int order=200;for(const auto &v:replay.videoLayers()){const auto r=v.toMap();append(r.value("id").toString(),r.value("label").toString(),"video",order++);}
        order=300;for(const auto &v:replay.cloudViews()){const auto r=v.toMap();append(r.value("id").toString(),r.value("label").toString(),"view",order++);}
        order=400;for(const auto &v:replay.sceneLayers()){const auto r=v.toMap();append(r.value("id").toString(),r.value("label").toString(),r.value("kind")=="images"?"image":"text",order++);}
        const auto legacySpeed=replay.speedGauge().value("visible").toBool();
        if(legacySpeed)append("speed",tr("Speed Meter · 速度表"),"speed",500);
        order=501;for(const auto &v:replay.property("speedMeters").toList()){const auto r=v.toMap();if(legacySpeed&&r.value("id")=="speed")continue;append(r.value("id").toString(),r.value("label",tr("Speed Meter · 速度表")).toString(),"speed",order++);}
        return result;
    };
    b.exists=[raw=b.rawLayers](const QString &id){for(const auto &v:raw())if(v.toMap().value("id")==id)return true;return false;};
    b.remove=[&replay](const QStringList &ids){replay.removeLayers(ids);};
    b.duplicate=[&replay](const QString &id)->QString{
        auto document=replay.projectSnapshot();auto composition=document.composition();
        for(const auto &key:{"videos","views","images","texts","audio","speed_meters","plots","annotations","legends","colorbars"}){
            auto array=composition.value(key).toArray();
            for(const auto &v:array){auto item=v.toObject();if(item.value("id").toString()!=id)continue;
                const auto copy=QUuid::createUuid().toString(QUuid::WithoutBraces);item["id"]=copy;item["label"]=item.value("label").toString()+tr(" Copy");
                array.append(item);composition[key]=array;document.setComposition(composition);replay.project_=std::move(document);
                replay.refreshVideoLayers();replay.refreshCloudViews();replay.refreshSceneLayers();emit replay.recordingChanged();return copy;
            }
        }
        return {};
    };
    return b;
}

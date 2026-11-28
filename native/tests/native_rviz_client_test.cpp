#include "native_rviz_client.h"
#include "appearance_manager.h"
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QThread>
#include <algorithm>
#include <functional>
#include <cmath>
#include <memory>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void waitAt(int line, const std::function<bool()> &done, int milliseconds = 30000)
{
    QElapsedTimer deadline; deadline.start();
    while (!done() && deadline.elapsed() < milliseconds) {
        QCoreApplication::processEvents(); QThread::msleep(2);
    }
    if(!done())throw std::runtime_error("Native asynchronous operation did not settle at test line " + std::to_string(line));
}
#define wait(...) waitAt(__LINE__, __VA_ARGS__)
QVariantMap row(NativeRvizClient &client, const QString &name, const QString &type = {})
{
    for (const auto &value : client.properties()) {
        const auto item = value.toMap();
        if (item.value("name").toString() == name &&
            (type.isEmpty() || item.value("native_type").toString().endsWith(type))) return item;
    }
    return {};
}
void checkViewArchive()
{
    const QString original="Class: vendor/ActualController\nUnknown Native Setting:\n  nested: preserved\n";
    auto snapshot=[](const QString &id,const QString &yaml,const QString &plugin=QString("vendor/ActualController")) {
        return QJsonObject{{"view_id",id},{"class",plugin},{"controller_yaml",yaml}};
    };
    NativeViewArchive archive(2,4096);
    archive.remember({snapshot("camera-a",original)});
    require(archive.value("camera-a").value("controller_yaml").toString()==original,"Archive changed complete unknown native YAML");
    require(archive.state().value("yaml_storage_bytes").toLongLong()==original.size()*qint64(sizeof(QChar)),"Archive byte accounting missed its stored UTF-16 text");
    archive.remember({snapshot("camera-b","second"),snapshot("camera-a","updated")});
    require(archive.state().value("entries").toInt()==2&&archive.state().value("yaml_storage_bytes").toLongLong()==26,
        "Replacing a view snapshot duplicated the archive entry or byte count");
    archive.remember({snapshot("camera-c","third")});
    require(archive.value("camera-b").isEmpty()&&archive.classes().contains("camera-a")&&archive.classes().contains("camera-c"),
        "Bounded view archive did not evict the oldest identity after an update");
    archive.remember({snapshot("main","invalid"),snapshot("","invalid"),snapshot("camera-invalid","","")});
    require(archive.state().value("entries").toInt()==2,"Invalid or main camera snapshots entered the archive");
    NativeViewArchive bounded(3,30);
    bounded.remember({snapshot("first",QString(10,'a')),snapshot("second",QString(10,'b'))});
    require(bounded.value("first").value("controller_yaml").toString().isEmpty()
        &&bounded.classes().value("first")=="vendor/ActualController"&&bounded.value("second").value("controller_yaml").toString()==QString(10,'b')
        &&bounded.state().value("yaml_storage_bytes").toLongLong()==20&&bounded.state().value("incomplete_snapshots").toInt()==1,
        "Archive memory budget lost the class tombstone or retained excess YAML storage");
    NativeViewArchive disabled(0,0);disabled.remember({snapshot("absent",original)});
    require(disabled.classes().isEmpty()&&disabled.state().value("yaml_storage_bytes").toLongLong()==0,"Zero archive bounds retained state");
    bounded.clear();require(bounded.classes().isEmpty()&&bounded.state().value("yaml_storage_bytes").toLongLong()==0,"Clearing the archive retained native undo text");
    std::cout<<"Bounded native camera Undo archive: exact complete YAML, replacement/FIFO, memory tombstones, invalid inputs and clear passed.\n";
}
void checkCameraPropertySync(NativeRvizClient &client,const QStringList &args)
{
    require(args.size()>=7,"--camera-property-sync requires launcher, distribution, actual config, actual Bag and output directory");
    const auto output=args[6];require(QDir().mkpath(output),"Cannot create camera property evidence directory");
    QObject observer;QHash<QString,int> finished,failed,sent;QStringList errors;bool expectFailure=false;
    QObject::connect(&client,&NativeRvizClient::commandSent,&observer,[&](const QString &command){++sent[command];});
    QObject::connect(&client,&NativeRvizClient::commandFinished,&observer,[&](const QString &command,bool ok,const QString &error){
        if(ok)++finished[command];else{++failed[command];if(!expectFailure)errors.append(command+": "+error);}
    });
    client.start(QUrl::fromLocalFile(args[2]),args[3],QUrl::fromLocalFile(args[4]));
    wait([&]{return client.connected()||(!client.running()&&!client.diagnostic().isEmpty());},90000);
    require(client.connected(),qPrintable(client.status()+"\n"+client.diagnostic()));
    wait([&]{return !client.properties().isEmpty()&&!client.viewControllers().isEmpty();});
    const auto viewsDone=finished.value("views.sync");
    client.syncViews({QVariantMap{{"view_id","property-orbit"},{"class","rviz/Orbit"}},
        QVariantMap{{"view_id","property-top"},{"class","rviz/TopDownOrtho"}}});
    wait([&]{return finished.value("views.sync")>viewsDone&&client.renderViews().size()==2;});
    const auto changedViews=finished.value("view");client.setViewController("rviz/TopDownOrtho");
    wait([&]{return finished.value("view")>changedViews;});
    auto property=[&](const QString &viewId,const QString &name){
        client.setSearch(name);for(const auto &entry:client.properties()){const auto item=entry.toMap();
            if(item.value("view_id").toString()==viewId&&item.value("name").toString()==name
                &&!item.value("native_type").toString().endsWith("StatusProperty"))return item;}
        return QVariantMap{};
    };
    wait([&]{return !property("main","Scale").isEmpty()&&!property("property-top","Scale").isEmpty();});
    const auto grid=property({},"Grid");require(!grid.isEmpty(),"Actual fixture omitted Grid");
    const auto gridChanges=finished.value("set");client.setPropertyValue(grid.value("id").toString(),true);
    wait([&]{return finished.value("set")>gridChanges&&property({},"Plane").value("enabled").toBool();});
    auto resource=[&]{const auto count=finished.value("resources");client.refreshResources();
        wait([&]{return finished.value("resources")>count;});return client.nativeResources();};
    auto identities=[&]{client.setSearch("rviz");QVariantMap result;
        for(const auto &entry:client.properties()){const auto item=entry.toMap();if(!item.value("plugin_class").toString().isEmpty())
            result[item.value("id").toString()]=QVariantList{item.value("instance_id"),item.value("scene_id"),item.value("source_id")};}
        return result;
    };
    client.indexBag(QUrl::fromLocalFile(args[5]),"header","reject",{{"topics",QStringList{"/cloud_registered","/Odometry","/tf"}}});
    wait([&]{return !client.busy();},90000);require(client.indexed(),"Actual camera-property Bag index failed");
    const auto target=QString::number(client.replayResult().value("begin_ns").toString().toULongLong()+8000000000ULL);
    client.seekAbsolute(target);wait([&]{return !client.busy()&&client.synchronized()&&client.sceneReady()&&client.targetNs()==target;},90000);
    const auto replay=client.replayResult(),before=resource(),ids=identities();require(!ids.isEmpty(),"No real Display identity evidence");
    auto bagRequests=[&]{return sent.value("bag.open")+sent.value("bag.index")+sent.value("bag.seek")+sent.value("bag.advance")+sent.value("bag.play_from_start");};
    const auto baselineReads=bagRequests();QJsonArray cases;
    auto preserved=[&]{
        require(client.indexed()&&client.clockNs()==target&&client.targetNs()==target&&client.replayResult()==replay,
            "Camera set changed exact clock, target or actual replay history");
        require(bagRequests()==baselineReads,"Camera set or missing TF unnecessarily opened/indexed/replayed the Bag");
        require(identities()==ids,"Camera property edit recreated scene/Display/source/property identities");
    };
    auto edit=[&](const QString &viewId,const QString &name,const QVariant &value,bool succeeds=true){
        auto item=property(viewId,name);require(!item.isEmpty(),qPrintable("Missing native property "+viewId+" / "+name));
        const auto id=item.value("id").toString();const auto old=item.value("value");
        const auto success=finished.value("set"),failure=failed.value("set");expectFailure=!succeeds;
        client.setPropertyValue(id,value);wait([&]{return finished.value("set")>success||failed.value("set")>failure;});expectFailure=false;
        require((finished.value("set")>success)==succeeds,qPrintable(QString("Unexpected set result for %1 / %2 (%3), value %4: %5")
            .arg(viewId,name,item.value("native_type").toString(),value.toString(),client.status())));
        wait([&]{return property(viewId,name).value("id").toString()==id&&property(viewId,name).value("value")== (succeeds?value:old);});
        if(succeeds&&!viewId.isEmpty())wait([&]{return !client.viewPreviews().value(viewId).toMap().value("source").toString().isEmpty();});
        cases.append(QJsonObject{{"view_id",viewId},{"property",name},{"property_id",id},{"accepted",succeeds},
            {"value",QJsonValue::fromVariant(value)},{"clock_ns",client.clockNs()},{"target_ns",client.targetNs()},{"bag_request_count",bagRequests()-baselineReads}});
        QFile checkpoint(output+"/edit-checkpoints.json");require(checkpoint.open(QIODevice::WriteOnly)
            &&checkpoint.write(QJsonDocument(cases).toJson())>0,"Cannot persist actual property checkpoints");
    };
    QString job;QImage image;QVariantMap metadata;bool outputDone=false,outputOk=false;QString outputError;
    QObject::connect(&client,&NativeRvizClient::outputFramePrepared,&observer,[&](const QString &id,bool ok,const QString &,const QImage &frame,const QVariantMap &metrics,const QString &error){
        if(id!=job)return;outputDone=true;outputOk=ok;image=frame;metadata=metrics;outputError=error;
    });
    auto render=[&](const QString &viewId,const QString &file,bool succeeds=true){
        outputDone=false;image={};metadata.clear();expectFailure=!succeeds;
        job=client.prepareViewOutputFrame(viewId,target,1920,1080);wait([&]{return outputDone;},90000);expectFailure=false;
        require(outputOk==succeeds,qPrintable("Unexpected camera property output result: "+outputError));
        if(succeeds){require(image.size()==QSize(1920,1080)&&metadata.value("view_id")==viewId&&metadata.value("clock_ns")==target
            &&metadata.value("target_ns")==target&&metadata.value("native_scene_ready").toBool(),"Camera property output mixed view/time/readiness");
            require(image.save(output+"/"+file),"Cannot save native camera property PNG");}
        else require(image.isNull(),"Missing camera Target Frame produced output pixels");
        preserved();
    };
    for(const auto &viewId:{QString("main"),QString("property-top")}){
        edit(viewId,"Scale",19.25);preserved();require(client.synchronized()&&client.sceneReady(),"Scale canceled shared synchronization");
        edit(viewId,"Angle",.375);preserved();
        edit(viewId,"Scale",QString("invalid-number"),false);preserved();
        edit(viewId,"Scale",1e300,false);preserved();
        require(client.synchronized()&&client.sceneReady(),"Invalid camera number damaged synchronization");
        edit(viewId,"Target Frame",QString("body"));preserved();render(viewId,viewId+"-body.png");
        edit(viewId,"Target Frame",QString("uav_studio_missing_camera_property_tf"));
        const auto preview=client.viewPreviews().value(viewId).toMap();
        require(!preview.value("synchronized").toBool()&&!preview.value("native_scene_ready").toBool(),"Missing Target Frame was reported ready");
        preserved();
        // Matches the Inspector's automatic same-time seek after a successful
        // set, including when main-view readiness is false. No Bag workaround.
        client.seekAbsolute(target);QCoreApplication::processEvents();preserved();
        render(viewId,{},false);render(viewId=="main"?"property-top":"main",viewId+"-other-view.png");
        edit(viewId,"Target Frame",QString("body"));preserved();
        require(client.synchronized()&&client.sceneReady(),"Restoring camera Target Frame did not recover without replay");
        render(viewId,viewId+"-recovered.png");
    }
    edit("property-orbit","Distance",27.5);preserved();render("property-orbit","orbit-distance.png");
    for(const auto &plugin:{QString("rviz/Orbit"),QString("rviz/TopDownOrtho")}){
        const auto count=finished.value("view");client.setViewController(plugin);
        wait([&]{return finished.value("view")>count&&!client.viewPreviews().value("main").toMap().value("source").toString().isEmpty();});
        require(client.synchronized()&&client.sceneReady(),"Changing native main camera controller canceled shared data synchronization");
        preserved();render("main",plugin=="rviz/Orbit"?"main-controller-orbit.png":"main-controller-topdown.png");
        cases.append(QJsonObject{{"command","view"},{"class",plugin},{"clock_ns",client.clockNs()},{"target_ns",client.targetNs()},{"bag_request_count",bagRequests()-baselineReads}});
    }
    edit({},"Plane",QString("invalid-plane"),false);preserved();
    edit({},"Topic",17,false);preserved();
    require(client.synchronized()&&client.sceneReady(),"Invalid Display value canceled accurate history");
    const auto after=resource();
    for(const auto &key:{"displays","groups","tracked_display_sources","scene_nodes","cameras","tf_frames","replay","source_id","movable_objects"})
        require(after.value(key)==before.value(key),qPrintable(QString("Camera set changed native resource: ")+key));
    edit({},"Topic",QString("/uav_studio_missing_display_topic"));
    require(!client.synchronized()&&!client.sceneReady(),"Changing a Display Topic did not invalidate history");
    client.requestPreview();wait([&]{return !client.viewPreviews().value("main").toMap().value("source").toString().isEmpty();});
    require(!client.viewPreviews().value("main").toMap().value("synchronized").toBool(),"Changed Display Topic was certified synchronized");
    require(bagRequests()==baselineReads,"A Display edit silently replayed the Bag before an explicit seek");
    require(errors.isEmpty(),qPrintable(errors.join('\n')));
    QJsonObject counts;for(auto it=sent.cbegin();it!=sent.cend();++it)counts.insert(it.key(),it.value());
    QJsonObject report{{"target_ns",target},{"actual_bag",args[5]},{"cases",cases},{"command_counts",counts},{"experiment_replay_requests_during_camera_edits",0},
        {"resources_before",QJsonObject::fromVariantMap(before)},{"resources_after",QJsonObject::fromVariantMap(after)},
        {"identities",QJsonObject::fromVariantMap(ids)},{"main_and_additional_camera_missing_tf_refused",true},{"display_topic_change_invalidated",true},
        {"main_application_gui_verified",false},{"errors",QJsonArray::fromStringList(errors)}};
    QFile evidence(output+"/report.json");require(evidence.open(QIODevice::WriteOnly)&&evidence.write(QJsonDocument(report).toJson())>0,"Cannot write camera property report");
    client.stop();wait([&]{return !client.running();},15000);
    std::cout<<"Actual synchronized camera Scale/Angle/Distance/Target Frame: exact time and native identities preserved, zero Bag requests, invalid values harmless, main/additional missing TF isolated/refused/recovered, Display Topic invalidation retained. Main GUI remains a separate gate.\n";
}
void checkViewLifecycle(NativeRvizClient &client,const QStringList &args)
{
    require(args.size()>=7,"--view-lifecycle requires launcher, distribution, actual config, actual Bag and output directory");
    require(QDir().mkpath(args[6]),"Cannot create native view lifecycle evidence directory");
    QObject observer;QHash<QString,int> finished,sent;QStringList errors;
    QObject::connect(&client,&NativeRvizClient::commandFinished,&observer,[&](const QString &command,bool ok,const QString &error){
        if(ok)++finished[command];else errors.append(command+": "+error);
    });
    QObject::connect(&client,&NativeRvizClient::commandSent,&observer,[&](const QString &command){++sent[command];});
    client.start(QUrl::fromLocalFile(args[2]),args[3],QUrl::fromLocalFile(args[4]));
    wait([&]{return client.connected()||(!client.running()&&!client.diagnostic().isEmpty());},90000);
    require(client.connected(),qPrintable(client.status()+"\n"+client.diagnostic()));
    wait([&]{return !client.viewControllers().isEmpty()&&!client.properties().isEmpty();});
    auto resource=[&]{const auto count=finished.value("resources");client.refreshResources();wait([&]{return finished.value("resources")>count;});return client.nativeResources();};
    auto synchronize=[&](const QVariantList &views){
        const auto count=finished.value("views.sync");client.syncViews(views);wait([&]{return finished.value("views.sync")>count;});
        wait([&]{return client.renderViews().size()==views.size()&&std::all_of(views.cbegin(),views.cend(),[&](const QVariant &entry){
            const auto wanted=entry.toMap();for(const auto &actual:client.renderViews()){const auto view=actual.toMap();
                if(view.value("view_id")==wanted.value("view_id")&&view.value("class")==wanted.value("class"))return true;}return false;
        });});
    };
    synchronize({});
    const QVariantMap orbit{{"view_id","lifecycle-orbit"},{"class","rviz/Orbit"}},top{{"view_id","lifecycle-top"},{"class","rviz/TopDownOrtho"}};
    synchronize({orbit,top});
    auto topProperty=[&](const QString &name){for(const auto &entry:client.properties()){const auto value=entry.toMap();
        if(value.value("view_id")=="lifecycle-top"&&value.value("name")==name)return value;}return QVariantMap{};};
    const QVariantMap expected{{"Scale",19.25},{"Angle",.375},{"X",2.5},{"Y",-4.75},{"Target Frame",QString("body")}};
    for(auto it=expected.cbegin();it!=expected.cend();++it){
        client.setSearch(it.key());wait([&]{return !topProperty(it.key()).isEmpty();});
        const auto count=finished.value("set");client.setPropertyValue(topProperty(it.key()).value("id").toString(),it.value());
        wait([&]{return finished.value("set")>count;});
    }
    QString snapshotJob,snapshotYaml,snapshotError;bool snapshotDone=false,snapshotOk=false;
    QObject::connect(&client,&NativeRvizClient::configSnapshotReady,&observer,[&](const QString &job,bool ok,const QString &yaml,const QVariantMap &,const QString &error){
        if(job!=snapshotJob)return;snapshotDone=true;snapshotOk=ok;snapshotYaml=yaml;snapshotError=error;
    });
    auto snapshot=[&]{snapshotDone=false;snapshotJob=client.snapshotConfig();wait([&]{return snapshotDone;});require(snapshotOk,qPrintable(snapshotError));return snapshotYaml;};
    auto yaml=snapshot();const auto viewsStart=yaml.indexOf("\n  Render Views:\n");
    const auto controllerStart=yaml.indexOf("        Class: rviz/TopDownOrtho\n",viewsStart);
    require(viewsStart>=0&&controllerStart>viewsStart,"Lifecycle fixture has no real TopDown controller map");
    yaml.insert(controllerStart,"        Future Native Camera Setting:\n          nested: preserved-value\n          units: exact\n");
    const auto loads=finished.value("load_yaml");client.loadConfigYaml(yaml);
    wait([&]{return finished.value("load_yaml")>loads&&!client.busy();});
    client.setSearch("");
    client.indexBag(QUrl::fromLocalFile(args[5]),"header","reject",{{"topics",QStringList{"/cloud_registered","/Odometry","/tf"}}});
    wait([&]{return !client.busy();},90000);require(client.indexed(),"Lifecycle actual Bag index failed");
    const auto target=QString::number(client.replayResult().value("begin_ns").toString().toULongLong()+8000000000ULL);
    client.seekAbsolute(target);wait([&]{return !client.busy()&&client.targetNs()==target;},90000);
    require(client.synchronized()&&client.sceneReady(),"Lifecycle native scene was not ready at the actual experiment timestamp");
    auto displayIdentities=[&]{QVariantMap identities;for(const auto &entry:client.properties()){const auto value=entry.toMap();
        if(!value.value("plugin_class").toString().isEmpty())identities[value.value("id").toString()]=QVariantList{value.value("instance_id"),value.value("scene_id"),value.value("source_id")};}return identities;};
    const auto before=resource(),replay=client.replayResult(),identities=displayIdentities();
    require(!identities.isEmpty(),"Lifecycle fixture omitted actual Display/scene identities");
    const auto reads=sent.value("bag.index")+sent.value("bag.open")+sent.value("bag.seek")+sent.value("bag.advance");
    auto checkRestored=[&]{
        for(auto it=expected.cbegin();it!=expected.cend();++it){
            const auto actual=topProperty(it.key());require(!actual.isEmpty(),qPrintable("Restored native property is missing: "+it.key()));
            require(it.key()=="Target Frame"?actual.value("value")==it.value():std::abs(actual.value("value").toDouble()-it.value().toDouble())<1e-5,
                qPrintable("Deleting/Undo changed the full native camera property: "+it.key()));
        }
        require(client.indexed()&&client.synchronized()&&client.sceneReady()&&client.targetNs()==target&&client.clockNs()==target&&client.replayResult()==replay,
            "Deleting/Undo invalidated the real experiment timestamp or replay readiness");
        require(displayIdentities()==identities,"Deleting/Undo recreated actual Display/scene/source identities");
        const auto after=resource();
        for(const auto &key:{"displays","groups","tracked_display_sources","scene_nodes","cameras","tf_frames","replay","source_id","movable_objects"})
            require(after.value(key)==before.value(key),qPrintable(QString("Deleting/Undo changed native resource: ")+key));
        const auto saved=snapshot();require(saved.contains("Future Native Camera Setting")&&saved.contains("nested: preserved-value")&&saved.contains("units: exact"),
            "Deleted camera Undo dropped unknown nested controller YAML");
    };
    for(int cycle=0;cycle<2;++cycle){
        synchronize({orbit});
        require(client.archivedViewClasses().value("lifecycle-top")=="rviz/TopDownOrtho","Deleted actual controller class was not archived");
        require(resource().value("cameras").toInt()==before.value("cameras").toInt()-2,"Deleted view retained native controller/panel camera resources");
        synchronize({orbit,top});checkRestored();
    }
    require(sent.value("bag.index")+sent.value("bag.open")+sent.value("bag.seek")+sent.value("bag.advance")==reads,
        "Restoring a removed camera reopened or replayed the actual Bag");
    QFile preserved(args[6]+"/restored-native-camera.rviz");require(preserved.open(QIODevice::WriteOnly)&&preserved.write(snapshot().toUtf8())>0,"Cannot save restored native Controller evidence");preserved.close();
    synchronize({});const auto cleared=resource();
    require(cleared.value("cameras").toInt()==before.value("cameras").toInt()-4&&cleared.value("additional_views").toInt()==0,"Final camera deletion did not release both controller/panel pairs");
    const auto archiveState=client.viewArchiveState();
    require(archiveState.value("entries").toInt()<=archiveState.value("maximum_entries").toInt()
        &&archiveState.value("yaml_storage_bytes").toLongLong()<=archiveState.value("maximum_bytes").toLongLong(),"Actual native camera archive exceeded its configured bounds");
    require(errors.isEmpty(),qPrintable(errors.join('\n')));
    QJsonObject report{{"actual_bag",args[5]},{"target_ns",target},{"delete_restore_cycles",2},{"native_properties",QJsonObject::fromVariantMap(expected)},
        {"unknown_nested_yaml_preserved",true},{"scene_display_source_identities_preserved",true},{"experiment_replay_requests_during_restore",0},
        {"archive",QJsonObject::fromVariantMap(archiveState)},{"resources_before",QJsonObject::fromVariantMap(before)},{"resources_cleared",QJsonObject::fromVariantMap(cleared)},
        {"main_application_gui_verified",false}};
    QFile evidence(args[6]+"/report.json");require(evidence.open(QIODevice::WriteOnly)&&evidence.write(QJsonDocument(report).toJson())>0,"Cannot save actual view lifecycle report");
    std::cout<<"Actual native TopDown remove/Undo, Redo/Undo: full Scale/Angle/X/Y/Target Frame and unknown nested YAML, scene/Display/source identities and exact "
        <<target.toStdString()<<" ns preserved; no Bag load/index/replay; bounded text archive and camera release verified. Main GUI remains a separate gate.\n";
    client.clearPendingConfig();require(client.archivedViewClasses().isEmpty(),"New project retained the preceding native view Undo archive");
    client.stop();wait([&]{return !client.running();},30000);
}
void checkNativeViews(NativeRvizClient &client,const QStringList &args)
{
    require(args.size()>=7,"--native-views requires launcher, distribution, real config, real Bag and output directory");
    const auto output=args[6];require(QDir().mkpath(output),"Cannot create native view evidence directory");
    QHash<QString,int> finished,failed,sent;QStringList errors;QObject observer;
    bool expectedFailure=false;
    QObject::connect(&client,&NativeRvizClient::commandFinished,&observer,[&](const QString &command,bool ok,const QString &error){
        if(ok)++finished[command];else{++failed[command];if(!expectedFailure)errors.append(command+": "+error);}
    });
    QObject::connect(&client,&NativeRvizClient::commandSent,&observer,[&](const QString &command){++sent[command];});
    client.start(QUrl::fromLocalFile(args[2]),args[3],QUrl::fromLocalFile(args[4]));
    wait([&]{return client.connected()||(!client.running()&&!client.diagnostic().isEmpty());},90000);
    require(client.connected(),qPrintable(client.status()+"\n"+client.diagnostic()));
    wait([&]{return !client.viewControllers().isEmpty()&&!client.properties().isEmpty();});
    client.setViewController("rviz/Orbit");wait([&]{return finished.value("view")>0;});
    client.resetCamera();wait([&]{return finished.value("view.reset")>0;});
    auto resource=[&]{const auto count=finished.value("resources");client.refreshResources();wait([&]{return finished.value("resources")>count;});return client.nativeResources();};
    const auto baseline=resource();
    auto synchronize=[&](const QVariantList &views){const auto count=finished.value("views.sync");client.syncViews(views);wait([&]{return finished.value("views.sync")>count;});
        wait([&]{return client.renderViews().size()==views.size();});};
    const QVariantList views{QVariantMap{{"view_id","orbit-a"},{"class","rviz/Orbit"}},QVariantMap{{"view_id","top-b"},{"class","rviz/TopDownOrtho"}}};
    synchronize(views);
    require(client.independentCameras(),"Connected host omitted its real per-view camera capability");
    const auto added=resource();
    require(added.value("additional_views").toInt()==2&&added.value("cameras").toInt()==baseline.value("cameras").toInt()+4,
        "Native additional views did not create two independent controller/panel camera pairs");
    require(added.value("displays")==baseline.value("displays")&&added.value("tracked_display_sources")==baseline.value("tracked_display_sources")
        &&added.value("source_id")==baseline.value("source_id")&&!added.value("independent_view_visibility").toBool(),
        "Adding native cameras duplicated Display/source data or claimed independent visibility");
    client.setSearch("Distance");
    auto viewProperty=[&](const QString &viewId,const QString &name){for(const auto &entry:client.properties()){const auto item=entry.toMap();
        if(item.value("view_id")==viewId&&item.value("name")==name)return item;}return QVariantMap{};};
    wait([&]{return !viewProperty("orbit-a","Distance").isEmpty()&&!viewProperty("main","Distance").isEmpty();});
    const auto mainDistance=viewProperty("main","Distance").value("value").toDouble();
    auto setValue=[&](const QString &viewId,const QString &name,const QVariant &value){
        client.setSearch(name);wait([&]{return !viewProperty(viewId,name).isEmpty();});const auto count=finished.value("set");
        client.setPropertyValue(viewProperty(viewId,name).value("id").toString(),value);wait([&]{return finished.value("set")>count;});
        wait([&]{return viewProperty(viewId,name).value("value")==value;});
    };
    setValue("orbit-a","Distance",37.);
    client.setSearch("Distance");require(viewProperty("main","Distance").value("value").toDouble()==mainDistance,"Additional camera property changed the main camera");
    client.activateTool("rviz/MoveCamera");wait([&]{return client.activeToolClass()=="rviz/MoveCamera";});
    const auto pointers=finished.value("pointer");client.cameraMouseForView("orbit-a","wheel",.5,.5,0,0,0,120);wait([&]{return finished.value("pointer")>pointers;});
    client.refresh();wait([&]{return viewProperty("orbit-a","Distance").value("value").toDouble()!=37.;});
    const auto wheelDistance=viewProperty("orbit-a","Distance").value("value").toDouble();
    require(viewProperty("main","Distance").value("value").toDouble()==mainDistance,"Native per-view wheel changed the main camera");
    const auto resets=finished.value("view.reset");client.resetCameraForView("orbit-a");wait([&]{return finished.value("view.reset")>resets;});
    wait([&]{return std::abs(viewProperty("orbit-a","Distance").value("value").toDouble()-10.)<1e-5;});
    const auto undos=finished.value("undo");client.undoNative();wait([&]{return finished.value("undo")>undos;});
    wait([&]{return std::abs(viewProperty("orbit-a","Distance").value("value").toDouble()-wheelDistance)<1e-5;});
    const auto redos=finished.value("redo");client.redoNative();wait([&]{return finished.value("redo")>redos;});
    wait([&]{return std::abs(viewProperty("orbit-a","Distance").value("value").toDouble()-10.)<1e-5;});
    const auto distanceId=viewProperty("orbit-a","Distance").value("id").toString();
    const auto stable=resource();synchronize(views);const auto unchanged=resource();
    require(viewProperty("orbit-a","Distance").value("id").toString()==distanceId&&unchanged.value("history")==stable.value("history"),
        "Unchanged native view synchronization recreated cameras or added undo history");
    expectedFailure=true;const auto rejected=failed.value("views.sync");
    client.syncViews({views[0],views[0]});wait([&]{return failed.value("views.sync")>rejected;});expectedFailure=false;
    require(resource().value("cameras")==unchanged.value("cameras")&&client.renderViews().size()==2,"Rejected duplicate identities changed the live native view list");
    setValue("orbit-a","Distance",23.);setValue("orbit-a","Target Frame",QString("body"));
    client.setSearch("");
    auto index=[&]{client.indexBag(QUrl::fromLocalFile(args[5]),"header","reject",{{"topics",QStringList{"/cloud_registered","/Odometry","/tf"}}});
        wait([&]{return !client.busy();},90000);require(client.indexed(),"Actual multi-view Bag indexing failed");};
    index();const auto origin=client.replayResult().value("begin_ns").toString().toULongLong();
    const auto target=QString::number(origin+8000000000ULL),earlier=QString::number(origin+2000000000ULL);
    client.seekAbsolute(target);wait([&]{return !client.busy()&&client.targetNs()==target;},90000);
    require(client.synchronized()&&client.sceneReady(),"Actual native scene did not synchronize before per-view output");
    QString outputJob;QImage frame;QVariantMap metadata;QString outputError;bool outputFinished=false,outputSuccess=false;
    QObject::connect(&client,&NativeRvizClient::outputFramePrepared,&observer,[&](const QString &job,bool ok,const QString &time,const QImage &image,const QVariantMap &metrics,const QString &error){
        if(job!=outputJob)return;outputFinished=true;outputSuccess=ok;frame=image;metadata=metrics;outputError=error;
        require(time==metrics.value("target_ns").toString()||!ok,"Native output callback target mismatch");
    });
    QJsonArray evidence;
    auto render=[&](const QString &viewId,const QString &stamp,const QSize &size,const QString &file,bool succeeds=true){
        outputFinished=false;frame={};metadata.clear();outputError.clear();
        outputJob=client.prepareViewOutputFrame(viewId,stamp,size.width(),size.height());
        wait([&]{return outputFinished;},120000);
        require(outputSuccess==succeeds,qPrintable("Unexpected native view output result: "+outputError));
        if(!succeeds){require(frame.isNull(),"Missing-TF native view returned pixels as ready");return QImage{};}
        require(frame.size()==size&&metadata.value("view_id")==viewId&&metadata.value("target_ns")==stamp&&metadata.value("clock_ns")==stamp
            &&metadata.value("native_render_texture").toBool()&&metadata.value("native_scene_ready").toBool()&&metadata.value("required_tf_complete").toBool(),
            "Native per-view output identity/time/readiness/resolution was not exact");
        require(frame.save(output+"/"+file),"Could not save the actual native view PNG");
        auto entry=QJsonObject::fromVariantMap(metadata);entry.insert("file",file);entry.insert("pixel_sha256",QString::fromLatin1(QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(frame.constBits()),frame.sizeInBytes()),QCryptographicHash::Sha256).toHex()));
        evidence.append(entry);return frame;
    };
    const auto seeks=sent.value("bag.seek"),advances=sent.value("bag.advance");
    const auto main=render("main",target,{1920,1080},"main-8s.png");
    const auto orbit=render("orbit-a",target,{1920,1080},"orbit-a-8s.png");
    const auto top=render("top-b",target,{1920,1080},"top-b-8s.png");
    require(main!=orbit&&orbit!=top&&main!=top,"Three independent native cameras produced identical pixels");
    require(sent.value("bag.seek")==seeks&&sent.value("bag.advance")==advances,"Rendering shared exact-time cameras unnecessarily replayed/reset the Bag");
    QString batchJob;bool batchDone=false,batchOk=false,cancelBatch=false;QString batchError;
    QHash<QString,QImage> batchImages;QVariantList batchMetrics;
    QObject::connect(&client,&NativeRvizClient::outputViewFramePrepared,&observer,
        [&](const QString &job,const QString &viewId,const QString &stamp,const QImage &image,const QVariantMap &metrics){
            if(job!=batchJob)return;
            require(client.busy()&&stamp==target&&metrics.value("view_id")==viewId,"Native batch released its lock or mixed identities");
            batchImages.insert(viewId,image);
            const auto pointerCount=sent.value("pointer"),syncCount=sent.value("views.sync"),setCount=sent.value("set");
            client.cameraMouseForView("orbit-a","wheel",.5,.5,0,0,0,120);client.syncViews({});
            const auto property=viewProperty("orbit-a","Distance");client.setPropertyValue(property.value("id").toString(),99.);
            client.seekAbsolute(earlier);
            require(sent.value("pointer")==pointerCount&&sent.value("views.sync")==syncCount&&sent.value("set")==setCount,
                "Native batch allowed pointer/configuration mutation between camera frames");
            if(cancelBatch)client.cancelOutputFrame(job);
        });
    QObject::connect(&client,&NativeRvizClient::outputFramesPrepared,&observer,
        [&](const QString &job,bool ok,const QString &stamp,const QVariantList &metrics,const QString &error){
            if(job!=batchJob)return;batchDone=true;batchOk=ok;batchError=error;batchMetrics=metrics;
            require(stamp==target,"Native batch completion mixed the experiment target");
        });
    const QVariantList batchViews{QVariantMap{{"view_id","main"},{"width",1920},{"height",1080}},
        QVariantMap{{"view_id","orbit-a"},{"width",1920},{"height",1080}},QVariantMap{{"view_id","top-b"},{"width",1920},{"height",1080}}};
    const auto batchRenders=sent.value("render_frame");
    batchJob=client.prepareOutputFrames(target,batchViews);wait([&]{return batchDone;},120000);
    require(batchOk&&batchImages.size()==3&&batchMetrics.size()==3,qPrintable("Native locked batch failed: "+batchError));
    require(batchImages.value("main")==main&&batchImages.value("orbit-a")==orbit&&batchImages.value("top-b")==top,
        "Locked native batch pixels differ from the same individual camera frames");
    require(sent.value("render_frame")==batchRenders+3&&sent.value("bag.seek")==seeks&&sent.value("bag.advance")==advances,
        "Native camera batch replayed data or rendered the wrong camera count");
    const auto resourcesAtTime=resource();
    render("orbit-a",earlier,{1920,1080},"orbit-a-2s.png");
    const auto again=render("orbit-a",target,{1920,1080},"orbit-a-8s-restored.png");
    require(again==orbit,"Random-time restoration changed the same native camera pixels");
    render("top-b",target,{3840,2160},"top-b-4k.png");
    render("orbit-a",target,{7680,4320},"orbit-a-8k.png");
    require(resource().value("displays")==resourcesAtTime.value("displays"),"Per-view high-resolution output duplicated native Displays");
    for(const auto &viewId:{QString("orbit-a"),QString("top-b")}){
        wait([&]{client.requestViewPreview(viewId);return !client.viewPreviews().value(viewId).toMap().value("source").toString().isEmpty();});
        const auto preview=client.viewPreviews().value(viewId).toMap();require(preview.value("view_id")==viewId&&preview.value("clock_ns")==target,"Native preview map mixed camera/time identities");
    }
    setValue("orbit-a","Target Frame",QString("uav_studio_missing_target"));expectedFailure=true;
    render("orbit-a",target,{1920,1080},"refused-missing-tf.png",false);expectedFailure=false;
    require(client.synchronized()&&client.sceneReady(),"Missing additional-camera TF corrupted the shared ready scene");
    render("top-b",target,{1920,1080},"top-b-after-refusal.png");
    setValue("orbit-a","Target Frame",QString("body"));
    batchDone=false;batchImages.clear();cancelBatch=true;
    batchJob=client.prepareOutputFrames(target,batchViews);wait([&]{return batchDone;},120000);cancelBatch=false;
    require(!batchOk&&batchImages.size()==1&&!client.busy(),"Native batch cancellation rendered more views or retained the output lock");
    client.seekAbsolute(target);wait([&]{return !client.busy()&&client.synchronized();},90000);
    batchDone=false;batchImages.clear();const auto rejectedRenders=sent.value("render_frame");
    batchJob=client.prepareOutputFrames(target,{batchViews[0],batchViews[0]});wait([&]{return batchDone;});
    require(!batchOk&&batchImages.isEmpty()&&sent.value("render_frame")==rejectedRenders&&!client.busy(),
        "Duplicate native batch identities were not rejected before rendering");
    const auto saved=output+"/native-two-views.rviz";client.saveConfig(QUrl::fromLocalFile(saved));wait([&]{return QFileInfo::exists(saved)&&QFileInfo(saved).size()>0;});
    QFile config(saved);require(config.open(QIODevice::ReadOnly),"Cannot read actual saved native multi-view config");const auto yaml=QString::fromUtf8(config.readAll());
    require(yaml.contains("Render Views")&&yaml.contains("orbit-a")&&yaml.contains("top-b")&&yaml.contains("rviz/TopDownOrtho"),"Native YAML omitted real independent view configuration");
    auto extended=yaml;const auto viewsStart=extended.indexOf("\n  Render Views:\n");
    const auto controllerStart=extended.indexOf("        Class: rviz/Orbit\n",viewsStart);
    require(viewsStart>=0&&controllerStart>viewsStart,"Native multi-view YAML has no actual controller map for preservation check");
    extended.insert(controllerStart,"        Future Native Camera Setting: preserved-value\n");
    const auto loads=finished.value("load_yaml");client.loadConfigYaml(extended);
    require(client.busy(),"Native configuration load did not gate camera synchronization while its real tree was in flight");
    const auto loadingSyncs=sent.value("views.sync");client.syncViews({});
    require(sent.value("views.sync")==loadingSyncs,"Native view synchronization bypassed an in-flight configuration restore");
    wait([&]{return finished.value("load_yaml")>loads&&client.renderViews().size()==2&&!client.busy();});
    for(const auto &entry:client.renderViews()) {
        const auto view=entry.toMap();
        require(view.value("class").toString()==(view.value("view_id").toString()=="top-b"?"rviz/TopDownOrtho":"rviz/Orbit"),
            "Native configuration restore lost an actual saved view-controller plugin");
    }
    client.setSearch("Distance");wait([&]{return viewProperty("orbit-a","Distance").value("value").toDouble()==23.;});
    require(viewProperty("orbit-a","Distance").value("id").toString()!=distanceId,"Restored native view incorrectly retained an expired Property identity");
    const auto preserved=output+"/native-preserved-views.rviz";client.saveConfig(QUrl::fromLocalFile(preserved));
    wait([&]{return QFileInfo::exists(preserved)&&QFileInfo(preserved).size()>0;});
    QFile preservedFile(preserved);require(preservedFile.open(QIODevice::ReadOnly)&&preservedFile.readAll().contains("Future Native Camera Setting: preserved-value"),
        "Native additional controller silently dropped an unknown imported setting");
    synchronize({});const auto cleared=resource();
    for(const auto &key:{"cameras","scene_nodes","displays","tracked_display_sources","movable_objects"})
        require(cleared.value(key)==baseline.value(key),qPrintable(QString("Native view deletion did not return to resource baseline: ")+key));
    require(!client.viewPreviews().contains("orbit-a")&&!client.viewPreviews().contains("top-b"),"Deleted native camera retained stale preview pixels");
    for(int i=0;i<5;++i){synchronize(views);const auto presses=finished.value("pointer");
        client.cameraMouseForView("orbit-a","press",.5,.5,1,1,0);wait([&]{return finished.value("pointer")>presses;});synchronize({});}
    const auto cycled=resource();for(const auto &key:{"cameras","scene_nodes","displays","movable_objects","pending_pointer_transactions"})require(cycled.value(key)==cleared.value(key),"Repeated real native camera creation/removal leaked tracked resources");
    synchronize(views);const auto closes=finished.value("bag.close");client.closeBag();wait([&]{return finished.value("bag.close")>closes&&!client.busy();});
    const auto detached=resource();require(!client.sourceAvailable()&&!client.indexed()&&detached.value("additional_views").toInt()==2,
        "Closing native source lost independent cameras or retained source readiness");
    require(errors.isEmpty(),qPrintable(errors.join('\n')));
    QJsonObject report{{"actual_bag",args[5]},{"target_ns",target},{"views",3},{"rendered_frames",evidence},{"native_independent_cameras",true},
        {"native_view_tf_refusal",true},{"random_restore_pixel_equal",true},{"same_time_replay_requests",0},{"native_yaml_restore",true},
        {"resource_cycles",5},{"resources_baseline",QJsonObject::fromVariantMap(baseline)},{"resources_cleared",QJsonObject::fromVariantMap(cleared)},
        {"main_application_gui_verified",false},{"independent_view_visibility_verified",false},{"batch_composition_verified",false},
        {"locked_native_batch_verified",true},{"batch_pixel_equal",true},{"batch_cancel_verified",true}};
    QFile result(output+"/report.json");require(result.open(QIODevice::WriteOnly)&&result.write(QJsonDocument(report).toJson())>0,"Cannot save native multi-view report");
    std::cout<<"Actual native multi-view passed: one shared scene/replay, three independent cameras; per-view mouse/reset/Property/history/YAML, exact "<<target.toStdString()
        <<" ns, 1080P/4K/8K RenderTexture, missing-TF refusal, random restoration and 5 resource cycles. Main GUI and composed multi-view output remain separate gates.\n";
    client.stop();wait([&]{return !client.running();},30000);
}
void checkCameraReset(NativeRvizClient &client, const QStringList &args)
{
    require(args.size() >= 6, "--camera-reset requires launcher, distribution, config and actual bag paths");
    QHash<QString, int> finished;
    QStringList errors;
    QObject observer;
    QObject::connect(&client,&NativeRvizClient::commandFinished,&observer,[&](const QString &command,bool success,const QString &error){
        if(success) ++finished[command]; else errors.append(command+": "+error);
    });
    client.start(QUrl::fromLocalFile(args[2]),args[3],QUrl::fromLocalFile(args[4]));
    wait([&]{return client.connected()||(!client.running()&&!client.diagnostic().isEmpty());},90000);
    require(client.connected(),qPrintable(client.status()+"\n"+client.diagnostic()));
    wait([&]{return !client.viewControllers().isEmpty()&&!client.properties().isEmpty();});
    client.setViewController("rviz/Orbit");wait([&]{return finished.value("view")>0;});
    client.setSearch("Distance");
    auto distance=[&]{for(const auto &entry:client.properties()){const auto value=entry.toMap();
        if(value.value("name")=="Distance"&&value.value("native_type").toString().endsWith("FloatProperty")&&value.value("display_id").toString().isEmpty())return value;}return QVariantMap{};};
    wait([&]{return !distance().isEmpty();});
    client.resetCamera();wait([&]{return finished.value("view.reset")==1;});
    const auto resetTrees=finished.value("tree");client.refresh();wait([&]{return finished.value("tree")>resetTrees;});
    wait([&]{return !distance().isEmpty()&&distance().value("value").toDouble()>0.;});
    const auto baseline=distance().value("value").toDouble();
    require(std::abs(baseline-37.)>1.,"Native camera reset fixture unexpectedly has the edited distance");
    client.setPropertyValue(distance().value("id").toString(),37.);
    wait([&]{return std::abs(distance().value("value").toDouble()-37.)<1e-5;});
    const auto snapshot=client.runtimeInfo().value("configurationSnapshotSerial").toULongLong();
    client.resetCamera();wait([&]{return finished.value("view.reset")==2&&std::abs(distance().value("value").toDouble()-baseline)<1e-5;});
    require(client.runtimeInfo().value("configurationSnapshotSerial").toULongLong()>snapshot&&client.canUndo(),
        "Camera reset omitted its acknowledged native configuration cache/history");
    client.undoNative();wait([&]{return finished.value("undo")==1&&std::abs(distance().value("value").toDouble()-37.)<1e-5;});
    client.redoNative();wait([&]{return finished.value("redo")==1&&std::abs(distance().value("value").toDouble()-baseline)<1e-5;});
    client.setSearch("");
    client.indexBag(QUrl::fromLocalFile(args[5]),"header","reject",{{"topics",QStringList{"/cloud_registered","/Odometry","/tf"}}});
    wait([&]{return !client.busy();},90000);require(client.indexed(),"Real camera-reset Bag index failed");
    const auto origin=client.replayResult().value("begin_ns").toString().toULongLong();
    const auto target=QString::number(origin+8000000000ULL);client.seekAbsolute(target);
    wait([&]{return !client.busy()&&client.targetNs()==target;},90000);
    require(client.synchronized()&&client.sceneReady()&&client.clockNs()==target,"Real pre-reset camera scene was not synchronized/ready");
    // Expand actual native nodes rather than enumerating a few plugin examples.
    for(int pass=0;pass<12;++pass){bool expanded=false;const auto rows=client.properties();for(const auto &entry:rows){const auto value=entry.toMap();
        if(!value.value("expanded").toBool()&&!value.value("children").toList().isEmpty()){client.toggleExpanded(value.value("id").toString());expanded=true;}}
        if(!expanded)break;}
    auto identities=[&]{QVariantMap result;for(const auto &entry:client.properties()){const auto value=entry.toMap();
        if(!value.value("plugin_class").toString().isEmpty())result[value.value("id").toString()]=QVariantList{value.value("instance_id"),value.value("scene_id")};}return result;};
    const auto instances=identities();require(instances.size()>2,"Actual reset test lost the imported nested native Display tree");
    const auto resourcesBefore=finished.value("resources");client.refreshResources();wait([&]{return finished.value("resources")>resourcesBefore;});
    const auto resources=client.nativeResources(),replayResult=client.replayResult();
    client.activateTool("rviz/MoveCamera");wait([&]{return client.activeToolClass()=="rviz/MoveCamera";});
    const auto pointers=finished.value("pointer");client.cameraMouse("wheel",.5,.5,0,0,0,120);wait([&]{return finished.value("pointer")>pointers;});
    const auto resetCount=finished.value("view.reset");client.resetCamera();wait([&]{return finished.value("view.reset")>resetCount;});
    require(client.indexed()&&client.synchronized()&&client.sceneReady()&&client.targetNs()==target&&client.clockNs()==target&&client.replayResult()==replayResult,
        "Resetting the native camera invalidated or changed the exact experiment data/time");
    require(identities()==instances,"Camera reset recreated native Display/property/scene identities");
    const auto resourceCount=finished.value("resources");client.refreshResources();wait([&]{return finished.value("resources")>resourceCount;});
    const auto after=client.nativeResources();
    for(const auto &key:{"displays","groups","tracked_display_sources","scene_nodes","cameras","tf_frames","replay","source_id"})
        require(after.value(key)==resources.value(key),qPrintable(QString("Camera reset changed native resource: ")+key));
    const auto frames=finished.value("frame");
    wait([&]{client.requestPreview();return finished.value("frame")>frames&&client.previewTargetNs()==target&&!client.preview().isEmpty();});
    require(client.synchronized()&&client.sceneReady(),"Post-reset real PNG lost native readiness");
    require(errors.isEmpty(),qPrintable(errors.join('\n')));
    std::cout<<"Actual librviz Orbit reset passed: default distance "<<baseline<<", real property edit/reset/undo/redo/cache; "<<instances.size()
        <<" native Display/property identities and scene/TF/replay resources preserved at exact experiment time "<<target.toStdString()<<" ns.\n";
    client.stop();wait([&]{return !client.running();},30000);
}
void checkManualTopicRuntime(NativeRvizClient &client, const QString &directory)
{
    client.ensureStarted();
    wait([&] { return client.connected() || client.runtimeState() == "unavailable"; }, 90000);
    require(client.connected(), "No-source manual Topic runtime failed to connect");
    wait([&] { return !client.plugins().isEmpty() && !client.properties().isEmpty() && !client.topics().isEmpty(); });
    const QString topic="/uav_studio/manual_no_source";
    for(const auto &value:client.topics())require(value.toMap().value("name").toString()!=topic,"Manual Topic unexpectedly has a discovered data source");
    client.addDisplay("rviz/PointCloud2","No-source manual cloud",topic);
    wait([&] { return !client.busy()&&!row(client,"No-source manual cloud").isEmpty()&&client.displayTopics().contains(topic); });
    auto display=row(client,"No-source manual cloud");const auto instance=display.value("instance_id");
    client.toggleExpanded(display.value("id").toString());
    auto property=row(client,"Topic","RosTopicProperty");
    require(property.value("value").toString()==topic&&property.value("message_type").toString()=="sensor_msgs/PointCloud2",
        "Manual Topic was not assigned to the real native plugin property");
    require(client.status().contains("compatibility is unverified"),"Unknown Topic falsely claimed discovered compatibility");
    client.requestPreview();wait([&] { return !client.preview().isEmpty(); });
    require(!client.indexed()&&!client.synchronized()&&!client.sceneReady(),"No-source manual Topic falsely claimed experiment data readiness");
    const auto saved=directory+"/manual-no-source.rviz";client.saveConfig(QUrl::fromLocalFile(saved));
    wait([&] { return QFileInfo::exists(saved)&&QFileInfo(saved).size()>0; });
    QFile output(saved);require(output.open(QIODevice::ReadOnly),"Manual Topic config was not saved");
    const auto yaml=QString::fromUtf8(output.readAll());
    require(yaml.contains(topic)&&yaml.contains("No-source manual cloud"),"Native YAML omitted the manually configured Topic");
    bool restored=false;QObject restoreObserver;
    QObject::connect(&client,&NativeRvizClient::commandFinished,&restoreObserver,[&](const QString &command,bool success,const QString &){if(command=="load_yaml")restored=success;});
    client.loadConfigYaml(yaml);
    wait([&] { return restored&&row(client,"No-source manual cloud").value("instance_id")==instance&&!client.busy(); });
    display=row(client,"No-source manual cloud");client.toggleExpanded(display.value("id").toString());
    property=row(client,"Topic","RosTopicProperty");
    require(property.value("value").toString()==topic&&client.displayTopics().contains(topic),"Native YAML restore lost the manual Topic");
    require(!client.indexed()&&!client.synchronized(),"Restoring no-source Topic falsely synchronized data");
    QString knownIncompatibleTopic;
    for(const auto &value:client.topics())if(value.toMap().value("type").toString()!="sensor_msgs/PointCloud2"){
        knownIncompatibleTopic=value.toMap().value("name").toString();break;
    }
    require(!knownIncompatibleTopic.isEmpty(),"No actual known incompatible ROS Topic was available for strict validation");
    int adds=0;QObject sentObserver;
    QObject::connect(&client,&NativeRvizClient::commandSent,&sentObserver,[&](const QString &command){if(command=="add")++adds;});
    client.addDisplay("rviz/PointCloud2","Rejected known incompatible topic",knownIncompatibleTopic);
    require(adds==0&&row(client,"Rejected known incompatible topic").isEmpty(),"Known incompatible Topic bypassed native message-type validation");
    bool missingPropertyRejected=false,rollbackRemoved=false;QObject resultObserver;
    QObject::connect(&client,&NativeRvizClient::commandFinished,&resultObserver,[&](const QString &command,bool success,const QString &){
        if(command=="add"&&!success)missingPropertyRejected=true;
        if(command=="remove"&&success)rollbackRemoved=true;
    });
    client.addDisplay("rviz/Grid","Rejected missing native Topic",topic);
    wait([&] { return missingPropertyRejected&&rollbackRemoved&&!client.busy()&&row(client,"Rejected missing native Topic").isEmpty(); });
    client.stop();wait([&] { return !client.running(); },30000);
    std::cout << "Actual no-bag manual Topic, real RosTopicProperty, native YAML save/restore, unverified/waiting state, known-type rejection and missing-property rollback passed.\n";
}
void checkAutomaticRuntime(NativeRvizClient &client, const QString &directory)
{
    const auto config = directory + "/automatic-initial.rviz";
    QFile file(config); require(file.open(QIODevice::WriteOnly), "Cannot create explicit automatic-start configuration");
    file.write("Visualization Manager:\n  Global Options:\n    Fixed Frame: map\n    Background Color: 31; 33; 37\n  Displays:\n    - Class: rviz/Grid\n      Name: Explicit initial grid\n      Enabled: true\n      Alpha: 0.375\n"); file.close();
    client.setLauncher(QUrl::fromLocalFile(directory + "/obsolete-workspace/run_host.sh").toString());
    client.setDistribution("Obsolete-UAV-Workspace");
    QElapsedTimer response; response.start();
    client.ensureStarted(QUrl::fromLocalFile(config));
    require(response.elapsed() < 200 && client.detecting(), "Automatic native discovery blocked the UI thread");
    client.ensureStarted(QUrl::fromLocalFile(config)); // An idempotent call must not start another host.
    wait([&] { return client.connected() || client.runtimeState() == "unavailable"; }, 90000);
    if (!client.connected()) std::cerr << client.status().toStdString() << '\n' << client.diagnostic().toStdString();
    require(client.connected(), "Actual optional native runtime was not automatically discovered");
    require(client.runtimeInfo().value("optional").toBool() && client.runtimeInfo().value("independentBackendAvailable").toBool(),
            "Optional native runtime was confused with the independent C++ backend");
    wait([&] { return !row(client, "Explicit initial grid").isEmpty() && !client.plugins().isEmpty(); });
    require(!client.indexed(), "Empty-scene configuration unexpectedly required a bag index");
    client.requestPreview(); wait([&] { return !client.preview().isEmpty(); });
    require(!client.synchronized(), "A no-bag scene was falsely marked experiment-synchronized");
    const auto initial = row(client, "Explicit initial grid");
    const auto initialId = initial.value("id").toString();
    auto serial = client.runtimeInfo().value("configurationSnapshotSerial").toULongLong();
    client.displayAction("rename", initialId, "Edited before disconnect");
    wait([&] { return !row(client, "Edited before disconnect").isEmpty()
        && client.runtimeInfo().value("configurationSnapshotSerial").toULongLong() > serial; });
    client.toggleExpanded(initialId);
    auto alpha = row(client, "Alpha", "FloatProperty"); require(!alpha.isEmpty(), "Native Grid alpha missing");
    serial = client.runtimeInfo().value("configurationSnapshotSerial").toULongLong();
    client.setPropertyValue(alpha.value("id").toString(), .625);
    wait([&] { return client.runtimeInfo().value("configurationSnapshotSerial").toULongLong() > serial; });
    client.addDisplay("rviz/Axes", "Added before disconnect");
    wait([&] { return !row(client, "Added before disconnect").isEmpty(); });
    const auto addedId = row(client, "Added before disconnect").value("instance_id");
    // A controlled reconnect must flush the latest real native snapshot first.
    bool disconnected = false, stale = false;
    QObject observer;
    QObject::connect(&client, &NativeRvizClient::changed, &observer, [&] {
        if (!client.connected() && client.runtimeState() == "reconnecting") {
            disconnected = true; stale = stale || !client.preview().isEmpty() || client.synchronized() || client.indexed();
        }
    });
    client.retryConnection();
    wait([&] { return disconnected && client.connected(); }, 90000);
    wait([&] { return !row(client, "Edited before disconnect").isEmpty() && !row(client, "Added before disconnect").isEmpty(); });
    require(!stale && row(client, "Added before disconnect").value("instance_id") == addedId,
        "Reconnect retained stale pixels or lost edited native instance identities");
    client.toggleExpanded(row(client, "Edited before disconnect").value("id").toString());
    alpha = row(client, "Alpha", "FloatProperty");
    require(std::abs(alpha.value("value").toDouble() - .625) < 1e-6, "Reconnect restored the initial file instead of the latest edited properties");
    // Terminate only this client's process. The owned-session token cleanup
    // must release its ROS children before the automatic replacement starts.
    disconnected = false;
    // Force a process loss immediately after an acknowledged native edit. Its
    // success response must already contain the matching full configuration.
    QProcess terminate;
    QObject immediateObserver;bool immediateRename=false;
    QObject::connect(&client,&NativeRvizClient::commandFinished,&immediateObserver,[&](const QString &command,bool success,const QString &){
        if(command!="rename"||!success||immediateRename)return;
        immediateRename=true;
        const auto ownedPid=QString::number(client.runtimeInfo().value("clientProcessId").toLongLong());
#ifdef Q_OS_WIN
        terminate.start(QStandardPaths::findExecutable("taskkill.exe"),{"/PID",ownedPid,"/F"});
#else
        terminate.start("kill",{"-TERM",ownedPid});
#endif
    });
    client.displayAction("rename",row(client,"Edited before disconnect").value("id").toString(),"Acknowledged immediately before disconnect");
    wait([&]{return immediateRename;});
    const auto ownedPid = QString::number(client.runtimeInfo().value("clientProcessId").toLongLong());
    require(ownedPid.toLongLong() > 0, "Owned native client PID is unavailable");
    require((terminate.state()==QProcess::NotRunning||terminate.waitForFinished(10000)) && terminate.exitCode() == 0, "Could not terminate the owned native client for disconnect acceptance");
    wait([&] { return disconnected && client.connected(); }, 90000);
    wait([&] { return !row(client, "Acknowledged immediately before disconnect").isEmpty() && !row(client, "Added before disconnect").isEmpty(); });
    require(!stale && !client.indexed() && !client.synchronized(), "Unexpected disconnect retained stale synchronization");
    require(client.diagnostic().contains("Stale launcher"), "Stale launcher fallback was not diagnosed");
    client.stop(); wait([&] { return !client.running(); }, 30000);
    const auto attempts = client.reconnectAttempt();
    QElapsedTimer stopped; stopped.start();
    while (stopped.elapsed() < 2500) { QCoreApplication::processEvents(); QThread::msleep(2); }
    require(!client.running() && !client.detecting() && client.reconnectAttempt() == attempts,
            "Explicit stop unexpectedly restarted the optional native host");
    NativeRvizClient empty;
    empty.loadConfigYaml("Visualization Manager:\n  Global Options:\n    Fixed Frame: map\n  Displays:\n    - Class: rviz/Grid\n      Name: Obsolete queued project\n      Enabled: true\n");
    empty.loadConfigYaml("");
    empty.ensureStarted();
    wait([&] { return empty.connected() || empty.runtimeState() == "unavailable"; }, 90000);
    require(empty.connected(), "Fresh no-bag optional native scene failed to connect");
    wait([&] { return !row(empty, "Grid").isEmpty() && !row(empty, "Axes").isEmpty() && !empty.tools().isEmpty(); });
    require(row(empty,"Obsolete queued project").isEmpty(),"Cleared project resurrected an obsolete offline native configuration");
    for (const auto &plugin : {"rviz/MoveCamera", "rviz/Select", "rviz/FocusCamera", "rviz/Measure"}) {
        bool installed=false;
        for (const auto &tool:empty.tools()) if(tool.toMap().value("class").toString()==plugin)installed=true;
        require(installed, "Required actual native camera tool was not discovered");
        empty.activateTool(plugin); wait([&] { return empty.activeToolClass()==plugin; });
    }
    const auto beforeInvalid=empty.activeToolClass();empty.activateTool("missing/FakeTool");
    require(empty.activeToolClass()==beforeInvalid, "Missing tool was falsely activated");
    empty.activateTool("rviz/MoveCamera");wait([&] { return empty.activeToolClass()=="rviz/MoveCamera"; });
    bool pointerHandled=false;QObject pointerObserver;
    QObject::connect(&empty,&NativeRvizClient::commandFinished,&pointerObserver,[&](const QString &command,bool success,const QString &){if(command=="pointer")pointerHandled=success;});
    empty.cameraMouse("wheel",.5,.5,0,0,0,120);wait([&] { return pointerHandled; });
    empty.requestPreview(); wait([&] { return !empty.preview().isEmpty(); });
    require(!empty.indexed()&&!empty.synchronized(), "Default empty scene falsely required or claimed a bag index");
    empty.stop();wait([&] { return !empty.running(); },30000);
    NativeRvizClient pending;
    pending.ensureStarted();
    pending.loadConfigYaml("Visualization Manager:\n  Global Options:\n    Fixed Frame: map\n  Displays:\n    - Class: rviz/Axes\n      Name: Pending project before connection\n      Enabled: true\n");
    wait([&]{return pending.connected()||pending.runtimeState()=="unavailable";},90000);
    require(pending.connected(),"Queued initial project failed to connect");
    wait([&]{return !row(pending,"Pending project before connection").isEmpty();});
    require(row(pending,"Grid").isEmpty(),"Default configuration replaced the queued initial project");
    pending.stop();wait([&]{return !pending.running();},30000);
    std::cout << "Actual automatic WSL/Linux discovery, stale preference fallback, explicit initial configuration, no-bag native scene, edited configuration recovery, unexpected disconnect and owned cleanup passed.\n";
    std::cout << "Fresh default Grid/Axes, actual ToolManager catalog/activation and MoveCamera pointer response passed; spatial GUI selection/measurement remains a separate acceptance.\n";
}
void checkStandalonePackage(const QString &directory)
{
    const auto appDirectory=QCoreApplication::applicationDirPath();
    const auto manifestPath=appDirectory+"/native-rviz-backend.json";
    const bool originallyExists=QFileInfo::exists(manifestPath);QByteArray original;
    if(originallyExists){QFile existing(manifestPath);require(existing.open(QIODevice::ReadOnly),"Cannot preserve existing acceptance manifest");original=existing.readAll();}
    QFile manifest(manifestPath);require(manifest.open(QIODevice::WriteOnly),"Cannot create standalone acceptance manifest");
    manifest.write("{\"included\":false,\"optional\":true,\"package_profile\":\"Standalone\"}");manifest.close();
    NativeRvizClient standalone;
    standalone.setLauncher(QUrl::fromLocalFile(directory+"/old-workspace/run_host.sh").toString());
    standalone.ensureStarted();
    const bool respected=standalone.runtimeState()=="unavailable"&&!standalone.detecting()&&!standalone.running()
        &&standalone.runtimeInfo().value("code").toString()=="native_not_packaged";
    if(originallyExists){require(manifest.open(QIODevice::WriteOnly),"Cannot restore existing acceptance manifest");require(manifest.write(original)==original.size(),"Cannot restore acceptance manifest bytes");manifest.close();}
    else require(QFile::remove(manifestPath),"Cannot remove standalone acceptance manifest");
    require(respected,"Standalone package attempted WSL/source-tree discovery despite its manifest");
}
void checkUnavailableOptionalRuntime(const QString &directory)
{
    const auto oldPath=qgetenv("PATH");qputenv("PATH",directory.toUtf8());
    NativeRvizClient unavailable;
    unavailable.ensureStarted();
    wait([&] { return unavailable.runtimeState()=="unavailable"; });
    qputenv("PATH",oldPath);
    require(!unavailable.running()&&!unavailable.detecting()&&!unavailable.connected()
        &&unavailable.runtimeInfo().value("independentBackendAvailable").toBool()
        &&unavailable.runtimeInfo().value("optional").toBool(),"Missing optional runtime disabled standalone features or remained busy");
    require(!unavailable.runtimeInfo().value("code").toString().isEmpty(),"Missing optional runtime had no actionable diagnostic");
}
}
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir directory;
    QCoreApplication::setOrganizationName("UAVStudioNativeRvizTest");
    QCoreApplication::setApplicationName("Client");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    try {
        checkViewArchive();
        if(app.arguments().contains("--view-archive"))return 0;
        NativeRvizClient client;
        client.start(QUrl::fromLocalFile(directory.filePath("missing.sh")), "unused");
        require(!client.running() && !client.connected(), "Invalid launcher started a process");
        require(client.runtimeInfo().value("independentBackendAvailable").toBool(), "Native validation disabled the independent backend");
        checkUnavailableOptionalRuntime(directory.path());
        checkStandalonePackage(directory.path());
        if (app.arguments().contains("--camera-reset")) { checkCameraReset(client, app.arguments()); return 0; }
        if (app.arguments().contains("--camera-property-sync")) { checkCameraPropertySync(client, app.arguments()); return 0; }
        if (app.arguments().contains("--view-lifecycle")) { checkViewLifecycle(client, app.arguments()); return 0; }
        if (app.arguments().contains("--native-views")) { checkNativeViews(client, app.arguments()); return 0; }
        if (app.arguments().contains("--manual-topic")) { checkManualTopicRuntime(client, directory.path()); return 0; }
        if (app.arguments().contains("--auto-runtime")) { checkAutomaticRuntime(client, directory.path()); return 0; }
        AppearanceManager appearance(nullptr, directory.filePath("appearance.ini"));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("appearance", &appearance);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(NATIVE_QML_DIRECTORY "/NativeRvizPanel.qml")));
        if (component.isError()) std::cerr << component.errorString().toStdString();
        require(component.isReady(), "Native panel QML failed to load");
        std::unique_ptr<QObject> panel(component.createWithInitialProperties({{"client", QVariant::fromValue(&client)}}));
        if (!panel) std::cerr << component.errorString().toStdString();
        require(bool(panel), "Native panel QML failed to instantiate");
        auto *item = qobject_cast<QQuickItem *>(panel.get()); require(item, "Native panel is not an Item");
        item->setWidth(360); item->setHeight(800);
        for (int theme : {AppearanceManager::Light, AppearanceManager::Dark}) {
            appearance.setMode(theme); QCoreApplication::processEvents();
            require(item->implicitWidth() > 0, "Theme switch lost native panel layout");
        }
        const auto args = app.arguments();
        if (args.size() < 5) {
            std::cout << "Panel loads in both themes; invalid launcher rejected. Native integration SKIPPED: supply launcher, WSL distribution, config, bag.\n";
            return 0;
        }
        QStringList errors, sentCommands; int successes = 0, resourceResponses = 0; QString lastCommand; bool expectedCancellation = false;
        QObject commandObserver;
        QObject::connect(&client, &NativeRvizClient::commandSent, &commandObserver, [&](const QString &command) { sentCommands.append(command); });
        QObject::connect(&client, &NativeRvizClient::commandFinished, &commandObserver, [&](const QString &command, bool success, const QString &error) {
            if (!success && !(expectedCancellation && command == "bag.seek" && error.contains("canceled"))) errors.append(command + ": " + error);
            else { ++successes; lastCommand = command; if(command=="resources")++resourceResponses; }
        });
        client.start(QUrl::fromLocalFile(args[1]), args[2], QUrl::fromLocalFile(args[3]));
        wait([&] { return client.connected() || (!client.running() && !client.diagnostic().isEmpty()); });
        if (!client.connected()) std::cerr << client.status().toStdString() << '\n' << client.diagnostic().toStdString();
        require(client.connected(), "Actual native host did not connect");
        wait([&] { return !client.plugins().isEmpty() && !client.properties().isEmpty(); });
        const auto pluginCount = client.plugins().size();
        require(pluginCount > 0, "No actual native display plugins returned");
        client.addDisplay("rviz/Grid", "Client acceptance grid");
        wait([&] { return !row(client, "Client acceptance grid").isEmpty(); });
        const auto gridId = row(client, "Client acceptance grid").value("id").toString();
        client.toggleExpanded(gridId);
        auto plane = row(client, "Plane", "EnumProperty"); require(!plane.isEmpty(), "Real native Grid plane property missing");
        const auto planeId = plane.value("id").toString(); client.select(planeId);
        wait([&] { return client.selected().value("options").toStringList().size() == 3; });
        const auto options = client.selected().value("options").toStringList();
        require(options.contains("XY") && options.contains("XZ") && options.contains("YZ"), "Native enum did not use actual editor options");
        client.refresh();
        wait([&] { return lastCommand == "tree"; });
        require(client.selected().value("options").toStringList() == options, "Refresh lost native enum choices");
        client.setPropertyValue(planeId, "XZ");
        wait([&] { return client.selected().value("value").toString() == "XZ"; });
        auto alpha = row(client, "Alpha", "FloatProperty"); require(!alpha.isEmpty(), "Native alpha missing");
        const auto alphaId = alpha.value("id").toString(); client.select(alphaId); client.setPropertyValue(alphaId, .375);
        wait([&] { return std::abs(client.selected().value("value").toDouble() - .375) < 1e-6; });
        const auto saved = directory.filePath(QString::fromUtf8("原生配置.rviz")); client.saveConfig(QUrl::fromLocalFile(saved));
        wait([&] { QFile file(saved); return file.exists() && file.size() > 0; });
        QFile output(saved); require(output.open(QIODevice::ReadOnly) && output.readAll().contains("Client acceptance grid"), "Saved native YAML did not contain actual display");
        client.openBag(QUrl::fromLocalFile(args[4]));
        wait([&] { for (const auto &v:client.topics()) if(v.toMap().value("type").toString()=="sensor_msgs/PointCloud2")return true;return false; }, 60000);
        QString cloudTopic;
        for (const auto &v : client.topics()) if (v.toMap().value("type").toString() == "sensor_msgs/PointCloud2") {cloudTopic=v.toMap().value("name").toString();break;}
        require(!client.compatiblePlugins("sensor_msgs/PointCloud2").isEmpty(), "Actual plugin compatibility missing");
        client.addDisplay("rviz/PointCloud2", "Client topic acceptance", cloudTopic);
        wait([&] { return !row(client, "Client topic acceptance").isEmpty(); });
        const auto cloudId = row(client, "Client topic acceptance").value("id").toString(); client.toggleExpanded(cloudId);
        wait([&] { for (const auto &v:client.properties()) {const auto n=v.toMap(); if(n.value("display_id").toString()==cloudId && n.value("native_type").toString().endsWith("RosTopicProperty") && n.value("value").toString()==cloudTopic)return true;} return false; });
        client.addDisplay("rviz/PointCloud2", "Native-only body topic", "/cloud_registered_body");
        wait([&] { return client.displayTopics().contains("/cloud_registered_body") && !client.busy(); });
        const auto configuredTopics=client.displayTopics();client.setSearch("no_matching_property");
        require(client.displayTopics()==configuredTopics,"Tree search changed actual native Display subscriptions");client.setSearch({});
        QString snapshotJob,snapshotYaml,snapshotError;bool snapshotDone=false,snapshotSuccess=false;
        QObject snapshotObserver;
        QObject::connect(&client,&NativeRvizClient::configSnapshotReady,&snapshotObserver,[&](const QString &job,bool success,const QString &yaml,const QVariantMap &,const QString &error){
            if(job!=snapshotJob)return;snapshotDone=true;snapshotSuccess=success;snapshotYaml=yaml;snapshotError=error;
        });
        snapshotJob=client.snapshotConfig();require(!snapshotJob.isEmpty()&&!snapshotDone,"Native snapshot must return an asynchronous job identity");
        wait([&]{return snapshotDone;});require(snapshotSuccess,qPrintable(snapshotError));
        require(snapshotYaml.contains("Display ID")&&snapshotYaml.contains("Original Imported YAML"),"Native project snapshot omitted stable identities or preserved source");
        lastCommand.clear();client.loadConfigYaml(snapshotYaml);wait([&]{return lastCommand=="load_yaml";});
        require(client.displayTopics()==configuredTopics,"Native YAML project restore changed actual Display subscriptions");
        client.requestPreview(); wait([&] { return client.preview().startsWith("data:image/png;base64,"); });
        require(!client.clockNs().isEmpty(), "Native frame omitted actual clock");
        panel->setProperty("experimentBag",QUrl::fromLocalFile(args[4]));
        panel->setProperty("experimentTopics",QStringList{"/cloud_registered","/Odometry","/tf"});
        panel->setProperty("experimentRevision","real-client-fixture");
        require(QMetaObject::invokeMethod(panel.get(),"indexExperiment"),"Actual panel index action was unavailable");
        wait([&] { return !client.busy(); }, 60000);
        require(client.indexed(), "Actual Header time index did not complete");
        bool nativeTopicIndexed=false;
        for(const auto &topic:client.replayResult().value("topics").toList())
            if(topic.toMap().value("name").toString()=="/cloud_registered_body")nativeTopicIndexed=true;
        require(nativeTopicIndexed,"Panel omitted the actual native-only Topic Property from its explicit index");
        bool timeOk=false; const auto origin=client.replayResult().value("begin_ns").toString().toULongLong(&timeOk);
        require(timeOk && origin>0, "Native index omitted exact absolute time range");
        client.seekAbsolute(QString::number(origin+8000000000ULL));
        client.seekAbsolute(QString::number(origin+2000000000ULL));
        wait([&] { return !client.busy() && client.targetNs()==QString::number(origin+2000000000ULL) && !client.preview().isEmpty(); }, 60000);
        require(client.synchronized() && client.sceneReady() && client.clockNs()==client.targetNs(), "Latest queued target did not retain exact Header time and native readiness");
        require(client.replayResult().value("history_from_beginning").toBool(), "Backward client seek did not restore real history");
        bool outputDone=false,outputSuccess=false;QString outputJob,outputError;QImage outputImage;QVariantMap outputMetadata;
        QObject outputObserver;
        QObject::connect(&client,&NativeRvizClient::outputFramePrepared,&outputObserver,[&](const QString &job,bool success,const QString &target,const QImage &frame,const QVariantMap &metadata,const QString &error){
            if(job!=outputJob)return;outputDone=true;outputSuccess=success;outputImage=frame;outputMetadata=metadata;outputError=error;
            require(target==QString::number(origin+2000000000ULL),"Native output signal target changed");
        });
        const auto editsBeforeOutput=sentCommands.count("set");
        outputJob=client.prepareOutputFrame(QString::number(origin+2000000000ULL),3840,2160);
        require(!outputJob.isEmpty()&&!outputDone,"Native output failed to provide an asynchronous job identity");
        client.setPropertyValue(alpha.value("id").toString(),0.1);
        require(sentCommands.count("set")==editsBeforeOutput,"Native output allowed a concurrent scene edit");
        wait([&]{return outputDone;},60000);
        require(outputSuccess,qPrintable(outputError));
        require(outputImage.size()==QSize(3840,2160)&&outputMetadata.value("native_render_texture").toBool()
            &&outputMetadata.value("clock_ns").toString()==QString::number(origin+2000000000ULL),"Native client output dimensions or exact clock failed");
        const auto advancesBefore=sentCommands.count("bag.advance");
        client.advanceAbsolute(QString::number(origin+2100000000ULL));client.advanceAbsolute(QString::number(origin+2200000000ULL));
        wait([&]{return !client.busy()&&client.targetNs()==QString::number(origin+2200000000ULL)&&client.previewTargetNs()==client.targetNs();},60000);
        require(client.synchronized()&&sentCommands.count("bag.advance")>=advancesBefore+1
            &&client.replayResult().value("replay_mode").toString()=="incremental-forward"&&!client.replayResult().value("native_scene_reset").toBool(),
            "Forward native client advance reset the scene or displayed a stale timestamp");
        const auto seeksBeforeCancel=sentCommands.count("bag.seek");expectedCancellation=true;
        client.seekAbsolute(QString::number(origin+8000000000ULL));
        client.seekAbsolute(QString::number(origin+2000000000ULL));client.cancelPendingSeek();
        wait([&]{return !client.busy();});expectedCancellation=false;
        require(sentCommands.count("bag.seek")==seeksBeforeCancel+1&&!client.synchronized(),
            "Canceling paused playback retained and sent the queued obsolete seek");
        client.seekAbsolute(QString::number(origin));
        wait([&] { return !client.busy(); }, 60000);
        require(!client.replayResult().value("tf_coverage_complete").toBool() && !client.synchronized(), "Missing first-frame TF incorrectly claimed synchronization");
        client.addDisplay("rviz/Grid","Lifecycle grid A");
        wait([&]{return !row(client,"Lifecycle grid A").isEmpty();});
        client.addDisplay("rviz/Grid","Lifecycle grid B");
        wait([&]{return !row(client,"Lifecycle grid B").isEmpty();});
        const auto gridA=row(client,"Lifecycle grid A"),gridB=row(client,"Lifecycle grid B");
        require(client.canUndo(),"Native configuration did not expose real undo availability");
        client.removeDisplays({gridA.value("id").toString(),gridB.value("id").toString()});
        wait([&]{return row(client,"Lifecycle grid A").isEmpty()&&row(client,"Lifecycle grid B").isEmpty();});
        client.undoNative();
        wait([&]{return !row(client,"Lifecycle grid A").isEmpty()&&!row(client,"Lifecycle grid B").isEmpty()&&client.canRedo();});
        require(row(client,"Lifecycle grid A").value("instance_id")==gridA.value("instance_id")
            &&row(client,"Lifecycle grid B").value("instance_id")==gridB.value("instance_id"),"Native bulk undo lost persistent identities");
        client.redoNative();
        wait([&]{return row(client,"Lifecycle grid A").isEmpty()&&row(client,"Lifecycle grid B").isEmpty();});
        client.closeBag();
        wait([&]{return !client.busy()&&!client.sourceAvailable();},60000);
        require(!client.indexed()&&!client.synchronized()&&client.targetNs().isEmpty()&&client.preview().isEmpty()&&client.topics().isEmpty(),
            "Closing a native bag retained data, preview, source catalog or synchronized state");
        require(client.nativeResources().value("tf_frames").toInt()==0
            &&client.nativeResources().value("replay").toMap().value("owned_topic_publishers").toInt()==0,
            "Closing a native bag did not release actual native TF/replay resources");
        const auto beforeResources=resourceResponses;client.refreshResources();
        wait([&]{return resourceResponses>beforeResources;});
        require(!client.nativeResources().value("source_available").toBool(),"Native resource refresh resurrected a removed source");
        require(errors.isEmpty(), qPrintable(errors.join('\n')));
        client.stop(); wait([&] { return !client.running(); }, 25000);
        require(!client.connected() && client.properties().isEmpty() && client.preview().isEmpty(), "Disconnect retained stale native state");
        std::cout << "Real native host: " << pluginCount << " plugins; enum refresh/edit, float edit, Unicode YAML save, real bag By Topic, PNG preview and process cleanup verified (" << successes << " responses). Main-playhead synchronization is not certified by this test.\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}

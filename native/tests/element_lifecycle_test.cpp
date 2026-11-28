#include "canvas_layer_model.h"
#include "replay_controller.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
void write(const QString &path,const QByteArray &bytes){QFile file(path);check(file.open(QIODevice::WriteOnly),"Cannot create lifecycle fixture");check(file.write(bytes)==bytes.size(),"Cannot write lifecycle fixture");}
QByteArray bytes(const QString &path){QFile file(path);check(file.open(QIODevice::ReadOnly),"Cannot read lifecycle fixture");return file.readAll();}
QByteArray hash(const QString &path){return QCryptographicHash::hash(bytes(path),QCryptographicHash::Sha256);}
QVariantMap source(const QVariantList &sources,const QString &kind,const QString &path={})
{
    for(const auto &v:sources){const auto row=v.toMap();if(row.value("kind")==kind&&(path.isEmpty()||row.value("path")==path))return row;}
    return {};
}
QJsonObject loadJson(const QString &path){return QJsonDocument::fromJson(bytes(path)).object();}
void waitLoading(ReplayController &controller){QElapsedTimer timer;timer.start();while(controller.loading()&&timer.elapsed()<10000){QCoreApplication::processEvents();QThread::msleep(2);}check(!controller.loading(),"Lifecycle project reload did not settle");}
}
int main(int argc,char **argv)
{
    QGuiApplication app(argc,argv);QTemporaryDir temporary;
    try{
        check(temporary.isValid(),"Cannot create fixture directory");QDir dir(temporary.path());dir.mkdir("moved");
        const auto video=dir.filePath("shared.mp4"),other=dir.filePath("replacement.mp4"),audio=dir.filePath("flight.wav"),picture=dir.filePath("logo.png");
        // CRUD/manifest fixtures deliberately do not claim decodable flight media.
        write(video,"read-only project material fixture\n");write(other,"replacement material fixture\n");write(audio,"read-only audio fixture\n");
        QImage image(20,20,QImage::Format_ARGB32);image.fill(Qt::green);check(image.save(picture),"Cannot write image fixture");
        const auto originalVideo=hash(video),originalOther=hash(other),originalAudio=hash(audio),originalPicture=hash(picture);
        {
            ReplayController backend; backend.openDemo(); backend.seek(3.125);
            const auto label = backend.canvas()->addElement("text", {{"text", "Preserved backend edit"}});
            backend.canvas()->select(label);
            const auto record = backend.recording(); const auto state = backend.canvas()->settings(label);
            int notifications = 0, recordingChanges = 0;
            QObject observer;
            QObject::connect(&backend, &ReplayController::renderBackendChanged, &observer, [&]{ ++notifications; });
            QObject::connect(&backend, &ReplayController::recordingChanged, &observer, [&]{ ++recordingChanges; });
            backend.togglePlayback(); check(backend.playing(), "Backend fixture failed to start its real controller clock");
            backend.setRenderBackend("native-rviz");
            check(backend.renderBackend()=="native-rviz"&&notifications==1&&recordingChanges==0&&!backend.loading()&&
                backend.recording()==record&&std::abs(backend.position()-3.125)<1e-9&&backend.playing()&&
                backend.canvas()->settings(label)==state&&backend.canvas()->selection()==QStringList{label},
                "Backend preference reloaded data, interrupted playback or discarded canvas edits/selection");
            backend.setRenderBackend("native-rviz");backend.setRenderBackend("missing/Backend");
            check(notifications==1&&backend.renderBackend()=="native-rviz", "No-op/invalid backend preference mutated the project");
            backend.undo();check(backend.renderBackend()=="independent"&&backend.recording()==record&&backend.playing(), "Backend Undo changed recording/playback instead of requested preference");
            backend.redo();check(backend.renderBackend()=="native-rviz"&&backend.recording()==record&&backend.playing(), "Backend Redo failed to restore the requested preference");
            backend.pause();const auto manifest=dir.filePath("backend-preference.uavstudio");backend.saveProjectAs(QUrl::fromLocalFile(manifest));
            ReplayController reopenedBackend;int restored=0;QObject reopenedObserver;
            QObject::connect(&reopenedBackend,&ReplayController::renderBackendChanged,&reopenedObserver,[&]{++restored;});
            reopenedBackend.openProject(QUrl::fromLocalFile(manifest));waitLoading(reopenedBackend);
            check(reopenedBackend.renderBackend()=="native-rviz"&&restored>0&&reopenedBackend.canvas()->settings(label)==state,
                "Project reopen omitted requested backend, notification or canvas state");
            reopenedBackend.openDemo();check(reopenedBackend.renderBackend()=="independent", "New demo inherited a different project's backend request");
        }
        auto document=replay::ProjectDocument::create("none");document.addVideo(video);document.addVideo(video);
        const auto first=document.videos()[0].id,second=document.videos()[1].id;
        const auto stableId=source(document.sources(),"video").value("id").toString();
        for(int i=0;i<10;++i)check(source(document.sources(),"video").value("referenceCount").toInt()==2,"Querying sources accumulated derived references");
        auto noChange=document.json();document.removeCanvasLayers({});document.removeCanvasLayers({"","does-not-exist"});
        check(document.json()==noChange,"Empty or unknown deletion changed the project/Undo state");
        check(!document.removeSource(stableId,false)&&document.json()==noChange,"Refused source removal mutated the project");
        check(!document.removeSource("does-not-exist",true)&&document.json()==noChange,"Unknown source removal mutated the project");
        document.removeCanvasLayers({first});check(document.videos().size()==1&&document.videos()[0].id==second,"Deleting one shared instance removed another");
        check(source(document.sources(),"video").value("referenceCount").toInt()==1,"Shared source retained stale reference count");
        document.removeCanvasLayers({second});const auto retained=source(document.sources(),"video");
        check(document.videos().empty()&&retained.value("referenceCount").toInt()==0&&retained.value("id")==stableId,"Last layer removal lost source or retained references");
        const auto cleanSave=document.saveAs(dir.filePath("deleted.uavstudio"));const auto reopened=replay::ProjectDocument::load(cleanSave.path());
        check(reopened.videos().empty()&&source(reopened.sources(),"video").value("referenceCount").toInt()==0,"Save/reopen resurrected a removed layer");
        check(document.removeSource(stableId,false)&&document.sources().isEmpty(),"Unused source cannot be removed");
        // Imported stale runtime fields must never become reference truth, and
        // unknown source metadata must survive retaining/remapping the catalog.
        auto imported=reopened.json();auto entries=imported.value("sources").toArray();auto entry=entries[0].toObject();
        entry["references"]=QJsonArray{QJsonObject{{"id","ghost"}}};entry["referenceCount"]=97;entry["size"]="incorrect";entry["notes"]="preserve user metadata";entries[0]=entry;imported["sources"]=entries;
        const auto importedPath=dir.filePath("stale.uavstudio");write(importedPath,QJsonDocument(imported).toJson());
        auto stale=replay::ProjectDocument::load(importedPath);check(source(stale.sources(),"video").value("referenceCount").toInt()==0,"Persisted references were treated as live instances");
        stale.retainSourceCatalog();check(stale.json().value("sources").toArray()[0].toObject().value("notes")=="preserve user metadata","Retaining catalog discarded unknown user metadata");
        const auto sanitized=stale.saveAs(dir.filePath("sanitized.uavstudio")).json().value("sources").toArray()[0].toObject();
        check(!sanitized.contains("references")&&!sanitized.contains("referenceCount")&&!sanitized.contains("size"),"Derived file/reference data was persisted");
        // Audio participates in every media-remapping operation.
        auto audioProject=replay::ProjectDocument::create("none");auto scene=audioProject.composition();
        scene["audio"]=QJsonArray{QJsonObject{{"id","sound"},{"path",audio}}};audioProject.setComposition(scene);
        check(audioProject.media().contains(audio),"Audio omitted from protected source paths");
        const auto moved=audioProject.saveAs(dir.filePath("moved/audio.uavstudio"));
        check(source(moved.sources(),"audio").value("path")==audio,"SaveAs did not rebase audio");
        const auto portable=replay::ProjectDocument::load(audioProject.exportFolder(dir.filePath("audio-package")));
        const auto packedAudio=source(portable.sources(),"audio").value("path").toString();
        check(packedAudio!=audio&&hash(packedAudio)==originalAudio,"Portable project omitted/copied wrong audio");
        // The same file used as two kinds remains independently replaceable.
        auto replacement=replay::ProjectDocument::create("none");replacement.addVideo(video);scene=replacement.composition();
        scene["audio"]=QJsonArray{QJsonObject{{"id","shared-sound"},{"path",video}}};replacement.setComposition(scene);
        const auto videoSource=source(replacement.sources(),"video").value("id").toString();replacement.replaceSource(videoSource,other);
        check(replacement.videos()[0].path==other&&source(replacement.sources(),"audio").value("path")==video,"Replacing video silently changed an independent audio source");
        check(source(replacement.sources(),"video").value("id")==videoSource,"Replacing source changed its identity");
        // All deletion entry points remove generic canvas state and motions.
        auto legacy=replay::ProjectDocument::create("none");legacy.addSceneLayer("images",picture,0.,10.);legacy.addCloudView("rear");
        const auto imageId=legacy.sceneLayers()[0].toMap().value("id").toString(),viewId=legacy.cloudViews()[0].toMap().value("id").toString();
        legacy.setCanvasState({{"items",QVariantMap{{imageId,QVariantMap{{"x",.2}}},{viewId,QVariantMap{{"rotation",32.}}}}}});
        legacy.addLayerMotion("image",imageId,0.,1.,.4,.5,.5,.5);legacy.addLayerMotion("view",viewId,0.,1.,.4,.5,.5,.5);
        legacy.addLayerMotion("main",{},0.,1.,.4,.5,.5,.5);legacy.addLayerMotion("speed",{},0.,1.,.4,.5,.5,.5);
        legacy.removeSceneLayer(imageId);legacy.removeCloudView(viewId);legacy.removeCanvasLayers({"main","speed"});
        check(legacy.canvasState().value("items").toMap().isEmpty()&&legacy.layerMotions().isEmpty(),"Delete left orphan geometry or main/speed keyframed motion");
        check(source(legacy.sources(),"image").value("referenceCount").toInt()==0,"Legacy image deletion did not retain unused source");
        // Shared scientific images are real project assets in both workspaces.
        auto scientific=replay::ProjectDocument::create("none");
        const QVariantList scientificImages{QVariantMap{{"id","scientific-one"},{"kind","image"},{"source",QUrl::fromLocalFile(picture)}},
            QVariantMap{{"id","scientific-two"},{"kind","image"},{"source",picture}}};
        scientific.setCanvasState({{"elements",scientificImages},{"items",QVariantMap{{"scientific-one",QVariantMap{{"rotation",13.}}}}}});
        scientific.setTrajectorySettings({{"path",video},{"background_file",picture},{"canvas",QJsonObject{{"elements",QJsonArray{QJsonObject{{"id","trajectory-image"},{"kind","image"},{"source",picture}}}}}}});
        check(scientific.media().contains(picture)&&source(scientific.sources(),"image").value("referenceCount").toInt()==4,"Scientific image/background source references are missing");
        auto scientificSaved=scientific.saveAs(dir.filePath("moved/scientific.uavstudio"));
        check(scientificSaved.canvasState().value("elements").toList()[0].toMap().value("source")==picture&&
            scientificSaved.trajectorySettings().value("canvas").toObject().value("elements").toArray()[0].toObject().value("source")==picture&&
            scientificSaved.trajectorySettings().value("background_file")==picture,"Shared image paths were not rebased/resolved on SaveAs");
        const auto scientificPortable=replay::ProjectDocument::load(scientificSaved.exportFolder(dir.filePath("scientific-package")));
        const auto packedImage=scientificPortable.canvasState().value("elements").toList()[0].toMap().value("source").toString();
        check(packedImage!=picture&&hash(packedImage)==originalPicture&&scientificPortable.trajectorySettings().value("background_file")==packedImage,"Portable project omitted shared scientific/background image bytes");
        scientific.removeCanvasLayers({"scientific-one"});check(source(scientific.sources(),"image").value("referenceCount").toInt()==3,"Scientific instance deletion retained stale reference");
        const auto scientificSource=source(scientific.sources(),"image").value("id").toString();const auto scientificBefore=scientific.json();
        check(!scientific.removeSource(scientificSource,false)&&scientific.json()==scientificBefore,"Refused scientific source removal changed workspace state");
        check(scientific.removeSource(scientificSource,true)&&scientific.canvasState().value("elements").toList().isEmpty()&&
            scientific.trajectorySettings().value("canvas").toObject().value("elements").toArray().isEmpty()&&
            !scientific.trajectorySettings().contains("background_file")&&source(scientific.sources(),"image").isEmpty(),"Confirmed source removal left shared element/background references");
        // Real Controller/Canvas transaction behavior, without pretending that
        // this headless state test exercises an actual decoder or visible GUI.
        ReplayController controller;controller.openDemo();controller.addVideo(QUrl::fromLocalFile(video));controller.addVideo(QUrl::fromLocalFile(video));
        controller.addSceneImage(QUrl::fromLocalFile(picture));controller.addSceneText("Experiment label · 实验标注");controller.addCloudView(0);
        auto *canvas=controller.canvas();const auto a=controller.videoLayers()[0].toMap().value("id").toString(),b=controller.videoLayers()[1].toMap().value("id").toString();
        canvas->edit(a,{{"x",.1234},{"y",.2345},{"width",.321},{"height",.222},{"rotation",37.5},{"opacity",.71},{"name","Shared material A"},
                        {"visibilityRanges",QVariantList{QVariantList{1.125,3.25},QVariantList{5.,9.75}}},{"visibilityRangesEnabled",true}});
        canvas->addKeyframe(a,1.125,{{"x",.1},{"opacity",.2}});canvas->addKeyframe(a,4.875,{{"x",.7},{"opacity",.9}});
        canvas->edit(b,{{"x",.55},{"rotation",-23.},{"visible",false}});
        const auto baselineSettings=canvas->settings(a);const auto baseline=dir.filePath("before.uavstudio");controller.saveProjectAs(QUrl::fromLocalFile(baseline));
        const auto baselineDocument=loadJson(baseline);QStringList remove{a,b};
        for(const auto &row:controller.sceneLayers())remove.append(row.toMap().value("id").toString());for(const auto &row:controller.cloudViews())remove.append(row.toMap().value("id").toString());
        controller.removeLayers(remove);check(controller.videoLayers().isEmpty()&&controller.sceneLayers().isEmpty()&&controller.cloudViews().isEmpty(),"Batch deletion left live instances");
        check(source(controller.sources(),"video").value("referenceCount").toInt()==0,"Controller deletion retained stale source references");
        controller.undo();check(controller.videoLayers().size()==2&&canvas->settings(a)==baselineSettings,"One Undo failed to restore full geometry/keyframes/ranges");
        check(source(controller.sources(),"video").value("referenceCount").toInt()==2,"Undo accumulated derived source references");
        controller.saveProjectAs(QUrl::fromLocalFile(dir.filePath("restored.uavstudio")));check(loadJson(dir.filePath("restored.uavstudio"))==baselineDocument,"Undo did not restore the complete batch document");
        controller.redo();check(controller.videoLayers().isEmpty(),"One Redo failed to remove the complete batch");
        const auto scientificText=canvas->addElement("text",{{"text","Model-only scientific label"}});
        const auto scientificImage=canvas->addElement("image",{{"source",picture}});
        const auto scientificState=canvas->settings(scientificImage);const auto beforeScientificDelete=source(controller.sources(),"image").value("referenceCount").toInt();
        canvas->select(scientificText);canvas->select(scientificImage,true);canvas->removeSelected();
        check(canvas->element(scientificText).isEmpty()&&canvas->element(scientificImage).isEmpty()&&source(controller.sources(),"image").value("referenceCount").toInt()==beforeScientificDelete-1,"Shared element deletion left live source reference");
        controller.undo();check(!canvas->element(scientificText).isEmpty()&&canvas->settings(scientificImage)==scientificState,"Shared batch deletion did not Undo in one complete state restoration");
        controller.redo();check(canvas->element(scientificImage).isEmpty(),"Shared batch Redo did not remove all scientific elements");
        const auto deletedPath=dir.filePath("batch-deleted.uavstudio");controller.saveProjectAs(QUrl::fromLocalFile(deletedPath));
        ReplayController newSession;newSession.openProject(QUrl::fromLocalFile(deletedPath));waitLoading(newSession);
        check(newSession.videoLayers().isEmpty()&&source(newSession.sources(),"video").value("referenceCount").toInt()==0,"Controller project reload resurrected video");
        ReplayController repeated;repeated.openDemo();
        for(int i=0;i<100;++i){
            repeated.addVideo(QUrl::fromLocalFile(video));const auto id=repeated.videoLayers()[0].toMap().value("id").toString();
            check(source(repeated.sources(),"video").value("referenceCount").toInt()==1,"100-cycle add reference drift");
            repeated.removeLayers({id});check(repeated.videoLayers().isEmpty()&&source(repeated.sources(),"video").value("referenceCount").toInt()==0,"100-cycle deletion retained instance/references");
            repeated.undo();check(repeated.videoLayers().size()==1&&source(repeated.sources(),"video").value("referenceCount").toInt()==1,"100-cycle Undo reference drift");
            repeated.redo();check(repeated.videoLayers().isEmpty()&&repeated.sources().size()==1,"100-cycle Redo accumulated source catalog entries");
        }
        check(hash(video)==originalVideo&&hash(other)==originalOther&&hash(audio)==originalAudio&&hash(picture)==originalPicture,"A CRUD operation modified original material bytes");
        std::cout<<"Element lifecycle state checks passed: current references, no-op/refused edits, shared sources, audio remapping, kind-isolated replacement, batch Undo/Redo, complete state restoration, reopen and 100 cycles; source SHA-256 unchanged.\n";
        std::cout<<"These are actual C++ model operations on explicit fixtures, not GUI/decoder/resource or real flight acceptance.\n";return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}

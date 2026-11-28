#include "movie_exporter.h"
#include "replay_controller.h"
#include "cloud_view.h"
#include "canvas_layer_model.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <functional>
#include <memory>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void wait(const std::function<bool()> &done, qint64 timeout = 7000)
{
    QElapsedTimer deadline; deadline.start();
    while (!done() && deadline.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(1); }
    require(done(), "Movie preflight did not complete asynchronously");
}
void fixture(const QString &path, bool audio)
{
    QStringList args{"-v", "error", "-y", "-f", "lavfi", "-i", "color=c=blue:size=160x90:rate=24:duration=1"};
    if (audio) args << "-f" << "lavfi" << "-i" << "sine=frequency=440:sample_rate=48000:duration=1";
    args << "-vf" << "setpts=PTS+5/TB";
    if (audio) args << "-af" << "asetpts=PTS+5/TB" << "-c:a" << "aac";
    args << "-c:v" << "libx264" << path;
    QProcess process; process.start(QStandardPaths::findExecutable("ffmpeg"), args);
    require(process.waitForStarted() && process.waitForFinished(10000) && process.exitCode() == 0, "Cannot create movie preflight fixture");
}
void check(const QString &source, const QString &destination, bool cancel)
{
    ReplayController replay; replay.addVideo(QUrl::fromLocalFile(source)); replay.seek(.5); replay.togglePlayback();
    require(replay.playing() && replay.videoClips().size() == 1, "Cannot prepare movie preflight project");
    MovieImages images; MovieExporter movie(replay, images);
    QQuickItem canvas; canvas.setSize({160, 90});
    const QVariantMap original{{"preserved", "editing-state"}};
    canvas.setProperty("captureOptions", original);
    bool finished = false, succeeded = true, restored = false;
    QString error;
    QObject::connect(&movie, &MovieExporter::finished, [&](bool success, const QString &message) {
        finished = true; succeeded = success; error = message;
        restored = std::abs(replay.position() - .5) < 1.e-9 && replay.playing() &&
            canvas.property("captureOptions").toMap() == original;
    });
    movie.start(QUrl::fromLocalFile(destination), &canvas, .1, .2, 1., 0, 320, 240);
    require(movie.exporting() && !finished && !replay.playing(), "Audio preflight ran synchronously on the GUI thread");
    if (cancel) movie.cancel();
    wait([&] { return finished; });
    require(!succeeded && restored && !movie.exporting() && !QFileInfo::exists(destination),
        "Failed/cancelled audio preflight lost editor state or published an incomplete movie");
    require(cancel ? (error.contains("取消") || error.contains("cancel", Qt::CaseInsensitive)) :
        error.contains("audio", Qt::CaseInsensitive), "Audio preflight returned a misleading result");
}
void checkExactCameraValidation()
{
    ReplayController replay; replay.openDemo(); // Explicit generated camera-coverage fixture.
    CloudView main, inset;
    main.setProperty("exportViewId", "main"); inset.setProperty("exportViewId", "independent-view");
    for (auto *view : {&main, &inset}) {
        view->setController(&replay); view->setCamera(2); view->setSize({160, 90});
        view->setAuxiliary(view == &inset);
        view->setTargetFrame("__missing_tf__");
        require(view->exportValidationError(.5).contains("No valid target"), "Missing target was certified for export");
        view->setTargetFrame(""); view->setFollowLag(2.);
        // No renderer runs here: verification must use the requested time, never UI status.
        for (const double time : {5., .1, 3., 0., 4.}) {
            replay.seek(replay.duration() * .7);
            require(view->exportValidationError(time).isEmpty() == (time >= 2.),
                "Camera export used the current playhead or stale following status instead of exact source coverage");
        }
        view->setCamera(0); view->setTargetFrame("__missing_tf__");
        require(view->exportValidationError(.1).isEmpty(), "Independent map overview incorrectly requires a target");
    }
}

void exposeGraphicsWindow(QQuickWindow &window)
{
    window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    if (QGuiApplication::platformName() == "windows") {
        // Native test commands launch with STARTUPINFO/SW_HIDE. Windows applies
        // it to the first ShowWindow call; expose only this non-focusable test
        // window before requesting the export, without pumping a preview frame.
        window.show(); window.hide();
    }
    window.show();
}

void checkFirstGraphicsExport(const QString &directory, bool selected)
{
    // Actual QQuickFramebufferObject/OpenGL views, with no preview render before
    // the request. The generated demo is an explicit rendering control fixture.
    ReplayController replay; replay.openDemo(); replay.seek(.5); replay.togglePlayback();
    QQuickWindow window; window.setGeometry(0, 0, 160, 90);
    QQuickItem canvas(window.contentItem()); canvas.setSize({160, 90});
    CloudView main(&canvas), inset(&canvas);
    main.setProperty("exportViewId", "pending-main"); inset.setProperty("exportViewId", "pending-inset");
    main.setController(&replay); main.setSize({160, 90});
    inset.setController(&replay); inset.setSize({64, 36}); inset.setPosition({90, 4}); inset.setCamera(1);
    if (selected) {
        // This selected view becomes visible only at the requested experiment
        // time, so preflight must use the resolved target rather than preview.
        main.setVisible(false);
        QObject::connect(&replay, &ReplayController::positionChanged, &main, [&] { main.setVisible(replay.position() < .4); });
    }
    const QVariantMap original{{"preserved", "first-graphics-editor-state"}};
    canvas.setProperty("captureOptions", original);
    MovieImages images; MovieExporter movie(replay, images);
    bool finished = false, succeeded = false, restored = false, responsive = false;
    int captured = 0; QString message;
    QObject::connect(&movie, &MovieExporter::frameCaptured, [&](const QImage &pixels, double time) {
        ++captured;
        require(!pixels.isNull() && pixels.size() == QSize(320, 180) && std::abs(time - .25) < 1.e-9 &&
            std::abs(replay.position() - .25) < 1.e-9, "First graphics export used an incorrect size or experiment time");
    });
    QObject::connect(&movie, &MovieExporter::finished, [&](bool success, const QString &error) {
        finished = true; succeeded = success; message = error;
        restored = replay.playing() && std::abs(replay.position() - .5) < 1.e-9 && canvas.property("captureOptions").toMap() == original;
    });
    exposeGraphicsWindow(window);
    require(main.textureLimit() == 0 && inset.textureLimit() == 0, "First graphics fixture rendered before the export request");
    QVariantMap options{{"width", 320}, {"height", 180}, {"background", "#ffffff"}};
    if (selected) { options.insert("scope", "view"); options.insert("viewId", "pending-main"); }
    const auto destination = directory + (selected ? "/first-selected-view.png" : "/first-full-canvas.png");
    movie.startStill(QUrl::fromLocalFile(destination), &canvas, .25, options);
    require(movie.exporting() && !finished && !replay.playing(), "First graphics export rejected an uninitialized GPU synchronously");
    QTimer::singleShot(0, &movie, [&] { responsive = true; });
    try { wait([&] { return finished; }); }
    catch (...) {
        std::cerr << "First graphics state: visible=" << window.isVisible() << " exposed=" << window.isExposed()
            << " scenegraph=" << window.isSceneGraphInitialized() << " mainLimit=" << main.textureLimit()
            << " insetLimit=" << inset.textureLimit() << " mainVisible=" << main.isVisible()
            << " mainSize=" << main.width() << 'x' << main.height() << " message=" << movie.message().toStdString() << '\n';
        throw;
    }
    const QImage output(destination);
    require(succeeded && restored && responsive && captured == 1 && !movie.exporting() && !output.isNull() && output.size() == QSize(320, 180),
        qPrintable("First real OpenGL export failed or lost editing state: " + message));
    require(main.textureLimit() > 0 && !movie.captureMetrics().isEmpty(), "First output bypassed actual GPU capability/resolution reporting");
    require(movie.captureMetrics().size() == (selected ? 1 : 2), "GPU preflight checked the wrong resolved target views");
    int nonMatte = 0;
    const auto corner = output.pixelColor(0, 179);
    for (int y = 0; y < output.height(); ++y) for (int x = 0; x < output.width(); ++x)
        if (output.pixelColor(x, y) != corner) ++nonMatte;
    require(nonMatte > 100, "First real OpenGL image contains only an empty matte");
    replay.pause();
    if (!selected) {
        // A known hardware limit must still reject before any oversized FBO is
        // allocated. Restore geometry before processing another render event.
        main.setSize({double(main.textureLimit()) + 1., 90});
        finished = false; succeeded = true;
        const auto rejected = directory + "/gpu-oversize-must-not-exist.png";
        movie.startStill(QUrl::fromLocalFile(rejected), &canvas, .25, options);
        main.setSize({160, 90});
        require(finished && !succeeded && !movie.exporting() && !QFileInfo::exists(rejected)
            && message.contains("texture limits") && canvas.property("captureOptions").toMap() == original,
            "Asynchronous graphics preflight bypassed the known GPU texture limit");
    }
    window.hide();
}

void checkGraphicsFailure(const QString &directory, const QString &kind, bool timeout)
{
    ReplayController replay; replay.openDemo(); replay.seek(.5); replay.togglePlayback();
    QQuickWindow window; window.setGeometry(0, 0, 160, 90);
    QQuickItem canvas(window.contentItem()); canvas.setSize({160, 90});
    CloudView view(&canvas); view.setController(&replay); view.setSize({160, 90});
    const QVariantMap original{{"preserved", "failed-graphics-editor-state"}};
    canvas.setProperty("captureOptions", original);
    MovieImages images; MovieExporter movie(replay, images);
    bool finished = false, succeeded = true, restored = false, responsive = false;
    int captures = 0; QString message;
    QObject::connect(&movie, &MovieExporter::frameCaptured, [&](const QImage &, double) { ++captures; });
    QObject::connect(&movie, &MovieExporter::finished, [&](bool success, const QString &error) {
        finished = true; succeeded = success; message = error;
        restored = replay.playing() && std::abs(replay.position() - .5) < 1.e-9 && canvas.property("captureOptions").toMap() == original;
    });
    const auto destination = directory + "/graphics-" + kind + "-must-not-exist.png";
    exposeGraphicsWindow(window);
    QElapsedTimer elapsed; elapsed.start();
    movie.startStill(QUrl::fromLocalFile(destination), &canvas, .25, {{"width", 320}, {"height", 180}});
    require(movie.exporting() && !finished && view.textureLimit() == 0, "Graphics failure fixture was not pending asynchronously");
    QTimer::singleShot(0, &movie, [&] { responsive = true; });
    if (kind == "cancel") movie.cancel();
    else if (kind == "closed") window.hide();
    else if (kind == "context") {
        // Deliberately exercise the error signal boundary; this is not a fake
        // graphics-ready signal or a substitute for the real OpenGL test.
        window.sceneGraphError(QQuickWindow::ContextNotAvailable, "controlled context failure");
    }
    wait([&] { return finished; }, timeout ? 35000 : 7000);
    require(!succeeded && restored && !movie.exporting() && captures == 0 && !QFileInfo::exists(destination),
        "Graphics failure/cancellation published pixels or failed to restore playback and editing state");
    require(kind == "cancel" ? message.contains("取消") : kind == "closed" ? message.contains("closed") :
        kind == "context" ? message.contains("controlled context failure") : message.contains("within 30 seconds"),
        qPrintable("Graphics failure returned a misleading result: " + message));
    if (timeout) require(responsive && elapsed.elapsed() >= 29000 && elapsed.elapsed() < 35000 && view.textureLimit() == 0,
        "Actual 30-second graphics deadline blocked the UI, restarted, or certified an unavailable GPU");
    replay.pause(); window.hide();
}

void checkNativeSubstitutionGate(const QString &destination)
{
    ReplayController replay; replay.openDemo(); replay.seek(.5); replay.togglePlayback();
    MovieImages images; MovieExporter movie(replay, images);
    QQuickItem canvas; canvas.setSize({160, 90});
    const QVariantMap original{{"preserved", "native-editor-state"}};
    canvas.setProperty("captureOptions", original);
    canvas.setProperty("nativeSceneActive", true);
    // No host is connected. This checks the rejection boundary only and does
    // not claim a fake native scene/frame is an accepted rendering backend.
    bool finished = false, succeeded = true; QString error;
    QObject::connect(&movie, &MovieExporter::finished, [&](bool success, const QString &message) {
        finished = true; succeeded = success; error = message;
    });
    movie.startStill(QUrl::fromLocalFile(destination), &canvas, .2, {{"width", 320}, {"height", 240}});
    require(finished && !succeeded && error.contains("Native output source") && !QFileInfo::exists(destination)
        && replay.playing() && std::abs(replay.position() - .5) < 1.e-9 && !movie.exporting()
        && !movie.nativeFrameActive() && movie.nativeFrameUrl().isEmpty()
        && canvas.property("captureOptions").toMap() == original,
        "Disconnected native output fell back to legacy/preview pixels or changed editing state");
}

void checkTargetTimeViewResolution(const QString &directory)
{
    // An explicitly generated QtQuick control canvas exercises real model
    // visibility/keyframe bindings and actual offscreen image files. It does
    // not substitute for the independent/native 3D rendering acceptance.
    ReplayController replay; replay.openDemo(); replay.addCloudView(1);
    require(!replay.cloudViews().isEmpty(),"Cannot create temporal view control fixture");
    const auto viewId=replay.cloudViews().first().toMap().value("id").toString();
    const QVariantList ranges{QVariantList{.2,.4},QVariantList{.6,.9}};
    replay.canvas()->edit(viewId,{{"visibilityRangesEnabled",true},{"visibilityRanges",ranges}});
    replay.canvas()->addKeyframe(viewId,.2,{{"width",.25},{"height",.5}});
    replay.canvas()->addKeyframe(viewId,.8,{{"width",.75},{"height",.5}});
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("testReplay",&replay);
    engine.rootContext()->setContextProperty("exportViewIdentity",viewId);
    QQmlComponent component(&engine);
    component.setData(R"qml(
        import QtQuick
        Item {
            width: 160; height: 90
            property var captureOptions: ({})
            Rectangle {
                objectName: "temporalExportView"
                property string exportViewId: exportViewIdentity
                property rect exactGeometry: {
                    const revision = testReplay.canvas.revision
                    return testReplay.canvas.geometry(exportViewId, Qt.rect(0,0,80,45), Qt.size(160,90), testReplay.position)
                }
                x: 0; y: 0; width: exactGeometry.width; height: exactGeometry.height
                visible: { const revision = testReplay.canvas.revision; return testReplay.canvas.active(exportViewId,testReplay.position) }
                color: testReplay.position < .5 ? "#00c853" : "#2979ff"
            }
        }
    )qml",QUrl("test:/target-time-view.qml"));
    wait([&] { return !component.isLoading(); });
    require(component.isReady(),qPrintable("Cannot load temporal view control canvas: "+component.errorString()));
    std::unique_ptr<QObject> object(component.create());auto *canvas=qobject_cast<QQuickItem *>(object.get());
    require(canvas,"Cannot instantiate temporal view control canvas");
    QQuickWindow window;window.setGeometry(0,0,160,90);canvas->setParentItem(window.contentItem());window.show();
    wait([&] { return window.isSceneGraphInitialized(); });
    MovieImages images;MovieExporter movie(replay,images);
    int index=0;
    const auto run=[&](double previous,double target,bool wasPlaying,bool expected){
        replay.pause();replay.seek(previous);if(wasPlaying)replay.togglePlayback();
        const QVariantMap original{{"preserved","temporal-editor-state"}};canvas->setProperty("captureOptions",original);
        const auto destination=directory+"/temporal-view-"+QString::number(index++)+".png";
        const auto geometry=replay.canvas()->geometry(viewId,QRectF(0,0,80,45),QSizeF(160,90),target);
        const auto expectedPixels=geometry.size().toSize().scaled(320,240,Qt::KeepAspectRatio);
        bool finished=false,succeeded=false,restored=false,captured=false;QString message;QImage pixels;
        QObject observer;
        QObject::connect(&movie,&MovieExporter::frameCaptured,&observer,[&](const QImage &image,double time){
            captured=true;pixels=image;require(std::abs(time-target)<1e-9&&std::abs(replay.position()-target)<1e-9,"Selected view output used the wrong experiment time");
        });
        QObject::connect(&movie,&MovieExporter::finished,&observer,[&](bool success,const QString &error){
            finished=true;succeeded=success;message=error;
            restored=std::abs(replay.position()-previous)<1e-9&&replay.playing()==wasPlaying&&canvas->property("captureOptions").toMap()==original;
        });
        movie.startStill(QUrl::fromLocalFile(destination),canvas,target,{{"scope","view"},{"viewId",viewId},{"width",320},{"height",240},{"background","transparent"}});
        require(movie.exporting()&&!finished&&!replay.playing(),"Temporal view resolution did not defer until exact-time bindings");
        wait([&] { return finished; });
        require(succeeded==expected&&restored&&!movie.exporting(),qPrintable("Temporal view outcome/state restoration failed: "+message));
        if(expected){
            const QImage output(destination);const auto color=QColor(target<.5?"#00c853":"#2979ff");
            require(captured&&pixels.size()==expectedPixels&&!output.isNull()&&output.size()==QSize(320,240),"Selected keyframe geometry or real output dimensions changed");
            require(output.pixelColor(160,120)==color&&output.text("Description").contains(QString::number(target,'f',9)),"Selected output pixels or experiment timestamp do not match the target");
        }else require(!captured&&!QFileInfo::exists(destination)&&message.contains("requested experiment time"),"Invisible target view was rendered or returned a misleading failure");
        replay.pause();
    };
    run(.5,.3,false,true);  // Initially hidden, first visible segment.
    run(.5,.7,true,true);   // Same original time, another segment and keyframe geometry.
    run(.3,.5,true,false);  // Initially visible, hidden target must fail and resume playback.
    run(.7,.9,false,false);// Half-open end of the second segment.
    run(.5,.2,false,true);  // Exact beginning is visible.
    run(.8,.4,false,false);// Exact end of the first segment is hidden.
    window.hide();canvas->setParentItem(nullptr);
}

QByteArray runTool(const QString &name,const QStringList &arguments)
{
    QProcess process;process.start(QStandardPaths::findExecutable(name),arguments);
    require(process.waitForStarted(5000)&&process.waitForFinished(20000),"Movie rate fixture/probe timed out");
    require(process.exitCode()==0,qPrintable(name+": "+QString::fromUtf8(process.readAllStandardError())));
    return process.readAllStandardOutput();
}

double toneFrequency(const QByteArray &samples,double begin,double end)
{
    constexpr int sampleRate=48000;
    const auto first=int(std::llround(begin*sampleRate)),last=int(std::llround(end*sampleRate));
    require(first>=0&&last>first&&last*qsizetype(sizeof(float))<=samples.size(),"Encoded audio interval is missing");
    int rising=0;double energy=0.;float previous=0.;
    for(int i=first;i<last;++i){
        float value;std::memcpy(&value,samples.constData()+i*sizeof(float),sizeof(float));
        if(i>first&&previous<0.f&&value>=0.f)++rising;
        energy+=double(value)*value;previous=value;
    }
    require(std::sqrt(energy/(last-first))>.03,"Speed-adjusted audio became silent");
    return double(rising)*sampleRate/(last-first);
}

void checkMovieRates(const QString &directory)
{
    const auto source=directory+"/rate-source-nonzero-pts.mp4";
    // Explicit generated moving-picture/two-tone control fixture. The audio
    // changes from 440 to 880 Hz at source time 1.25 s, with video/audio PTS +5 s.
    runTool("ffmpeg",{"-v","error","-y","-f","lavfi","-i","testsrc2=size=320x240:rate=24:duration=4",
        "-f","lavfi","-i","sine=frequency=440:sample_rate=48000:duration=1.25",
        "-f","lavfi","-i","sine=frequency=880:sample_rate=48000:duration=2.75",
        "-filter_complex","[0:v]setpts=PTS+5/TB[v];[1:a][2:a]concat=n=2:v=0:a=1,asetpts=PTS+5/TB[a]",
        "-map","[v]","-map","[a]","-fps_mode","vfr","-c:v","libx264","-crf","0","-c:a","aac",source});
    const auto sourceProbe=QJsonDocument::fromJson(runTool("ffprobe",{"-v","error","-select_streams","v:0",
        "-show_frames","-show_streams","-show_entries","frame=pts:stream=time_base","-of","json",source})).object();
    const auto sourceFrames=sourceProbe.value("frames").toArray();
    const auto timeBase=sourceProbe.value("streams").toArray().first().toObject().value("time_base").toString().split('/');
    require(sourceFrames.size()==96&&timeBase.size()==2,"Movie rate source presentation timeline is incomplete");
    const auto origin=sourceFrames.first().toObject().value("pts").toInteger();
    require(origin>0,"Movie rate fixture lost its nonzero PTS origin");
    QList<qint64> sourceNs;
    for(const auto &entry:sourceFrames)sourceNs.append(qint64(std::llround(
        double(entry.toObject().value("pts").toInteger()-origin)*timeBase[0].toDouble()/timeBase[1].toDouble()*1.e9)));
    const auto sourcePixels=runTool("ffmpeg",{"-v","error","-i",source,"-map","0:v:0","-fps_mode","passthrough",
        "-sws_flags","bicubic+accurate_rnd","-f","rawvideo","-pix_fmt","bgr24","pipe:1"});
    constexpr qsizetype frameBytes=320*240*3;
    require(sourcePixels.size()==96*frameBytes,"Independent movie rate source decode is incomplete");

    ReplayController replay;replay.addVideo(QUrl::fromLocalFile(source));
    require(replay.videoClips().size()==1,"Movie rate source was not added");
    QQmlEngine engine;auto *images=new MovieImages;engine.addImageProvider("movie",images);
    MovieExporter movie(replay,*images);
    engine.rootContext()->setContextProperty("rateMovie",&movie);
    QQmlComponent component(&engine);
    component.setData(R"qml(
        import QtQuick
        Rectangle {
            width:320; height:240; color:"black"
            property var captureOptions: ({})
            property bool outputDataPending: exactVideo.status === Image.Loading
            property string outputDataError: exactVideo.status === Image.Error ? "Exact fixture image failed" : ""
            Image {
                id:exactVideo; anchors.fill:parent; cache:false; asynchronous:false; smooth:false
                source:rateMovie.frames.length ? rateMovie.frames[0].url : ""
            }
        }
    )qml",QUrl("test:/movie-rate.qml"));
    wait([&]{return !component.isLoading();});require(component.isReady(),qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());auto *canvas=qobject_cast<QQuickItem *>(object.get());
    require(canvas,"Cannot create exact video rate canvas");
    QQuickWindow window;window.setGeometry(0,0,320,240);canvas->setParentItem(window.contentItem());window.show();
    wait([&]{return window.isSceneGraphInitialized();});
    for(const double rate:{2.,4.}){
        replay.pause();replay.seek(.35);replay.setMoviePreferences(rate,0);
        const auto projectPath=directory+"/rate-"+QString::number(rate)+".uavstudio";
        replay.saveProjectAs(QUrl::fromLocalFile(projectPath));
        ReplayController reopened;reopened.openProject(QUrl::fromLocalFile(projectPath));
        require(reopened.moviePreferences().value("speed").toDouble()==rate&&reopened.moviePreferences().value("audioIndex").toInt()==0
            &&reopened.videoClips().size()==1&&reopened.videoClips().front().timing.sourceSeconds(1.)==1.,
            "Project reopen lost rate/audio selection or changed source calibration");
        const auto destination=directory+"/rate-"+QString::number(rate)+".mp4";
        const QVariantMap original{{"preserved","rate-editor-state"}};canvas->setProperty("captureOptions",original);
        replay.togglePlayback();
        bool finished=false,succeeded=false,restored=false,exact=true;QString message,exactError;
        QList<QImage> captured;
        QObject observer;
        QObject::connect(&movie,&MovieExporter::frameCaptured,&observer,[&](const QImage &image,double time){
            const auto expected=.25+captured.size()*rate/24.;
            exact=exact&&std::abs(time-expected)<1e-9&&std::abs(replay.position()-expected)<1e-9&&image.size()==QSize(320,240);
            if(!exact&&exactError.isEmpty())exactError=QString("frame %1 experiment %2 expected %3, position %4, size %5x%6")
                .arg(captured.size()).arg(time,0,'f',9).arg(expected,0,'f',9).arg(replay.position(),0,'f',9).arg(image.width()).arg(image.height());
            const auto ordinal=std::upper_bound(sourceNs.begin(),sourceNs.end(),qint64(std::llround(time*1.e9)))-sourceNs.begin()-1;
            if(ordinal<0||ordinal>=sourceNs.size())exact=false;
            else for(int y=0;y<240&&exact;++y)for(int x=0;x<320&&exact;++x){
                const auto color=image.pixelColor(x,y);const auto offset=ordinal*frameBytes+(y*320+x)*3;
                exact=std::abs(color.red()-uchar(sourcePixels[offset+2]))<=3
                    &&std::abs(color.green()-uchar(sourcePixels[offset+1]))<=3
                    &&std::abs(color.blue()-uchar(sourcePixels[offset]))<=3;
                if(!exact&&exactError.isEmpty())exactError=QString("frame %1 source ordinal %2 pixel %3,%4 actual %5,%6,%7 expected %8,%9,%10")
                    .arg(captured.size()).arg(ordinal).arg(x).arg(y).arg(color.red()).arg(color.green()).arg(color.blue())
                    .arg(uchar(sourcePixels[offset+2])).arg(uchar(sourcePixels[offset+1])).arg(uchar(sourcePixels[offset]));
            }
            captured.append(image);
        });
        QObject::connect(&movie,&MovieExporter::finished,&observer,[&](bool success,const QString &error){
            finished=true;succeeded=success;message=error;
            restored=std::abs(replay.position()-.35)<1e-9&&replay.playing()&&canvas->property("captureOptions").toMap()==original;
        });
        movie.start(QUrl::fromLocalFile(destination),canvas,.25,2.25,rate,0,320,240,24);
        wait([&]{return finished;});replay.pause();
        const auto count=int(48/rate);const auto outputDuration=2./rate;
        require(succeeded&&restored&&!movie.exporting(),qPrintable("Fast movie export failed: "+message));
        require(exact&&captured.size()==count,qPrintable("Fast movie frame/time/source mismatch: "+exactError+QString("; count %1 expected %2").arg(captured.size()).arg(count)));
        const auto probe=QJsonDocument::fromJson(runTool("ffprobe",{"-v","error","-show_streams","-of","json",destination})).object();
        bool video=false,audio=false;
        for(const auto &entry:probe.value("streams").toArray()){
            const auto stream=entry.toObject();const auto kind=stream.value("codec_type").toString();
            const double duration=stream.value("duration").toString().toDouble();
            if(kind=="video")video=stream.value("nb_frames").toString().toInt()==count&&std::abs(duration-outputDuration)<1.e-6;
            if(kind=="audio")audio=std::abs(duration-outputDuration)<.03;
        }
        require(video&&audio,"Fast output has incorrect encoded video/audio duration or frame count");
        const auto decoded=runTool("ffmpeg",{"-v","error","-i",destination,"-map","0:v:0","-fps_mode","passthrough",
            "-sws_flags","bicubic+accurate_rnd","-f","rawvideo","-pix_fmt","rgb24","pipe:1"});
        require(decoded.size()==count*frameBytes,"Encoded fast movie frame sequence is incomplete");
        double difference=0.;
        for(int i=0;i<count;++i)for(int y=0;y<240;++y)for(int x=0;x<320;++x){
            const auto color=captured[i].pixelColor(x,y);const auto offset=i*frameBytes+(y*320+x)*3;
            difference+=std::abs(color.red()-uchar(decoded[offset]))+std::abs(color.green()-uchar(decoded[offset+1]))+std::abs(color.blue()-uchar(decoded[offset+2]));
        }
        require(difference/decoded.size()<5.,"Encoded fast movie contents differ from the exact captured frames");
        const auto samples=runTool("ffmpeg",{"-v","error","-i",destination,"-map","0:a:0","-ac","1","-ar","48000","-f","f32le","pipe:1"});
        require(std::abs(toneFrequency(samples,outputDuration*.125,outputDuration*.375)-440.)<24.
            &&std::abs(toneFrequency(samples,outputDuration*.625,outputDuration*.875)-880.)<24.,
            "Fast audio did not follow experiment time or preserve pitch through atempo chaining");
    }
    replay.openDemo();replay.seek(.25);replay.setMoviePreferences(1.,-1);const auto physical=replay.speed();
    require(physical!=QStringLiteral("—"),"Generated metric-invariance fixture has no speed data");
    for(const double rate:{2.,4.}){
        replay.seek(.25);replay.setMoviePreferences(1.,-1);
        replay.togglePlayback();wait([&]{return replay.position()>.35;});
        const auto before=replay.position();replay.setMoviePreferences(rate,-1);
        require(replay.playing()&&replay.position()==before,"Changing playback rate jumped or stopped the playhead");
        QElapsedTimer wall;wall.start();wait([&]{return replay.position()>before+.2;});replay.pause();
        require(std::abs((replay.position()-before)/(wall.nsecsElapsed()/1.e9)-rate)<.2,"Preview timer ignored the selected fast playback rate");
        replay.seek(.25);require(replay.speed()==physical,"Playback rate changed the physical speed measurement");
    }
    replay.setMoviePreferences(3.,-1);require(replay.moviePreferences().value("speed").toDouble()==4.,"Unsupported rate changed project preferences");
    window.hide();canvas->setParentItem(nullptr);
}
}
int main(int argc, char **argv)
{
    QSurfaceFormat format; format.setVersion(3, 3); format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24); format.setStencilBufferSize(8); QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    const bool graphics = app.arguments().contains("--gpu-preflight"), graphicsTimeout = app.arguments().contains("--gpu-timeout");
    QQuickWindow::setGraphicsApi(graphics ? QSGRendererInterface::OpenGL : QSGRendererInterface::Software);
    try {
        QTemporaryDir directory;
        if (graphics || graphicsTimeout) {
            if (graphics) {
                checkFirstGraphicsExport(directory.path(), false);
                checkFirstGraphicsExport(directory.path(), true);
                for (const auto &failure : {QStringLiteral("cancel"), QStringLiteral("closed"), QStringLiteral("context")})
                    checkGraphicsFailure(directory.path(), failure, false);
                std::cout << "Real first-render OpenGL canvas/selected-view PNGs, exact target time, GPU limits and cancellation/window/context failure restoration verified.\n";
            } else {
                // QQuickFramebufferObject cannot initialize in the actual Qt
                // software renderer. Wait the production 30-second deadline.
                checkGraphicsFailure(directory.path(), "timeout", true);
                std::cout << "Actual 30-second unavailable-GPU deadline and responsive state restoration verified.\n";
            }
            return 0;
        }
        const auto silent = directory.filePath("video-without-audio.mp4"), audio = directory.filePath("nonzero-origin-audio.mp4");
        fixture(silent, false); fixture(audio, true);
        checkExactCameraValidation();
        checkNativeSubstitutionGate(directory.filePath("native-must-not-exist.png"));
        checkTargetTimeViewResolution(directory.path());
        checkMovieRates(directory.path());
        check(silent, directory.filePath("must-not-exist.mp4"), false);
        check(audio, directory.filePath("cancelled-must-not-exist.mp4"), true);
        std::cout << "Exact main/inset camera coverage, disconnected native substitution rejection, target-time view/keyframe/visibility-range image output and failure restoration, actual 2x/4x movie source pixels/duration/audio timing and pitch, project rate recovery and preview clock, asynchronous audio preflight failure/cancellation verified.\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}

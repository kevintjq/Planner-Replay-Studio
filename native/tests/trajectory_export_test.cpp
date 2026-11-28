#include "trajectory_controller.h"
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QProcess>
#include <QPersistentModelIndex>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <opencv2/videoio.hpp>
#include <iostream>
#include <stdexcept>
#include <cmath>

void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool waitFor(TrajectoryController &controller) {
    if (!controller.working()) return true;
    QEventLoop loop; bool completed = false;
    auto connection = QObject::connect(&controller, &TrajectoryController::changed, &loop, [&] {
        if (!controller.working()) { completed = true; loop.quit(); }
    });
    QTimer::singleShot(30000, &loop, &QEventLoop::quit); loop.exec(); QObject::disconnect(connection); return completed;
}
bool waitForPreview(TrajectoryController &controller) {
    if (!controller.previewing()) return true;
    QEventLoop loop; bool completed = false;
    auto connection = QObject::connect(&controller, &TrajectoryController::changed, &loop, [&] {
        if (!controller.previewing()) { completed = true; loop.quit(); }
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit); loop.exec(); QObject::disconnect(connection); return completed;
}
QJsonObject manifest(const QString &directory) {
    QFile file(QDir(directory).filePath("manifest.json"));
    expect(file.open(QIODevice::ReadOnly), "Missing manifest");
    const auto result = QJsonDocument::fromJson(file.readAll()).object();
    expect(!result.isEmpty(), "Invalid manifest"); return result;
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        {
            TrajectoryCanvasModel layers; int resets = 0, removed = 0;
            QObject::connect(&layers, &QAbstractItemModel::modelReset, &app, [&] { ++resets; });
            QObject::connect(&layers, &QAbstractItemModel::rowsRemoved, &app, [&](const QModelIndex &, int first, int last) { removed += last - first + 1; });
            const QVariantMap background{{"id", "background"}, {"source", "stable-background"}};
            const QVariantMap first{{"id", "sprite:first"}, {"source", "stable-first"}};
            const QVariantMap second{{"id", "sprite:second"}, {"source", "stable-second"}};
            layers.setLayers({background, first, second});
            const QPersistentModelIndex persistentBackground(layers.index(0)), persistentSecond(layers.index(2));
            layers.setLayers({background, second});
            expect(resets == 0 && removed == 1 && persistentBackground.isValid() && persistentBackground.row() == 0 &&
                persistentSecond.isValid() && persistentSecond.row() == 1 && persistentSecond.data(TrajectoryCanvasModel::LayerRow).toMap() == second,
                "Deleting one canvas layer reset or invalidated unrelated persistent layer identities");
            auto changedSecond = second; changedSecond["source"] = "new-second-mask";
            layers.setLayers({changedSecond, background});
            expect(resets == 0 && persistentBackground.isValid() && persistentBackground.row() == 1 && persistentSecond.isValid() &&
                persistentSecond.row() == 0 && persistentSecond.data(TrajectoryCanvasModel::LayerRow).toMap() == changedSecond,
                "Layer content change or reordering recreated unrelated model identities");
        }
        {
            PreviewImages provider; auto track = std::make_shared<replay::FixedCameraTrajectory>();
            track->background = QImage(24, 24, QImage::Format_ARGB32_Premultiplied); track->background.fill(Qt::white);
            QImage sprite(12, 12, QImage::Format_ARGB32_Premultiplied); sprite.fill(Qt::red);
            const auto spritePath = directory.filePath("immutable-sprite.png"); expect(sprite.save(spritePath), "Cannot create immutable cache fixture");
            replay::TrajectoryFrame frame; frame.seconds = 0.; frame.bounds = {0, 0, 12, 12}; frame.cachedSprite = spritePath; frame.sourceCrop = sprite;
            track->frames.push_back(frame); const auto revision = provider.setTrajectory(track);
            const auto request = [&](const QString &version) { return provider.requestImage(QString("sprite/%1/0/%2").arg(revision).arg(version), nullptr, {}); };
            const auto base = request("base"); const auto baseKey = base.cacheKey(); expect(!base.isNull(), "Cannot populate immutable sprite cache");
            replay::TrajectoryEdits edits; edits.frames[0].opacity = .2; edits.frames[1].removed = true;
            provider.setEdits(edits);
            expect(request("base").cacheKey() == baseKey && provider.setTrajectory(track) == revision,
                "Opacity, unrelated deletion or same-snapshot undo invalidated unchanged image content");
            edits.frames[0].maskStrokes = {{{QPointF(.5, .5)}, .2, true}};
            const auto firstEdit = edits.frames[0]; const auto firstVersion = replay::trajectoryMaskContentKey(firstEdit);
            provider.setEdits(edits); // Queue no image yet: this version must remain reproducible after a newer edit.
            edits.frames[0].maskStrokes.push_back({{QPointF(.1, .1)}, .12, true}); provider.setEdits(edits);
            const auto firstMask = request(firstVersion), latestMask = request(replay::trajectoryMaskContentKey(edits.frames[0]));
            expect(!firstMask.isNull() && firstMask == replay::trajectorySprite(frame, firstEdit) && firstMask != latestMask &&
                request("base").cacheKey() == baseKey && request("base").pixelColor(6, 6).alpha() == 255 && firstMask.pixelColor(6, 6).alpha() < 255,
                "Queued immutable mask version used newer pixels or invalidated a different image");
            expect(request("unknown-content-version").isNull(), "Unrecognized content version was answered using current pixels");
        }
        const auto input = directory.filePath("source.avi");
        cv::VideoWriter writer(input.toStdString(), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 12., {240, 160});
        expect(writer.isOpened(), "Cannot create video fixture");
        for (int i = 0; i < 24; ++i) writer.write(cv::Mat(160, 240, CV_8UC3, i < 12 ? cv::Scalar(255, 0, 0) : cv::Scalar(0, 0, 255)));
        writer.release();
        PreviewImages images; TrajectoryController controller(images);
        controller.open(QUrl::fromLocalFile(input)); expect(waitFor(controller), "Initial decode timed out");
        controller.preview(1.25); expect(waitFor(controller), "Time selection timed out");
        const auto preview = controller.image(); const auto settings = controller.settings();
        bool exported = false, success = false;
        QObject::connect(&controller, &TrajectoryController::frameExported, &app, [&](bool ok, const QString &) { exported = true; success = ok; });
        for (const auto &format : {"png", "tiff", "pdf", "svg"}) {
            const auto path = directory.filePath(QString("frame.%1").arg(format));
            exported = false;
            controller.exportFrame(QUrl::fromLocalFile(path), {{"width", 3840}, {"height", 2160}, {"dpi", 600}, {"background", "transparent"}});
            expect(waitFor(controller) && exported && success, "Frame export failed");
            expect(controller.image() == preview && controller.settings() == settings && !controller.hasResult(), "Export changed preview or edits");
            if (QString(format) == "png" || QString(format) == "tiff") {
                const QImage image(path);
                expect(image.size() == QSize(3840, 2160) && image.pixelColor(0, 0).alpha() == 0 && image.pixelColor(1920, 1080).red() > 200,
                    "Wrong dimensions, alpha or experiment-time source frame");
                expect(std::abs(image.dotsPerMeterX() * .0254 - 600) < .1, "DPI changed");
                if (QString(format) == "png") expect(image.text("Description").contains("1.250000000") &&
                    image.text("Description").contains("pts=") && image.text("Description").contains("time_base=") &&
                    image.text("Description").contains("sample_aspect_ratio=1/1") && image.text("Description").contains("coded_size=240x160"),
                    "Missing source timestamp/pixel geometry provenance");
            } else expect(QFileInfo(path).size() > 1000, "Empty document");
        }
        exported = false;
        const auto sourceLayerPath=directory.filePath("selected-source.png");
        controller.exportFrame(QUrl::fromLocalFile(sourceLayerPath),{{"scope","layers"},{"layerIds",QVariantList{"background"}},{"width",240},{"height",160}});
        expect(waitFor(controller)&&exported&&success&&QImage(sourceLayerPath).pixelColor(120,80).red()>240,
            "Selected source-video background was not rendered from the exact source frame");
        expect(controller.frameExportReport().value("scope")=="layers"&&controller.frameExportReport().value("content")=="source"&&
            !controller.frameExportReport().value("foregroundGateApplied").toBool()&&controller.frameExportReport().value("composedUavInstances").toInt()==0&&
            std::abs(controller.frameExportReport().value("time").toDouble()-1.25)<1.e-9&&controller.settings()==settings,
            "Selected source export lost its explicit scope, source time or editor state");
        exported=false;
        controller.exportFrame(QUrl::fromLocalFile(directory.filePath("empty-selection.png")),{{"scope","layers"},{"layerIds",QVariantList{}}});
        expect(exported&&!success&&!QFile::exists(directory.filePath("empty-selection.png")),"Empty selected-layer export was accepted");
        exported = false; controller.exportFrame(QUrl::fromLocalFile(directory.filePath("invalid.png")), {{"width", 100000}});
        expect(exported && !success && !QFile::exists(directory.filePath("invalid.png")), "Unsafe dimensions accepted");
        bool batchDone = false, batchOk = false;
        QObject::connect(&controller, &TrajectoryController::batchExported, &app, [&](bool ok, const QString &) { batchDone = true; batchOk = ok; });
        QVariantMap options{{"width", 640}, {"height", 360}, {"dpi", 600}, {"background", "transparent"}, {"mode", "manual"},
            {"times", "1.25, .27, .25"}, {"format", "png"}, {"figure", true}, {"figureFormat", "svg"}, {"columns", 2}};
        controller.exportFrames(QUrl::fromLocalFile(directory.path()), options);
        expect(controller.working(), "Batch did not lock editor");
        controller.preview(0.); controller.open(QUrl::fromLocalFile(input)); controller.configure(.7, .3);
        controller.undo(); controller.redo(); controller.resetPreview();
        expect(controller.image() == preview && controller.settings() == settings, "Programmatic edit mutated active export snapshot");
        expect(waitFor(controller) && batchDone && batchOk, "Source batch failed");
        const auto batch = manifest(controller.exportDirectory());
        expect(batch["status"] == "complete" && batch["completed"].toArray().size() == 3, "Incomplete source manifest");
        const auto frames = batch["completed"].toArray();
        for (int i = 0; i < frames.size(); ++i) {
            const auto entry = frames[i].toObject(); const QImage image(QDir(controller.exportDirectory()).filePath(entry["file"].toString()));
            expect(image.size() == QSize(640, 360) && image.pixelColor(0, 0).alpha() == 0, "Batch dimensions or alpha changed");
            expect(i < 2 ? image.pixelColor(320, 180).blue() > 200 : image.pixelColor(320, 180).red() > 200, "Batch used preview time for source frames");
            expect(std::abs(entry["source_frame_time"].toDouble() - (i < 2 ? .25 : 1.25)) < 1.e-8, "Wrong selected decoder time");
        }
        expect(std::abs(frames[1].toObject()["requested_time"].toDouble() - .27) < 1.e-8, "Requested time was replaced by frame time");
        const auto stamp = frames[0].toObject()["source_timestamp"].toObject();
        if (!(stamp["coded_size"].toArray() == QJsonArray{240, 160} && stamp["sample_aspect_ratio"].toArray() == QJsonArray{1, 1} &&
            stamp["display_size"].toArray() == QJsonArray{240, 160} && stamp["pixel_normalization"] == "square_pixels_then_display_rotation"))
            throw std::runtime_error("Batch source pixel geometry provenance missing: " + QJsonDocument(stamp).toJson(QJsonDocument::Compact).toStdString());
        QFile figure(QDir(controller.exportDirectory()).filePath("figure.svg"));
        expect(figure.open(QIODevice::ReadOnly), "Figure missing"); const auto svg = figure.readAll();
        expect(svg.contains("<image") && svg.contains("<text") && svg.contains("(a)") && svg.contains("1.250000000"), "Missing raster panels or real vector captions");
        expect(controller.image() == preview && controller.settings() == settings, "Batch changed editor state");

        auto track = std::make_shared<replay::FixedCameraTrajectory>();
        track->background = QImage(240, 160, QImage::Format_ARGB32_Premultiplied); track->background.fill(Qt::black);
        QImage sprite(10, 10, QImage::Format_ARGB32_Premultiplied); sprite.fill(Qt::red);
        for (int i = 0; i < 4; ++i) track->frames.push_back({i * .5, QRect(10 + 50 * i, 70, 10, 10), sprite, {}});
        replay::TrajectoryEdits edits; edits.showLine = true;
        edits.ranges = {{"excluded", replay::trajectoryTimestamp(.5), replay::trajectoryTimestamp(.75), replay::Both}};
        edits.frames[replay::trajectoryTimestamp(1.)].opacity = .5;
        QVariantMap tracked{{"content", "trajectory"}, {"mode", "manual"}, {"times", ".25, 1, 1.5"},
            {"width", 240}, {"height", 160}, {"background", "white"}};
        const auto snapshots = replay::exportTrajectoryBatch(input, directory.path(), track, {0, 1, 2, 3}, edits, 1., 1.25, tracked);
        expect(snapshots.error.isEmpty() && snapshots.completed == 3, "Trajectory snapshot batch failed");
        auto vectorEdits = edits;
        vectorEdits.line = {"#1976d2", 1.25, .7, 2};
        vectorEdits.start.enabled = vectorEdits.goal.enabled = true;
        vectorEdits.start.showTime = vectorEdits.goal.showTime = true;
        expect(replay::TrajectoryEdits::fromJson(vectorEdits.toJson()) == vectorEdits, "Fractional line and independent marker styles were not persisted");
        const auto vectorPath = directory.filePath("true-vector-trajectory.svg");
        expect(replay::writeTrajectoryStillImage(*track,{0,1,2,3},vectorEdits,1.,{},vectorPath,{1920,1280},600,1.5,Qt::transparent).isEmpty(), "Vector trajectory export failed");
        QFile vectorFile(vectorPath); expect(vectorFile.open(QIODevice::ReadOnly), "Vector trajectory SVG missing");
        const auto vectorBytes = vectorFile.readAll();
        expect(vectorBytes.count("<image") >= 4 && (vectorBytes.contains("<path") || vectorBytes.contains("<polyline")) &&
            (vectorBytes.contains("<ellipse") || vectorBytes.contains("<circle")) && vectorBytes.contains("<text") && vectorBytes.contains("Start") && vectorBytes.contains("Goal"),
            "Trajectory SVG flattened its path, endpoint circles or text into a single raster");
        expect(replay::writeTrajectoryStillImage(*track,{0,1,2,3},vectorEdits,1.,{},directory.filePath("true-vector-trajectory.pdf"),{1920,1280},600,1.5,Qt::white).isEmpty(), "Vector trajectory PDF failed");
        // A document's dark text needs a light halo over a dark photograph,
        // while user-selected white text needs a dark halo over a white plate.
        // This must be shared by preview/export and retain vector SVG text.
        for (const bool lightText : {false,true}) {
            auto captionEdits = vectorEdits;
            captionEdits.start.showTime = false;
            captionEdits.start.textColor = lightText ? "#ffffff" : "#202124";
            const auto path = directory.filePath(lightText ? "white-caption.png" : "dark-caption.png");
            expect(replay::writeTrajectoryStillImage(*track,{0,1,2,3},captionEdits,1.,{},path,{240,160},300,1.5,
                lightText ? Qt::white : Qt::black,{},QStringList{"start"}).isEmpty(), "Caption-only contrast export failed");
            const QImage captionImage(path);
            int contrastingHaloPixels = 0, textPixels = 0;
            for (int y = 64; y < 85; ++y) for (int x = 26; x < 80; ++x) {
                const int value = qGray(captionImage.pixel(x,y));
                contrastingHaloPixels += lightText ? value < 100 : value > 155;
                textPixels += lightText ? value > 225 : value < 70;
            }
            expect(contrastingHaloPixels > 12 && textPixels > 12,
                "Endpoint caption lacks a contrasting halo over matching dark/light output background");
        }
        QVariantMap selectionReport;
        const auto backgroundOnly=directory.filePath("selected-background.png");
        expect(replay::writeTrajectoryStillImage(*track,{0,1,2,3},vectorEdits,1.,{},backgroundOnly,{240,160},300,1.5,Qt::transparent,{},QStringList{"background"},&selectionReport).isEmpty(),
            "Explicit background-only selected-layer export was rejected by the whole-trajectory gate");
        expect(QImage(backgroundOnly).pixelColor(15,75)==QColor(Qt::black)&&selectionReport.value("scope")=="layers"&&
            selectionReport.value("composedUavInstances").toInt()==0&&selectionReport.value("paintedLayers").toInt()==1&&
            !selectionReport.value("foregroundGateApplied").toBool()&&!selectionReport.value("cleanForegroundAccepted").toBool(),
            "Selected background leaked UAV layers or claimed complete foreground acceptance");
        const auto foregroundOnly=directory.filePath("selected-uav.png");
        expect(replay::writeTrajectoryStillImage(*track,{0,1,2,3},vectorEdits,1.,{},foregroundOnly,{240,160},300,1.5,Qt::transparent,{},QStringList{"sprite:0"},&selectionReport).isEmpty(),
            "Independent selected UAV export failed");
        const QImage isolatedUav(foregroundOnly);
        expect(isolatedUav.pixelColor(15,75).red()>240&&isolatedUav.pixelColor(0,0).alpha()==0&&isolatedUav.pixelColor(165,75).alpha()==0&&
            selectionReport.value("composedUavInstances").toInt()==1&&isolatedUav.text("Description").contains("export_scope=layers")&&
            isolatedUav.text("Description").contains("composed_uav_instances=1"),"Selected UAV output retained unselected content or lost scope/count metadata");
        QVariantMap selectedText{{"id","caption"},{"kind","text"},{"text","Selected Caption"},{"x",.1},{"y",.1},{"width",.8},{"height",.2},{"designWidth",240.},
            {"style",QVariantMap{{"fontSize",14.},{"color","#112233"}}}};
        QVariantMap selectedState{{"elements",QVariantList{selectedText}}};
        const auto selectedVector=directory.filePath("selected-vectors.svg");
        expect(replay::writeTrajectoryStillImage(*track,{0,1,2,3},vectorEdits,1.,selectedState,selectedVector,{1920,1280},600,1.5,Qt::transparent,{},QStringList{"line","start","caption"},&selectionReport).isEmpty(),
            "Selected native and scientific vector export failed");
        QFile selectedFile(selectedVector);expect(selectedFile.open(QIODevice::ReadOnly),"Selected vector SVG missing");const auto selectedBytes=selectedFile.readAll();
        const bool selectedVectorValid=!selectedBytes.contains("<image")&&(selectedBytes.contains("<path")||selectedBytes.contains("<polyline"))&&
            (selectedBytes.contains("<circle")||selectedBytes.contains("<ellipse"))&&selectedBytes.contains("<text")&&selectedBytes.contains("Selected")&&selectedBytes.contains("Caption")&&
            selectedBytes.contains("export_scope=layers")&&selectionReport.value("composedUavInstances").toInt()==0&&selectionReport.value("paintedLayers").toInt()==3;
        if(!selectedVectorValid)throw std::runtime_error(QString("Selected vector mismatch: image=%1 path=%2 polyline=%3 circle=%4 ellipse=%5 text=%6 scope=%7; report=%8")
            .arg(selectedBytes.count("<image")).arg(selectedBytes.count("<path")).arg(selectedBytes.count("<polyline")).arg(selectedBytes.count("<circle")).arg(selectedBytes.count("<ellipse"))
            .arg(selectedBytes.contains("<text")&&selectedBytes.contains("Selected")&&selectedBytes.contains("Caption")).arg(selectedBytes.contains("export_scope=layers"))
            .arg(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(selectionReport)).toJson(QJsonDocument::Compact))).toStdString());
        for (const auto &invalidSelection : {QStringList{},QStringList{"background","missing-layer"}}) {
            const auto invalidSelectedPath=directory.filePath("invalid-selected.svg");
            expect(!replay::writeTrajectoryStillImage(*track,{0,1,2,3},vectorEdits,1.,selectedState,invalidSelectedPath,{240,160},300,1.5,Qt::transparent,{},invalidSelection,&selectionReport).isEmpty()&&
                !QFile::exists(invalidSelectedPath)&&!selectionReport.value("success").toBool(),"Empty or partially invalid layer selection produced a successful file");
        }
        selectedState["items"]=QVariantMap{{"caption",QVariantMap{{"visible",false}}}};
        expect(!replay::writeTrajectoryStillImage(*track,{0,1,2,3},vectorEdits,1.,selectedState,directory.filePath("hidden-selected.png"),{240,160},300,1.5,Qt::transparent,{},QStringList{"caption"},&selectionReport).isEmpty()&&
            !QFile::exists(directory.filePath("hidden-selected.png")),"Hidden selected layers exported an empty successful image");
        auto missingUnselected=*track;missingUnselected.frames[0].cachedSprite=directory.filePath("missing-unselected.png");
        expect(replay::writeTrajectoryStillImage(missingUnselected,{0,1,2,3},vectorEdits,1.,{},directory.filePath("background-with-missing-uav.svg"),{240,160},300,1.5,Qt::transparent,{},QStringList{"background"},&selectionReport).isEmpty(),
            "Unselected foreground cache prevented explicit background-only export");
        auto selectedBatchOptions=tracked;selectedBatchOptions["scope"]="layers";selectedBatchOptions["layerIds"]=QStringList{"background"};
        const auto selectedBatch=replay::exportTrajectoryBatch(input,directory.path(),track,{0,1,2,3},vectorEdits,1.,1.5,selectedBatchOptions);
        expect(!selectedBatch.error.isEmpty()&&selectedBatch.directory.isEmpty(),"Unsupported selected-layer batch silently exported a different scope");
        QVariantMap relocated{{"items", QVariantMap{{"sprite:0", QVariantMap{{"x", .5}, {"y", .1}, {"width", .1}, {"height", .1}}}}}};
        replay::TrajectoryCompositionStats relocatedCounts;
        const auto relocatedImage = replay::renderTrajectory(*track,{0},1.,{},{},{},{},false,&relocatedCounts,true,relocated);
        expect(relocatedCounts.composed == 1 && relocatedImage.pixelColor(130,20).red() > 240 && relocatedImage.pixelColor(15,75) == QColor(Qt::black), "Canvas transform did not change the shared CPU composition");
        auto hidden = relocated; hidden["items"] = QVariantMap{{"sprite:0",QVariantMap{{"visible",false}}}};
        expect(!replay::writeTrajectoryStillImage(*track,{0},{},1.,hidden,directory.filePath("hidden-uav.png"),{240,160},300,0.,Qt::white).isEmpty() &&
            !QFile::exists(directory.filePath("hidden-uav.png")), "Hidden foreground passed the nonempty UAV export gate");
        auto outside = relocated; outside["items"] = QVariantMap{{"sprite:0",QVariantMap{{"x",2.}}}};
        expect(!replay::writeTrajectoryStillImage(*track,{0},{},1.,outside,directory.filePath("outside-uav.svg"),{240,160},300,0.,Qt::white).isEmpty() &&
            !QFile::exists(directory.filePath("outside-uav.svg")), "Off-canvas foreground passed the nonempty UAV export gate");
        auto lineOnly = vectorEdits; lineOnly.lineOnly = true;
        QVariantMap outsideLine{{"items",QVariantMap{{"line",QVariantMap{{"x",2.}}}}}};
        expect(!replay::writeTrajectoryStillImage(*track,{0,1,2,3},lineOnly,1.,outsideLine,directory.filePath("outside-line.svg"),{240,160},300,1.5,Qt::white).isEmpty(),
            "Off-canvas path passed the line-only export gate");
        QVariantMap shape{{"id","shape-1"},{"kind","rectangle"},{"x",0.},{"y",.4},{"width",.2},{"height",.2},{"z",50.},
            {"style",QVariantMap{{"fill","#00ff00"},{"strokeWidth",0.}}}};
        QVariantMap scientific{{"elements",QVariantList{shape}}};
        auto scientificImage = replay::renderTrajectory(*track,{0},1.,{},{},{},{},false,nullptr,true,scientific,0.);
        expect(scientificImage.pixelColor(15,75).red() > 240 && scientificImage.pixelColor(5,75).green() > 240,
            "Scientific element did not render below the trajectory sprite at its shared z position");
        shape["z"] = 500.; scientific["elements"] = QVariantList{shape};
        scientificImage = replay::renderTrajectory(*track,{0},1.,{},{},{},{},false,nullptr,true,scientific,0.);
        expect(scientificImage.pixelColor(15,75).green() > 240 && scientificImage.pixelColor(15,75).red() < 10,
            "Scientific element always rendered below native trajectory layers");
        auto appearance = scientific;
        auto emptyShape = shape; emptyShape["style"] = QVariantMap{{"fill","transparent"},{"strokeWidth",0.}};
        emptyShape["backgroundMode"] = "solid"; emptyShape["backgroundColor"] = "#0000ff"; emptyShape["backgroundOpacity"] = .5;
        appearance["elements"] = QVariantList{emptyShape};
        const auto appearanceImage = replay::renderTrajectory(*track,{0},1.,{},{},{},{},false,nullptr,true,appearance,0.);
        expect(std::abs(appearanceImage.pixelColor(5,75).blue()-128)<=1,"Shared layer Appearance was absent from trajectory image output");
        scientific["items"] = QVariantMap{{"shape-1",QVariantMap{{"keyframes",QVariantList{
            QVariantMap{{"time",0.},{"x",0.}},QVariantMap{{"time",1.5},{"x",.75}}}}}}};
        auto timed = tracked; timed["times"] = "0, 1.5";
        const auto timedBatch = replay::exportTrajectoryBatch(input,directory.path(),track,{0,1,2,3},{},1.,.75,timed,{},{},scientific);
        expect(timedBatch.error.isEmpty() && timedBatch.completed == 2,"Scientific keyframe batch failed");
        const auto timedFiles = manifest(timedBatch.directory)["completed"].toArray();
        const QImage early(QDir(timedBatch.directory).filePath(timedFiles[0].toObject()["file"].toString()));
        const QImage late(QDir(timedBatch.directory).filePath(timedFiles[1].toObject()["file"].toString()));
        expect(early.pixelColor(5,75).green()>240 && early.pixelColor(185,75)==QColor(Qt::black) &&
            late.pixelColor(5,75)==QColor(Qt::black) && late.pixelColor(185,75).green()>240,
            "Batch scientific elements used the last trajectory time instead of each requested timestamp");
        const auto snapshotsManifest = manifest(snapshots.directory)["completed"].toArray();
        for (int i = 0; i < 3; ++i) {
            const QImage image(QDir(snapshots.directory).filePath(snapshotsManifest[i].toObject()["file"].toString()));
            expect(image.pixelColor(15, 75).red() > 240 && image.pixelColor(65, 75) == QColor(Qt::black), "Snapshot lost first frame or ignored typed exclusion");
            expect(i == 0 ? image.pixelColor(115, 75) == QColor(Qt::black) : std::abs(image.pixelColor(115, 75).red() - 128) <= 1, "Future frame leaked or opacity changed");
            expect(i == 2 ? image.pixelColor(165, 75).red() > 240 : image.pixelColor(165, 75) == QColor(Qt::black), "Snapshot cutoff excluded endpoint or included future sprite");
        }
        tracked.insert("times", ".25"); edits.trim = {replay::trajectoryTimestamp(1.), replay::trajectoryTimestamp(1.5)};
        const auto trimmed = replay::exportTrajectoryBatch(input, directory.path(), track, {0, 1, 2, 3}, edits, 1., 1.25, tracked);
        expect(!trimmed.error.isEmpty() && trimmed.completed == 0, "A snapshot with no visible UAV before the generation trim was reported successful");

        bool cancel = false;
        options.insert("figure", false); options.insert("mode", "interval"); options.insert("begin", 0.); options.insert("end", 1.5); options.insert("interval", .1);
        const auto cancelled = replay::exportTrajectoryBatch(input, directory.path(), {}, {}, {}, 1., 1.25, options,
            [&] { return cancel; }, [&](double) { cancel = true; });
        expect(cancelled.cancelled && cancelled.completed == 1 && manifest(cancelled.directory)["status"] == "cancelled", "Cancellation did not retain completed files and manifest");
        const auto previousDirectories = QDir(directory.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        options.insert("mode", "manual"); options.insert("times", ".25"); options.insert("pattern", "../unsafe");
        const auto rejected = replay::exportTrajectoryBatch(input, directory.path(), {}, {}, {}, 1., 1.25, options);
        expect(!rejected.error.isEmpty() && rejected.directory.isEmpty() && QDir(directory.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot) == previousDirectories, "Invalid batch created files");
        options.remove("pattern"); options.insert("mode", "samples"); options.insert("times", "3, 15"); options.insert("sampleFps", 12.);
        const auto grid = replay::exportTrajectoryBatch(input, directory.path(), {}, {}, {}, 1., 1.25, options);
        expect(grid.error.isEmpty() && grid.completed == 2, "Sampling-grid batch failed");
        tracked.insert("mode", "tracked");
        const auto actualSamples = replay::exportTrajectoryBatch(input, directory.path(), track, {0, 2, 3}, {}, 1., 1.25, tracked);
        expect(actualSamples.error.isEmpty() && actualSamples.completed == 3, "Actual tracked-sample batch failed");
        track->frames[0].cachedSprite = directory.filePath("missing-sprite.png");
        const auto missing = replay::exportTrajectoryBatch(input, directory.path(), track, {0}, {}, 1., 1.25, tracked);
        expect(!missing.error.isEmpty() && missing.completed == 0 && manifest(missing.directory)["status"] == "failed", "Missing cached pixels silently exported incomplete trajectory");
        options.insert("mode", "interval"); options.insert("begin", 0.); options.insert("end", 1.5); options.insert("interval", .01);
        controller.exportFrames(QUrl::fromLocalFile(directory.path()), options);
        auto restoredSettings = settings; restoredSettings.insert("start", .5);
        controller.restoreSettings(restoredSettings);
        expect(waitFor(controller), "Project change did not cancel active batch");
        expect(waitFor(controller) && std::abs(controller.startTime() - .5) < 1.e-9 && controller.settings() == restoredSettings,
            "Pending project restore was lost or raced the export worker");
        controller.removeVideo();
        expect(controller.fileName().isEmpty() && controller.settings().isEmpty() && controller.image().isEmpty() && QFile::exists(input) && controller.canUndo(), "Remove video did not remove project state or deleted source media");
        controller.undo();
        expect(controller.settings() == restoredSettings && !controller.image().isEmpty(), "Undo did not restore removed video state");
        controller.redo();
        expect(controller.settings().isEmpty() && controller.image().isEmpty(), "Redo did not remove video state");
        controller.undo();
        const auto anamorphic = directory.filePath("anamorphic.mp4");
        QProcess fixture;
        fixture.start(QStandardPaths::findExecutable("ffmpeg"), {"-v", "error", "-y", "-f", "lavfi", "-i",
            "testsrc2=size=144x96:rate=30:duration=0.2", "-vf", "setsar=4/3", "-c:v", "libx264", "-crf", "0", anamorphic});
        expect(fixture.waitForStarted() && fixture.waitForFinished(10000) && fixture.exitCode() == 0, "Cannot create anamorphic export fixture");
        const auto sarBatch = replay::exportTrajectoryBatch(anamorphic, directory.path(), {}, {}, {}, 1., 0.,
            {{"mode", "manual"}, {"times", "0"}, {"width", 320}, {"height", 240}, {"background", "transparent"}});
        expect(sarBatch.error.isEmpty() && sarBatch.completed == 1, "Anamorphic source export failed");
        const auto sarEntry = manifest(sarBatch.directory)["completed"].toArray()[0].toObject();
        const auto sarStamp = sarEntry["source_timestamp"].toObject();
        const QImage sarImage(QDir(sarBatch.directory).filePath(sarEntry["file"].toString()));
        expect(sarStamp["coded_size"].toArray() == QJsonArray{144, 96} && sarStamp["sample_aspect_ratio"].toArray() == QJsonArray{4, 3} &&
            sarStamp["display_size"].toArray() == QJsonArray{192, 96} && sarImage.pixelColor(160, 30).alpha() == 0 &&
            sarImage.pixelColor(160, 50).alpha() == 255 && sarImage.pixelColor(160, 210).alpha() == 0,
            "Anamorphic export has incorrect display aspect or provenance");
        const auto stationary = directory.filePath("stationary-target.avi");
        cv::VideoWriter stationaryWriter(stationary.toStdString(), cv::VideoWriter::fourcc('M','J','P','G'), 12., {160, 120});
        expect(stationaryWriter.isOpened(), "Cannot create the failed-foreground lifecycle fixture");
        cv::Mat stationaryScene(120, 160, CV_8UC3, cv::Scalar(35, 55, 65));
        cv::RNG random(6); random.fill(stationaryScene(cv::Rect(62, 48, 24, 20)), cv::RNG::UNIFORM, 90, 230);
        for (int i = 0; i < 13; ++i) stationaryWriter.write(stationaryScene);
        stationaryWriter.release();
        controller.open(QUrl::fromLocalFile(stationary)); expect(waitFor(controller), "Foreground fixture decode timed out");
        controller.track({62./160, 48./120, 24./160, 20./120}, "1.0");
        expect(waitFor(controller) && waitForPreview(controller) && controller.hasResult() && !controller.canExport(), "All-transparent extraction incorrectly enabled export");
        const auto invalidOutput = directory.filePath("empty-uav.png"); exported = false;
        controller.exportFrame(QUrl::fromLocalFile(invalidOutput), {{"width", 160}, {"height", 120}});
        expect(waitFor(controller) && exported && !success && !QFile::exists(invalidOutput), "Controller exported a successful background-only UAV image");
        controller.selectFrame(0, false, false);
        expect(!controller.maskImages().value("source").toUrl().isEmpty(), "Original crop was unavailable for manual mask repair");
        const auto automaticMask = controller.maskImages();
        expect(!automaticMask.value("manuallyEdited").toBool() && automaticMask.value("currentMaskAvailable").toBool() &&
            automaticMask.value("currentAlphaPixels").toLongLong() == 0 && !automaticMask.value("reason").toString().isEmpty(),
            "Failed automatic mask did not expose its empty pixels and original reason separately");
        controller.setSkipInvalid(true);
        expect(waitForPreview(controller), "Skip-invalid preview timed out");
        const auto trackBeforeImmediateExport = controller.trajectorySnapshot();
        const auto beforeRepairPreview = controller.image();
        controller.paintMask({QVariant::fromValue(QPointF(.45, .45)), QVariant::fromValue(QPointF(.55, .55))}, .12, false);
        expect(controller.previewing() && controller.image() == beforeRepairPreview,
            "Immediate-export regression did not begin with an unpublished manual-mask preview");
        bool latestPreviewAtExport = false;
        const auto immediateExportConnection = QObject::connect(&controller, &TrajectoryController::frameExported, &app,
            [&](bool ok, const QString &) {
                latestPreviewAtExport = ok && !controller.previewing() && controller.image() != beforeRepairPreview &&
                    controller.foregroundCounts().value("composed").toInt() == 1;
            });
        const auto immediateRepairPath = directory.filePath("immediate-manual-repair.png"); exported = false;
        const auto immediateRepairSettings = controller.settings();
        controller.exportFrame(QUrl::fromLocalFile(immediateRepairPath), {{"width",160},{"height",120},{"background","transparent"}});
        controller.configure(.75, .3);
        expect(controller.working() && controller.settings() == immediateRepairSettings,
            "An export waiting for its preview allowed the captured edit generation to change");
        expect(waitFor(controller) && exported && success, "Immediate manual-mask export stalled or failed");
        QObject::disconnect(immediateExportConnection);
        expect(latestPreviewAtExport,
            "Manual-mask export completed before the latest preview and composition counts were published");
        expect(controller.trajectorySnapshot() == trackBeforeImmediateExport && controller.settings() == immediateRepairSettings &&
            QImage(immediateRepairPath).size() == QSize(160,120) && controller.frameExportReport().value("composedUavInstances").toInt() == 1,
            "Waiting for a manual-mask preview changed tracking, edits or the exported source-pixel instance");
        {
            const auto beforeSavePreview = controller.image();
            controller.paintMask({QVariant::fromValue(QPointF(.1,.1))},.03,false);
            const auto savePath = directory.filePath("immediate-legacy-repair.png");
            bool latestAtSave = false;
            const auto saveConnection = QObject::connect(&controller,&TrajectoryController::changed,&app,[&] {
                if (!controller.working() && QFile::exists(savePath))
                    latestAtSave = !controller.previewing() && controller.image() != beforeSavePreview;
            });
            controller.save(QUrl::fromLocalFile(savePath));
            expect(waitFor(controller) && QFile::exists(savePath), "Legacy PNG save stalled or failed");
            QObject::disconnect(saveConnection);
            expect(latestAtSave,"Legacy PNG save completed before the latest manual-mask preview was published");
            controller.undo();
            expect(waitForPreview(controller) && controller.settings() == immediateRepairSettings,
                "Legacy preview wait changed the saved edit history");
            const auto beforeBatchPreview = controller.image();
            controller.paintMask({QVariant::fromValue(QPointF(.2,.2))},.03,false);
            bool latestAtBatch = false, batchFinished = false;
            const auto batchConnection = QObject::connect(&controller,&TrajectoryController::batchExported,&app,[&](bool ok,const QString &) {
                batchFinished = ok; latestAtBatch = !controller.previewing() && controller.image() != beforeBatchPreview;
            });
            controller.exportFrames(QUrl::fromLocalFile(directory.path()),{{"content","trajectory"},{"mode","manual"},{"times","0"},
                {"width",160},{"height",120},{"format","png"}});
            expect(waitFor(controller) && batchFinished,"Immediate trajectory batch stalled or failed");
            QObject::disconnect(batchConnection);
            expect(latestAtBatch,"Trajectory batch completed before the latest manual-mask preview was published");
            controller.undo();
            expect(waitForPreview(controller) && controller.settings() == immediateRepairSettings && controller.trajectorySnapshot() == trackBeforeImmediateExport,
                "Batch preview wait changed tracking or the saved edit history");
            for (const bool batch : {false,true}) {
                controller.paintMask({QVariant::fromValue(QPointF(.3,.3))},.03,false);
                const auto cancelledSave = directory.filePath("cancelled-preview-save.png");
                const auto beforeDirectories = QDir(directory.path()).entryList({"trajectory-frames-*"},QDir::Dirs);
                bool batchCancelled = false;
                const auto cancelConnection = QObject::connect(&controller,&TrajectoryController::batchExported,&app,
                    [&](bool ok,const QString &) { batchCancelled = !ok; });
                if (batch) controller.exportFrames(QUrl::fromLocalFile(directory.path()),{{"content","trajectory"},{"mode","manual"},{"times","0"}});
                else controller.save(QUrl::fromLocalFile(cancelledSave));
                expect(controller.working(),"Legacy or batch preview wait was not busy");
                controller.cancel();
                expect(!controller.working() && controller.status().contains("Cancelled") && (!batch || batchCancelled) &&
                    !QFile::exists(cancelledSave) && QDir(directory.path()).entryList({"trajectory-frames-*"},QDir::Dirs) == beforeDirectories,
                    "Cancelled legacy or batch preview wait kept busy, missed its signal or created output");
                QObject::disconnect(cancelConnection);
                expect(waitForPreview(controller),"Preview stalled after legacy or batch cancellation");
                controller.undo();
                expect(waitForPreview(controller) && controller.settings() == immediateRepairSettings,
                    "Cancelled legacy or batch wait changed edit history");
            }
            const auto missingImage = directory.filePath("deleted-preview-element.png");
            QImage elementImage(8,8,QImage::Format_RGB32); elementImage.fill(Qt::green);
            expect(elementImage.save(missingImage),"Cannot create preview-failure image fixture");
            expect(!controller.canvas()->addElement("image",{{"source",missingImage}}).isEmpty() && QFile::remove(missingImage),
                "Cannot prepare the missing preview image fixture");
            const auto failedPreviewOutput = directory.filePath("failed-preview-export.png"); exported = false;
            controller.exportFrame(QUrl::fromLocalFile(failedPreviewOutput),{{"width",160},{"height",120}});
            expect(waitFor(controller) && exported && !success && !QFile::exists(failedPreviewOutput) &&
                controller.status().contains("scientific image"),"A failed latest preview exported stale content or left its request busy");
            exported = false;
            controller.exportFrame(QUrl::fromLocalFile(failedPreviewOutput),{{"width",160},{"height",120}});
            expect(!controller.working() && exported && !success,"An already failed preview left a subsequent export waiting forever");
            controller.undo();
            expect(waitForPreview(controller) && controller.settings() == immediateRepairSettings,
                "Preview-failure recovery lost the preceding manual repair");
        }
        const auto repairedMask = controller.maskImages();
        const auto providedMask = [&](const QVariantMap &mask) { return images.requestImage(mask.value("sprite").toUrl().path().mid(1), nullptr, {}); };
        expect(!repairedMask.isEmpty() && replay::trajectoryAlphaPixels(providedMask(repairedMask)) > 0,
            "Repaired selection did not resolve to actual visible source pixels");
        expect(repairedMask.value("manuallyEdited").toBool() && repairedMask.value("currentMaskAvailable").toBool() &&
            repairedMask.value("currentAlphaPixels").toLongLong() == replay::trajectoryAlphaPixels(providedMask(repairedMask)) &&
            repairedMask.value("reason") == automaticMask.value("reason"),
            "Manual repair reused the automatic empty status or discarded its original failure reason");
        controller.paintMask({QVariant::fromValue(QPointF(.5, .5))}, .5, true);
        const auto cancelledPreviewExport = directory.filePath("cancelled-preview-export.png"); exported = false;
        controller.exportFrame(QUrl::fromLocalFile(cancelledPreviewExport),
            {{"scope","layers"},{"layerIds",QStringList{"background"}},{"width",160},{"height",120}});
        expect(controller.working(), "Pending preview export did not report itself as busy");
        controller.cancel();
        expect(!controller.working() && exported && !success && !QFile::exists(cancelledPreviewExport),
            "Cancelling a queued preview export left it busy or wrote an output file");
        expect(waitForPreview(controller), "Empty manual mask preview timed out");
        const auto erasedMask = controller.maskImages();
        expect(erasedMask.value("manuallyEdited").toBool() && erasedMask.value("currentMaskAvailable").toBool() &&
            erasedMask.value("currentAlphaPixels").toLongLong() == 0 && erasedMask.value("reason") == automaticMask.value("reason"),
            "An erased manual mask was reported as visible or lost the automatic failure reason");
        controller.undo();
        expect(waitForPreview(controller) && controller.maskImages() == repairedMask, "Undo did not restore the repaired mask status");
        controller.resetMask();
        expect(waitForPreview(controller) && controller.maskImages() == automaticMask, "Reset did not restore the automatic mask status");
        controller.undo();
        expect(waitForPreview(controller) && controller.maskImages() == repairedMask, "Undo reset did not restore the manual mask status");
        controller.undo();
        expect(waitForPreview(controller) && controller.maskImages() == automaticMask &&
            replay::trajectoryAlphaPixels(providedMask(controller.maskImages())) == 0,
            "Undo cleared the still-existing selected frame or failed to restore its automatic mask pixels");
        controller.redo();
        expect(waitForPreview(controller) && controller.maskImages() == repairedMask &&
            replay::trajectoryAlphaPixels(providedMask(controller.maskImages())) > 0,
            "Redo lost the selected source time or actual repaired mask pixels");
        controller.configure(.25, .9, 0, 12);
        expect(waitForPreview(controller), "Alternate sampling preview timed out");
        controller.selectFrame(1, false, false);
        expect(std::abs(controller.maskImages().value("seconds").toDouble()-.25) < 1.e-9, "Alternate sample selection used the wrong source time");
        controller.undo();
        expect(waitForPreview(controller) && controller.maskImages().isEmpty(), "Undo kept a selected timestamp that is absent from the restored samples");
        controller.selectFrame(0, false, false);
        controller.setSkipInvalid(true);
        expect(waitForPreview(controller) && controller.canExport() && controller.foregroundCounts().value("composed").toInt() == 1, "Manual mask did not restore an exportable actual source-pixel instance");
        const auto repairedSettings = controller.settings(); const int sampleCount = controller.sampleCount();
        const auto immutableTrack = controller.trajectorySnapshot();
        controller.setLineStyle({{"color","#3d78c6"},{"width",1.25},{"pattern",1},{"opacity",.7}});
        controller.setEndpointStyle(false,{{"enabled",true},{"fill","#00aa55"},{"show_time",true}});
        expect(waitForPreview(controller) && controller.trajectorySnapshot() == immutableTrack, "Changing vector style re-extracted source video");
        const auto rasterSources = [&] {
            QMap<QString, QUrl> result;
            for (const auto &value : controller.canvasLayers()) {
                const auto row = value.toMap();
                if (row.value("kind") == "trajectory-background" || row.value("kind") == "trajectory-sprite") result[row.value("id").toString()] = row.value("source").toUrl();
            }
            return result;
        };
        const auto beforeRasterSources = rasterSources();
        for (const auto &value : controller.canvasLayers()) {
            const auto row = value.toMap(); const auto id = row.value("id").toString();
            const auto source = row.value("source").toUrl();
            expect(!id.isEmpty() && !row.value("sourceId").toString().isEmpty() && source.scheme()=="image" && source.host()=="trajectory",
                "Canvas row lost its native identity or image-provider source URL");
            expect(row.value("width").toDouble()>0. && row.value("height").toDouble()>0. &&
                controller.canvas()->active(id,0.) && controller.canvas()->active(id,controller.lastTime()),
                "Default source-time trajectory layer was hidden at the playhead or had empty geometry");
            const auto provided = images.requestImage(source.path().mid(1),nullptr,{});
            expect(!provided.isNull(),"Native canvas source URL did not resolve to cached image content");
            if (row.value("kind")=="trajectory-sprite") {
                expect(replay::trajectoryAlphaPixels(provided)>0.,"Native canvas sprite provider lost the actual repaired foreground alpha");
                const auto frameIndex=row.value("frameIndex").toInt();
                expect(frameIndex>=0 && size_t(frameIndex)<immutableTrack->frames.size() && provided.size()==immutableTrack->frames[size_t(frameIndex)].bounds.size(),
                    "Native canvas sprite image dimensions did not match its local source bounds");
            }
        }
        {
            PreviewImages restoredImages; TrajectoryController restored(restoredImages);
            auto positioned = controller.settings(); positioned["position"] = .75;
            QEventLoop restoreLoop;
            int publishedMaskThumbnails = 0; bool missingPublishedThumbnail = false;
            const auto publicationConnection = QObject::connect(restored.frames(), &QAbstractItemModel::dataChanged, &restoreLoop,
                [&](const QModelIndex &first, const QModelIndex &last, const QList<int> &roles) {
                    if (!roles.contains(TrajectoryFrameModel::Thumbnail) || first.row() > 0 || last.row() < 0) return;
                    // QML's asynchronous image worker may request the saved
                    // frame-zero mask immediately when its new URL is exposed.
                    const auto url = restored.frames()->data(restored.frames()->index(0, 0), TrajectoryFrameModel::Thumbnail).toUrl();
                    if (url.isEmpty()) return;
                    ++publishedMaskThumbnails;
                    const auto thumbnail = restoredImages.requestImage(url.path().mid(1), nullptr, {});
                    missingPublishedThumbnail |= thumbnail.isNull() || replay::trajectoryAlphaPixels(thumbnail) == 0;
                });
            const auto restoredConnection=QObject::connect(&restored,&TrajectoryController::changed,&restoreLoop,[&]{
                if(restored.hasResult()&&!restored.working()&&!restored.previewing())restoreLoop.quit();
            });
            QTimer::singleShot(30000,&restoreLoop,&QEventLoop::quit);
            restored.restoreSettings(positioned); restoreLoop.exec(); QObject::disconnect(restoredConnection);
            QObject::disconnect(publicationConnection);
            expect(publishedMaskThumbnails > 0 && !missingPublishedThumbnail,
                "Restored frame-zero mask thumbnail was published before its image-provider snapshot was ready");
            expect(restored.hasResult()&&!restored.working()&&!restored.previewing()&&std::abs(restored.position()-.75)<1.e-9&&
                std::abs(restored.settings().value("position").toDouble()-.75)<1.e-9,"Project restore lost the saved canvas playhead during automatic tracking");
        }
        QString spriteId;
        for (const auto &value : controller.canvasLayers()) if (value.toMap().value("kind") == "trajectory-sprite") { spriteId = value.toMap().value("id").toString(); break; }
        expect(!spriteId.isEmpty(), "Tracked UAV instance did not expose an independent canvas row");
        const auto beforeCopy = controller.settings();
        const auto copiedId = controller.canvas()->duplicate(spriteId);
        expect(!copiedId.isEmpty() && waitForPreview(controller) && controller.canvas()->active(copiedId,0.), "Canvas duplicate failed to create an independent instance reference");
        const auto afterCopySources = rasterSources();
        for (auto i = beforeRasterSources.cbegin(); i != beforeRasterSources.cend(); ++i)
            expect(afterCopySources.value(i.key()) == i.value(), "Duplicating a layer changed unrelated image content URLs");
        controller.undo();
        expect(waitForPreview(controller) && controller.settings() == beforeCopy && !controller.canvas()->active(copiedId,0.), "One undo did not remove the duplicate reference and all canvas settings together");
        controller.undo(); controller.undo();
        expect(waitForPreview(controller) && controller.settings() == repairedSettings, "Vector-style undo failed to restore the edit snapshot");
        controller.selectFrame(0,false,false);
        const auto beforeDeleteSources = rasterSources();
        auto *layerModel = controller.canvasLayerModel(); const QPersistentModelIndex backgroundIndex(layerModel->index(0, 0));
        int layerResets = 0; const auto resetConnection = QObject::connect(layerModel, &QAbstractItemModel::modelReset, &app, [&] { ++layerResets; });
        controller.deleteSelectedFrames();
        expect(waitForPreview(controller) && controller.sampleCount() == sampleCount-1 && !controller.canExport(), "Deleted sample remained in the model or export");
        expect(backgroundIndex.isValid() && backgroundIndex.data(TrajectoryCanvasModel::LayerRow).toMap().value("id") == "background" && layerResets == 0 &&
            rasterSources().value("background") == beforeDeleteSources.value("background"),
            "Deleting one UAV recreated the background model identity or changed its source pixels");
        QObject::disconnect(resetConnection);
        controller.addManualFrame(0.);
        expect(waitForPreview(controller) && controller.sampleCount() == sampleCount && controller.canExport(), "Manual re-add did not restore a removed source-time sample");
        controller.undo();
        expect(waitForPreview(controller) && controller.sampleCount() == sampleCount-1 && !controller.canExport(), "Undo manual re-add did not restore sample removal");
        controller.undo();
        expect(waitForPreview(controller) && controller.sampleCount() == sampleCount && controller.settings() == repairedSettings && controller.canExport(), "Undo failed to restore deleted sample and mask state");
        controller.clearGenerated();
        expect(!controller.hasResult() && controller.sampleCount() == 0 && !controller.image().isEmpty() && QFile::exists(stationary), "Clear generated result changed source media or retained generated samples");
        const auto clearedSettings = controller.settings();
        const auto clearedSelection = clearedSettings.value("selection").toList();
        expect(clearedSelection.size() == 4 && clearedSelection[2].toDouble() == 0. && clearedSelection[3].toDouble() == 0., "Clear generated result retained automatic regeneration selection");
        {
            PreviewImages restoredImages; TrajectoryController restored(restoredImages);
            restored.restoreSettings(clearedSettings);
            expect(waitFor(restored) && waitForPreview(restored) && !restored.hasResult() && restored.sampleCount() == 0 && !restored.image().isEmpty(), "Cleared result was regenerated when restoring project settings");
        }
        controller.undo();
        expect(waitForPreview(controller) && controller.hasResult() && controller.canExport() && controller.settings() == repairedSettings, "Undo failed to restore generated result and mask state");
        controller.removeVideo();
        expect(controller.settings().isEmpty() && !controller.hasResult() && controller.sampleCount() == 0 && QFile::exists(stationary), "Remove video retained generated state or deleted media");
        controller.undo();
        expect(waitForPreview(controller) && controller.hasResult() && controller.canExport() && controller.settings() == repairedSettings, "Undo failed to restore video, generated result and edits");
        std::cout << "Trajectory 4K formats, source-time batches, vector figure captions, cutoff/exclusion/opacity, cancellation, naming, sampling and editor preservation passed.\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

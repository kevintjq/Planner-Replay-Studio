#include "cloud_view.h"

#include "camera_presets.h"
#include "camera_follow.h"
#include "transform_tree.h"

#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QQuickOpenGLUtils>
#include <algorithm>
#include <cmath>
#include <map>
#include <string_view>

namespace {
constexpr auto Vertex = R"glsl(
#version 330 core
layout(location = 0) in vec3 point;
uniform mat4 cameraMatrix;
uniform vec3 bodyPosition;
uniform float colorRange;
uniform float pixelSize;
uniform vec3 fixedColor;
uniform bool solid;
uniform bool tfAxes;
out vec3 ink;

void main() {
    gl_Position = cameraMatrix * vec4(point, 1.0);
    gl_PointSize = pixelSize;
    float distanceMix = clamp(distance(point, bodyPosition) / colorRange, 0.0, 1.0);
    vec3 nearColor = vec3(0.27, 1.0, 0.50);
    vec3 middleColor = vec3(0.19, 0.81, 1.0);
    vec3 farColor = vec3(0.68, 0.37, 1.0);
    vec3 rangeColor = distanceMix < 0.48
        ? mix(nearColor, middleColor, distanceMix / 0.48)
        : mix(middleColor, farColor, (distanceMix - 0.48) / 0.52);
    int axis = (gl_VertexID / 2) % 3;
    vec3 axisColor = axis == 0 ? vec3(1.0, 0.28, 0.27)
                   : axis == 1 ? vec3(0.29, 1.0, 0.40) : vec3(0.31, 0.59, 1.0);
    ink = tfAxes ? axisColor : solid ? fixedColor : rangeColor;
}
)glsl";

constexpr auto Fragment = R"glsl(
#version 330 core
in vec3 ink;
out vec4 pixel;
uniform float opacity;
void main() { pixel = vec4(ink * opacity, opacity); }
)glsl";

struct SharedCloudVertices {
    QOpenGLBuffer buffer;
    const replay::Recording *recording = nullptr;
    QPointer<QOpenGLContext> context;
    int uploaded = 0;
};

// All QQuickFramebufferObject renderers in the window use the same render thread.
thread_local std::weak_ptr<SharedCloudVertices> sharedVertices;

using replay::Point;
}

class CloudRenderer : public QQuickFramebufferObject::Renderer {
public:
    void synchronize(QQuickFramebufferObject *item) override
    {
        auto *view = static_cast<CloudView *>(item);
        owner_ = view;
        // The GUI can clear published status when the controller changes. Read
        // that state while the GUI thread is blocked so an identical new target
        // is still published, instead of leaving the view permanently cleared.
        cameraStatus_ = view->cameraStatus_;
        followingTarget_ = view->followingTarget_;
        if (captureSize_ != view->captureSize_) {
            captureSize_ = view->captureSize_;
            invalidateFramebufferObject();
        }
        if (!view->textureLimit_) {
            int limit = 0;
            QOpenGLContext::currentContext()->extraFunctions()->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
            if (limit > 0) { view->textureLimit_ = limit; QMetaObject::invokeMethod(view, &CloudView::textureLimitChanged, Qt::QueuedConnection); }
        }
        pixelScale_ = captureSize_.isValid() ? float(captureSize_.width() / std::max(1., view->width())) : 1.f;
        if (view->controller_) {
            auto recording = view->controller_->recording();
            if (recording != recording_) {
                recording_ = std::move(recording);
                needsStorage_ = true;
            }
            stamp_ = recording_->stampAt(view->controller_->position());
            displayVisibility_ = view->controller_->topicDisplayVisibility();
            independentMode_ = view->controller_->independentDisplayModelActive();
            sceneFixedFrame_ = view->controller_->independentFixedFrame();
            independentStates_ = view->controller_->independentDisplayStates();
            const auto displayRecords = view->controller_->independentDisplayRecordings();
            if (displayRecords != independentRecords_) {
                independentRecords_ = displayRecords;
            }
            plan_ = view->controller_->planningLayerEnabled("plan");
            corridor_ = view->controller_->planningLayerEnabled("corridor");
            goal_ = view->controller_->planningLayerEnabled("goal");
            cameraFrustum_ = view->controller_->planningLayerEnabled("camera");
            currentScan_ = view->controller_->planningLayerEnabled("scan");
            comparePlans_ = view->controller_->comparePlans();
            background_ = view->controller_->sceneBackground();
        } else {
            recording_.reset();
        }
        yaw_ = view->yaw_;
        pitch_ = view->pitch_;
        scale_ = view->scale_ / view->cameraZoom_;
        auxiliary_ = view->auxiliary_;
        scan_ = !auxiliary_ || view->overlays_.contains("scan");
        if (auxiliary_) {
            plan_ = plan_ && view->overlays_.contains("plan");
            corridor_ = corridor_ && view->overlays_.contains("corridor");
            goal_ = goal_ && view->overlays_.contains("goal");
            cameraFrustum_ = cameraFrustum_ && view->overlays_.contains("camera");
        }
        pointSize_ = view->pointSize_;
        panX_ = view->panX_;
        panY_ = view->panY_;
        camera_ = view->camera_;
        followSettings_.camera = camera_;
        followSettings_.yaw = yaw_;
        followSettings_.pitch = pitch_;
        followSettings_.scale = scale_;
        followSettings_.panX = panX_;
        followSettings_.panY = panY_;
        followSettings_.targetFrame = view->targetFrame_;
        followSettings_.followMode = view->followMode_;
        followSettings_.followLag = view->followLag_;
        followSettings_.smoothing = view->smoothing_;
        followSettings_.opticalFrame = view->opticalFrame_;
        cloudVisible_ = view->cloudVisible_;
        cloudColor_ = view->cloudColor_.isValid() ? view->cloudColor_ : QColor::fromRgbF(.75f, .82f, .88f);
        pathColor_ = view->pathColor_.isValid() ? view->pathColor_ : QColor::fromRgbF(1.f, .78f, .35f);
        hiddenDisplays_ = view->hiddenDisplays_;
        if (view->viewBackground_.isValid()) background_ = view->viewBackground_;
        showPath_ = view->showPath_ && (!auxiliary_ || view->overlays_.contains("actual"));
        transparent_ = view->transparent_;
        if (view->captureBackground_.isValid()) {
            background_ = view->captureBackground_;
            // A canvas matte must not turn a transparent inset or video overlay
            // opaque. The canvas/writer supplies the requested backdrop beneath it.
            transparent_ = transparent_ || background_.alpha() == 0;
        }
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        format.setSamples(captureSize_.isValid() ? 4 : 0);
        if (captureSize_.isValid()) qInfo() << "Offscreen cloud FBO" << captureSize_;
        auto *fbo = new QOpenGLFramebufferObject(captureSize_.isValid() ? captureSize_ : size, format);
        const QSize allocated = fbo->isValid() ? fbo->size() : QSize();
        QMetaObject::invokeMethod(owner_, [view = owner_, allocated] {
            if (view) { view->framebufferSize_ = allocated; emit view->framebufferSizeChanged(); }
        }, Qt::QueuedConnection);
        return fbo;
    }

    void render() override
    {
        auto *gl = QOpenGLContext::currentContext()->extraFunctions();
        const auto size = framebufferObject()->size();
        gl->glDisable(GL_SCISSOR_TEST);
        gl->glDisable(GL_BLEND);
        gl->glDepthMask(GL_TRUE);
        gl->glDepthFunc(GL_LESS);
        gl->glViewport(0, 0, size.width(), size.height());
        if (transparent_)
            gl->glClearColor(0.f, 0.f, 0.f, 0.f);
        else
            gl->glClearColor(background_.redF(), background_.greenF(), background_.blueF(), 1.f);
        gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (!initialize() || !recording_) {
            QQuickOpenGLUtils::resetOpenGLState();
            return;
        }

        gl->glEnable(GL_DEPTH_TEST);
        gl->glEnable(GL_PROGRAM_POINT_SIZE);
        program_.bind();
        QOpenGLVertexArrayObject::Binder vertexBinding(&vao_);
        if (needsStorage_)
            allocate();
        const auto &frame = recording_->frameAt(stamp_);
        const auto pose = recording_->poseAt(stamp_);
        const auto body = pose ? pose->position : recording_->center;
        const replay::Recording *cameraRecording = recording_.get();
        if (independentMode_ && sceneFixedFrame_ != recording_->frameId) {
            referenceRecording_.frameId = sceneFixedFrame_;
            referenceRecording_.bodyFrame = recording_->bodyFrame;
            referenceRecording_.center = {};
            referenceRecording_.radius = 10.f;
            referenceRecording_.transforms = recording_->transforms;
            for (const auto &source : independentRecords_) if (source && source->frameId == sceneFixedFrame_) {
                referenceRecording_.center = source->center;
                referenceRecording_.radius = source->radius;
                referenceRecording_.transforms = source->transforms;
                break;
            }
            cameraRecording = &referenceRecording_;
        }
        const auto resolved = replay::cameraFollowFrame(*cameraRecording, stamp_, size, followSettings_);
        if (resolved.status != cameraStatus_ || resolved.following != followingTarget_) {
            cameraStatus_ = resolved.status;
            followingTarget_ = resolved.following;
            QMetaObject::invokeMethod(owner_, [view = owner_, status = cameraStatus_, following = followingTarget_] {
                if (!view) return;
                view->cameraStatus_ = status;
                view->followingTarget_ = following;
                emit view->cameraStatusChanged();
            }, Qt::QueuedConnection);
        }
        const auto &camera = resolved.matrix;
        program_.setUniformValue("cameraMatrix", camera);
        program_.setUniformValue("opacity", 1.f);
        program_.setUniformValue("bodyPosition", body);
        program_.setUniformValue("colorRange", std::max(5.f, recording_->radius * .65f));
        program_.setUniformValue("pixelSize", pointSize_ * pixelScale_);
        program_.setUniformValue("fixedColor", QVector3D(cloudColor_.redF(), cloudColor_.greenF(), cloudColor_.blueF()));
        program_.setUniformValue("solid", true);
        program_.setUniformValue("tfAxes", false);

        if (!independentMode_ && cloud_ && cloud_->buffer.isCreated()) {
            if (recording_->showMainCloud && cloudVisible_) {
                upload(frame.end);
                bind(cloud_->buffer);
                const int from = recording_->replace ? frame.first : 0;
                gl->glDrawArrays(GL_POINTS, from, frame.end - from);
            }
            if (scan_ && currentScan_ && recording_->scanTopic == recording_->cloudTopic &&
                frame.stamp <= stamp_ && stamp_ - frame.stamp < recording_->scanMaximumAge) {
                upload(frame.end);
                bind(cloud_->buffer);
                program_.setUniformValue("solid", true);
                program_.setUniformValue("fixedColor", QVector3D(1.f, .65f, .39f));
                program_.setUniformValue("pixelSize", pointSize_ + 1.f);
                gl->glDepthFunc(GL_LEQUAL);
                gl->glDrawArrays(GL_POINTS, frame.first, frame.end - frame.first);
                gl->glDepthFunc(GL_LESS);
            }
        }
        if (independentMode_) drawIndependentDisplays(gl, camera);
        else drawTopicOverlays(gl, camera);
        program_.setUniformValue("tfAxes", false);
        program_.setUniformValue("cameraMatrix", camera);
        program_.setUniformValue("opacity", 1.f);

        if (!independentMode_ && showPath_ && !recording_->poses.empty()) {
            const auto &poses = recording_->poses;
            const auto through = std::upper_bound(poses.begin(), poses.end(), stamp_,
                [](qint64 time, const replay::Pose &sample) { return time < sample.stamp; });
            program_.setUniformValue("solid", true);
            program_.setUniformValue("fixedColor", QVector3D(pathColor_.redF(), pathColor_.greenF(), pathColor_.blueF()));
            bind(path_);
            gl->glDrawArrays(GL_LINE_STRIP, 0, int(through - poses.begin()));
            if (pose) {
                const Point point{body.x(), body.y(), body.z()};
                marker_.bind();
                marker_.write(0, &point, sizeof(point));
                bind(marker_);
                program_.setUniformValue("pixelSize", 8.f);
                gl->glDrawArrays(GL_POINTS, 0, 1);
            }
        }
        program_.release();
        QQuickOpenGLUtils::resetOpenGLState();
    }

private:
    QColor background_;
    QHash<QString, bool> displayVisibility_;
    bool initialize()
    {
        if (!initialized_) {
            initialized_ = true;
            auto *gl = QOpenGLContext::currentContext()->functions();
            GLfloat lineRange[2]{1.f, 1.f};
            gl->glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, lineRange);
            lineWidthLimit_ = std::max(1.f, lineRange[1]);
            qInfo() << "UAV Studio OpenGL" << reinterpret_cast<const char *>(gl->glGetString(GL_VENDOR))
                    << reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER))
                    << reinterpret_cast<const char *>(gl->glGetString(GL_VERSION));
            ready_ = program_.addShaderFromSourceCode(QOpenGLShader::Vertex, Vertex)
                && program_.addShaderFromSourceCode(QOpenGLShader::Fragment, Fragment)
                && program_.link() && vao_.create() && path_.create() && marker_.create();
            if (!ready_)
                qWarning() << "OpenGL 3.3 required:" << program_.log();
        }
        return ready_;
    }

    void bind(QOpenGLBuffer &buffer)
    {
        buffer.bind();
        program_.enableAttributeArray(0);
        program_.setAttributeBuffer(0, GL_FLOAT, 0, 3, sizeof(Point));
    }

    struct OverlayBuffer {
        QOpenGLBuffer buffer;
        const replay::TopicOverlay::Sample *sample = nullptr;
        bool uploaded = false;
    };

    bool instanceVisible(const replay::TopicDisplay &display) const
    {
        if (!display.enabled || hiddenDisplays_.contains(display.id) || display.alpha <= 0) return false;
        QString parent = display.parentId;
        for (size_t depth = 0; !parent.isEmpty() && depth <= independentStates_.size(); ++depth) {
            const auto found = std::find_if(independentStates_.begin(), independentStates_.end(),
                [&](const auto &row) { return row.id == parent; });
            if (found == independentStates_.end()) return false;
            if (!found->enabled || hiddenDisplays_.contains(found->id)) return false;
            parent = found->parentId;
        }
        return parent.isEmpty();
    }

    void drawReference(QOpenGLExtraFunctions *gl, const QMatrix4x4 &camera, const replay::TopicDisplay &style)
    {
        std::vector<Point> points;
        if (style.kind == "grid") {
            const auto cells = std::clamp(style.properties.value("Plane Cell Count").toInt(20), 1, 1000);
            const auto cell = std::clamp(style.properties.value("Cell Size").toDouble(1), .001, 10000.);
            const float extent = float(cells * cell / 2.);
            for (int i = 0; i <= cells; ++i) {
                const auto p = float(-extent + i * cell);
                points.insert(points.end(), {{p,-extent,0}, {p,extent,0}, {-extent,p,0}, {extent,p,0}});
            }
        } else {
            const float length = float(std::clamp(style.properties.value("Length").toDouble(1), .001, 10000.));
            points = {{0,0,0},{length,0,0},{0,0,0},{0,length,0},{0,0,0},{0,0,length}};
        }
        if (!reference_.isCreated()) reference_.create();
        reference_.bind();
        reference_.allocate(points.data(), int(points.size() * sizeof(Point)));
        bind(reference_);
        program_.setUniformValue("cameraMatrix", camera);
        program_.setUniformValue("solid", true);
        program_.setUniformValue("tfAxes", style.kind == "axes");
        program_.setUniformValue("fixedColor", QVector3D(style.color.redF(), style.color.greenF(), style.color.blueF()));
        const float opacity = float(style.alpha * style.color.alphaF());
        program_.setUniformValue("opacity", opacity);
        gl->glEnable(GL_BLEND);
        gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        gl->glLineWidth(float(std::clamp(style.lineWidth * pixelScale_, 1., double(lineWidthLimit_))));
        gl->glDrawArrays(GL_LINES, 0, int(points.size()));
        gl->glLineWidth(1);
        gl->glDisable(GL_BLEND);
    }

    void drawIndependentDisplays(QOpenGLExtraFunctions *gl, const QMatrix4x4 &camera)
    {
        // Keep buffers for unaffected immutable Topic caches when another
        // Display is added/removed. Only orphaned cache entries are released.
        for (auto entry = independentBuffers_.begin(); entry != independentBuffers_.end();) {
            const auto source = std::find_if(independentRecords_.begin(), independentRecords_.end(),
                [&](const auto &record) { return record.get() == entry->first.first; });
            bool keep = false;
            if (source != independentRecords_.end() && entry->first.second < (*source)->topicOverlays.size()) {
                const auto &overlay = (*source)->topicOverlays[entry->first.second];
                keep = std::any_of(independentStates_.begin(), independentStates_.end(), [&](const auto &style) {
                    return overlay.display.kind == style.kind && (overlay.display.topic == style.topic
                        || (style.kind == "tf" && overlay.display.topic == "/tf_static"));
                });
            }
            if (keep) ++entry; else entry = independentBuffers_.erase(entry);
        }
        for (const auto &style : independentStates_) {
            if (!instanceVisible(style) || (auxiliary_ && style.mainOnly)) continue;
            if (style.kind == "grid" || style.kind == "axes") {
                drawReference(gl, camera, style);
                continue;
            }
            for (const auto &source : independentRecords_) {
                // All lazy caches use the scene's fixed frame. Never mix spaces
                // when a source has no valid transform into that frame.
                if (!source || source->frameId != sceneFixedFrame_) continue;
                for (size_t i = 0; i < source->topicOverlays.size(); ++i) {
                    const auto &overlay = source->topicOverlays[i];
                    if (overlay.display.kind != style.kind || (overlay.display.topic != style.topic
                        && !(style.kind == "tf" && overlay.display.topic == "/tf_static"))) continue;
                    const auto *latest = overlay.sampleAt(stamp_);
                    if (!latest) continue;
                    auto &gpu = independentBuffers_[{source.get(), i}];
                    if (style.historySeconds > 0 && (style.kind == "cloud" || style.kind == "laser" || style.kind == "position")) {
                        const qint64 first = stamp_ - qint64(style.historySeconds * 1.e9);
                        for (const auto &sample : overlay.samples) {
                            if (sample.range.stamp < first) continue;
                            if (sample.range.stamp > stamp_) break;
                            if (sample.expires && stamp_ >= sample.expires) continue;
                            drawTopicSample(gl, camera, overlay, &sample, gpu, false, source.get(), &style);
                        }
                    } else drawTopicSample(gl, camera, overlay, latest, gpu, false, source.get(), &style);
                }
            }
        }
    }

    void drawTopicOverlays(QOpenGLExtraFunctions *gl, const QMatrix4x4 &camera)
    {
        for (size_t i = 0; i < recording_->topicOverlays.size(); ++i) {
            const auto &overlay = recording_->topicOverlays[i];
            if (hiddenDisplays_.contains(overlay.display.id)
                || !displayVisibility_.value(overlay.display.id, overlay.display.enabled))
                continue;
            const auto &role = overlay.display.plannerRole;
            if ((auxiliary_ && overlay.display.mainOnly) || (overlay.display.kind == "laser" && !scan_) ||
                (role == "scan" && (!scan_ || !currentScan_)))
                continue;
            if ((role == "plan" && !plan_) || (role == "corridor" && !corridor_) || (role == "goal" && !goal_) ||
                (role == "camera" && !cameraFrustum_))
                continue;
            const auto *sample = overlay.sampleAt(stamp_);
            if (!sample || sample->range.first == sample->range.end)
                continue;
            if (role == "plan" && comparePlans_ && sample != overlay.samples.data()) {
                const auto *previous = sample - 1;
                while (previous != overlay.samples.data() && previous->range.first == previous->range.end)
                    --previous;
                if (previous->range.first != previous->range.end)
                    drawTopicSample(gl, camera, overlay, previous, comparisons_[i], true);
            }
            drawTopicSample(gl, camera, overlay, sample, overlays_[i], false);
        }
    }

    void drawTopicSample(QOpenGLExtraFunctions *gl, const QMatrix4x4 &camera, const replay::TopicOverlay &overlay,
                         const replay::TopicOverlay::Sample *sample, OverlayBuffer &gpu, bool comparison,
                         const replay::Recording *source = nullptr, const replay::TopicDisplay *style = nullptr)
    {
        if (!source) source = recording_.get();
        auto transform = camera;
        if (sample->frameLocked && sample->frameId != source->frameId) {
            if (!source->transforms)
                return;
            const auto pose = source->transforms->lookup(source->frameId, sample->frameId, stamp_);
            if (!pose)
                return;
            transform.translate(pose->translation);
            transform.rotate(pose->rotation);
        }
        program_.setUniformValue("cameraMatrix", transform);
        auto &buffer = gpu.buffer;
        if (!buffer.isCreated()) {
            if (!buffer.create())
                return;
            buffer.setUsagePattern(QOpenGLBuffer::DynamicDraw);
        }
        bind(buffer);
        const int count = sample->range.end - sample->range.first;
        int first = 0;
        if (style && !gpu.uploaded) {
            buffer.allocate(overlay.points.data(), int(overlay.points.size() * sizeof(Point)));
            gpu.uploaded = true;
        } else if (!style && gpu.sample != sample) {
            buffer.allocate(overlay.points.data() + sample->range.first, count * int(sizeof(Point)));
            gpu.sample = sample;
        }
        if (style) first = sample->range.first;
        const auto color = comparison ? QColor("#c5aaed") :
            (sample->color.isValid() && (!style || style->useMessageColor) ? sample->color : style ? style->color : overlay.display.color);
        const float opacity = comparison ? .35f : float(color.alphaF() * (style ? style->alpha : 1.));
        program_.setUniformValue("solid", true);
        program_.setUniformValue("tfAxes", overlay.display.kind == "tf");
        program_.setUniformValue("fixedColor", QVector3D(color.redF(), color.greenF(), color.blueF()));
        program_.setUniformValue("pixelSize", style ? float(style->pointSize) * pixelScale_ : sample->size > 0.f ? sample->size
                                 : overlay.display.plannerRole == "goal" ? 12.f : overlay.display.kind == "position" ? 8.f : pointSize_);
        program_.setUniformValue("opacity", opacity);
        GLenum primitive = overlay.display.kind == "path" ? GL_LINE_STRIP
            : overlay.display.kind == "tf" ? GL_LINES : GL_POINTS;
        switch (sample->primitive) {
        case replay::TopicOverlay::Primitive::Default: break;
        case replay::TopicOverlay::Primitive::Points: primitive = GL_POINTS; break;
        case replay::TopicOverlay::Primitive::Lines: primitive = GL_LINES; break;
        case replay::TopicOverlay::Primitive::LineStrip: primitive = GL_LINE_STRIP; break;
        case replay::TopicOverlay::Primitive::Triangles: primitive = GL_TRIANGLES; break;
        }
        if (opacity > 0.f) {
            gl->glLineWidth(style ? float(std::clamp(style->lineWidth * pixelScale_, 1., double(lineWidthLimit_))) : 1.f);
            const bool blend = opacity < 1.f;
            if (blend) {
                gl->glEnable(GL_BLEND);
                gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                gl->glDepthMask(GL_FALSE);
            }
            gl->glDrawArrays(primitive, first, count);
            gl->glLineWidth(1.f);
            if (blend) {
                gl->glDepthMask(GL_TRUE);
                gl->glDisable(GL_BLEND);
            }
        }
    }

    void allocate()
    {
        overlays_.clear();
        overlays_.resize(recording_->topicOverlays.size());
        comparisons_.clear();
        comparisons_.resize(recording_->topicOverlays.size());
        const auto *context = QOpenGLContext::currentContext();
        cloud_ = sharedVertices.lock();
        if (!cloud_ || cloud_->recording != recording_.get() || cloud_->context != context) {
            cloud_ = std::make_shared<SharedCloudVertices>();
            cloud_->recording = recording_.get();
            cloud_->context = QOpenGLContext::currentContext();
            if (cloud_->buffer.create()) {
                cloud_->buffer.bind();
                cloud_->buffer.setUsagePattern(QOpenGLBuffer::DynamicDraw);
                const bool useCloud = recording_->showMainCloud ||
                    (!recording_->scanTopic.isEmpty() && recording_->scanTopic == recording_->cloudTopic);
                cloud_->buffer.allocate(useCloud ? int(recording_->points.size() * sizeof(Point)) : 0);
                sharedVertices = cloud_;
            }
        }

        std::vector<Point> path;
        path.reserve(recording_->poses.size());
        for (const auto &sample : recording_->poses)
            path.push_back({sample.position.x(), sample.position.y(), sample.position.z()});
        path_.bind();
        path_.allocate(path.data(), int(path.size() * sizeof(Point)));
        marker_.bind();
        marker_.allocate(sizeof(Point));
        needsStorage_ = false;
    }

    void upload(int through)
    {
        if (through <= cloud_->uploaded)
            return;
        cloud_->buffer.bind();
        cloud_->buffer.write(cloud_->uploaded * int(sizeof(Point)), recording_->points.data() + cloud_->uploaded,
                             (through - cloud_->uploaded) * int(sizeof(Point)));
        cloud_->uploaded = through;
    }

    std::shared_ptr<const replay::Recording> recording_;
    QOpenGLShaderProgram program_;
    QOpenGLVertexArrayObject vao_;
    std::shared_ptr<SharedCloudVertices> cloud_;
    QOpenGLBuffer path_, marker_;
    std::vector<OverlayBuffer> overlays_, comparisons_;
    std::vector<replay::TopicDisplay> independentStates_;
    std::vector<std::shared_ptr<const replay::Recording>> independentRecords_;
    std::map<std::pair<const replay::Recording *, size_t>, OverlayBuffer> independentBuffers_;
    QOpenGLBuffer reference_;
    bool independentMode_ = false;
    QString sceneFixedFrame_;
    replay::Recording referenceRecording_;
    float lineWidthLimit_ = 1;
    bool initialized_ = false, ready_ = false, needsStorage_ = false, showPath_ = true, transparent_ = false;
    int camera_ = 0;
    bool auxiliary_ = false, scan_ = true, currentScan_ = false;
    bool plan_ = true, corridor_ = true, goal_ = true, cameraFrustum_ = true, comparePlans_ = false;
    qint64 stamp_ = 0;
    float yaw_ = 0, pitch_ = 0, scale_ = 1, pointSize_ = 2, panX_ = 0, panY_ = 0, pixelScale_ = 1;
    QSize captureSize_;
    QPointer<CloudView> owner_;
    replay::CameraFollowSettings followSettings_;
    QString cameraStatus_;
    bool followingTarget_ = false, cloudVisible_ = true;
    QColor cloudColor_, pathColor_;
    QStringList hiddenDisplays_;
};

CloudView::CloudView(QQuickItem *parent) : QQuickFramebufferObject(parent)
{
    setMirrorVertically(true);
}
QQuickFramebufferObject::Renderer *CloudView::createRenderer() const { return new CloudRenderer; }

void CloudView::setController(ReplayController *controller)
{
    if (controller_ == controller)
        return;
    if (controller_)
        disconnect(controller_, nullptr, this, nullptr);
    controller_ = controller;
    followingTarget_ = false;
    cameraStatus_.clear();
    emit cameraStatusChanged();
    if (controller_) {
        connect(controller_, &ReplayController::positionChanged, this, &CloudView::update);
        connect(controller_, &ReplayController::sceneStyleChanged, this, &CloudView::update);
        connect(controller_, &ReplayController::independentDisplaysChanged, this, &CloudView::update);
        currentRecording_ = controller_->recording();
        connect(controller_, &ReplayController::recordingChanged, this, [this] {
            const auto recording = controller_->recording();
            if (recording != currentRecording_.lock()) {
                currentRecording_ = recording;
                update();
            } else {
                update();
            }
        });
    }
    emit controllerChanged();
    update();
}

void CloudView::setCamera(int camera)
{
    if (camera >= 0 && size_t(camera) < replay::CameraPresets.size() && camera_ != camera) {
        camera_ = camera;
        resetCamera();
        emit cameraChanged();
    }
}

void CloudView::setCameraZoom(float zoom)
{
    if (!std::isfinite(zoom)) return;
    zoom = std::clamp(zoom, .5f, 4.f);
    if (cameraZoom_ != zoom) {
        cameraZoom_ = zoom;
        emit cameraZoomChanged();
        update();
    }
}

void CloudView::setAuxiliary(bool auxiliary)
{
    if (auxiliary_ != auxiliary) {
        auxiliary_ = auxiliary;
        emit auxiliaryChanged();
        update();
    }
}

void CloudView::setOverlays(const QStringList &overlays)
{
    if (overlays_ != overlays) {
        overlays_ = overlays;
        emit overlaysChanged();
        update();
    }
}

void CloudView::setPointSize(float size)
{
    if (!std::isfinite(size)) return;
    size = std::clamp(size, .25f, 20.f);
    if (pointSize_ != size) {
        pointSize_ = size;
        emit pointSizeChanged();
        update();
    }
}

void CloudView::setTargetFrame(const QString &frame)
{
    const auto normalized = frame.trimmed();
    if (targetFrame_ == normalized) return;
    targetFrame_ = normalized;
    emit followSettingsChanged();
    update();
}

void CloudView::setFollowMode(int mode)
{
    if (mode < 0 || mode > 3 || followMode_ == mode) return;
    followMode_ = mode;
    emit followSettingsChanged();
    update();
}

void CloudView::setFollowLag(double seconds)
{
    if (!std::isfinite(seconds)) return;
    seconds = std::clamp(seconds, 0., 10.);
    if (followLag_ == seconds) return;
    followLag_ = seconds;
    emit followSettingsChanged();
    update();
}

void CloudView::setSmoothing(double seconds)
{
    if (!std::isfinite(seconds)) return;
    seconds = std::clamp(seconds, 0., 10.);
    if (smoothing_ == seconds) return;
    smoothing_ = seconds;
    emit followSettingsChanged();
    update();
}

void CloudView::setOpticalFrame(bool optical)
{
    if (opticalFrame_ == optical) return;
    opticalFrame_ = optical;
    emit followSettingsChanged();
    update();
}

void CloudView::setCloudVisible(bool visible)
{
    if (cloudVisible_ == visible) return;
    cloudVisible_ = visible;
    emit viewStyleChanged();
    update();
}

void CloudView::setCloudColor(const QColor &color)
{
    if (cloudColor_ == color) return;
    cloudColor_ = color;
    emit viewStyleChanged();
    update();
}

void CloudView::setPathColor(const QColor &color)
{
    if (pathColor_ == color) return;
    pathColor_ = color;
    emit viewStyleChanged();
    update();
}

void CloudView::setViewBackground(const QColor &color)
{
    if (viewBackground_ == color) return;
    viewBackground_ = color;
    emit viewStyleChanged();
    update();
}

void CloudView::setHiddenDisplays(const QStringList &ids)
{
    auto unique = ids;
    unique.removeDuplicates();
    unique.removeAll(QString());
    if (hiddenDisplays_ == unique) return;
    hiddenDisplays_ = unique;
    emit viewStyleChanged();
    update();
}

void CloudView::setCaptureSize(const QSize &size)
{
    if (captureSize_ == size) return;
    captureSize_ = size;
    emit captureSizeChanged();
    update();
}

void CloudView::setCaptureBackground(const QColor &color)
{
    if (captureBackground_ == color) return;
    captureBackground_ = color;
    emit captureBackgroundChanged();
    update();
}

void CloudView::setTransparent(bool transparent)
{
    if (transparent_ != transparent) {
        transparent_ = transparent;
        emit transparentChanged();
        update();
    }
}

void CloudView::setShowPath(bool visible)
{
    if (showPath_ != visible) {
        showPath_ = visible;
        emit showPathChanged();
        update();
    }
}

void CloudView::orbit(float dx, float dy)
{
    if (!std::isfinite(dx) || !std::isfinite(dy)) return;
    yaw_ = std::remainder(yaw_ - .34f * dx, 360.f);
    pitch_ = std::clamp(pitch_ + .28f * dy, -75.f, 86.f);
    update();
}

void CloudView::pan(float dx, float dy)
{
    if (!std::isfinite(dx) || !std::isfinite(dy)) return;
    const auto pixels = std::max(1., height());
    panX_ = std::clamp(panX_ + float(dx / pixels), -100.f, 100.f);
    panY_ = std::clamp(panY_ + float(dy / pixels), -100.f, 100.f);
    update();
}

void CloudView::zoom(float steps)
{
    if (!std::isfinite(steps)) return;
    scale_ = std::clamp(scale_ * std::pow(.88f, steps), .08f, 8.f);
    update();
}

void CloudView::resetCamera()
{
    yaw_ = panX_ = panY_ = 0;
    pitch_ = 0.f;
    scale_ = 1;
    update();
}

QString CloudView::exportValidationError(double experimentTime) const
{
    if (camera_ == 0) return {}; // Map overview intentionally has no followed target.
    if (!controller_ || !std::isfinite(experimentTime))
        return tr("无法解析导出相机。 / Export camera is unavailable.");
    const auto recording = controller_->recording();
    if (!recording) return tr("导出相机没有数据源。 / Export camera has no recording.");
    const replay::Recording *cameraRecording = recording.get();
    replay::Recording reference;
    const auto fixedFrame = controller_->independentFixedFrame();
    if (controller_->independentDisplayModelActive() && fixedFrame != recording->frameId) {
        // Use the same coordinate reference as CloudRenderer. Export must
        // reject a missing TF in the selected scene, not validate the old one.
        reference.frameId = fixedFrame;
        reference.bodyFrame = recording->bodyFrame;
        reference.radius = 10.f;
        reference.transforms = recording->transforms;
        for (const auto &source : controller_->independentDisplayRecordings()) {
            if (!source || source->frameId != fixedFrame) continue;
            reference.center = source->center;
            reference.radius = source->radius;
            reference.transforms = source->transforms;
            break;
        }
        cameraRecording = &reference;
    }
    replay::CameraFollowSettings settings;
    settings.camera = camera_; settings.yaw = yaw_; settings.pitch = pitch_;
    settings.scale = scale_ / cameraZoom_; settings.panX = panX_; settings.panY = panY_;
    settings.targetFrame = targetFrame_; settings.followMode = followMode_;
    settings.followLag = followLag_; settings.smoothing = smoothing_; settings.opticalFrame = opticalFrame_;
    const auto resolved = replay::cameraFollowFrame(*cameraRecording, recording->stampAt(experimentTime),
        QSize(std::max(1, int(width())), std::max(1, int(height()))), settings);
    if (resolved.following) return {};
    return tr("无法导出机位 %1（实验时间 %2 s）：%3 / Camera target unavailable; export stopped.")
        .arg(property("exportViewId").toString()).arg(experimentTime, 0, 'f', 9).arg(resolved.status);
}

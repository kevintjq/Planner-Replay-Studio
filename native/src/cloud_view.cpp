#include "cloud_view.h"

#include "camera_presets.h"
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
        const auto *view = static_cast<CloudView *>(item);
        if (view->controller_) {
            auto recording = view->controller_->recording();
            if (recording != recording_) {
                recording_ = std::move(recording);
                needsStorage_ = true;
            }
            stamp_ = recording_->stampAt(view->controller_->position());
            plan_ = view->controller_->planningLayerEnabled("plan");
            corridor_ = view->controller_->planningLayerEnabled("corridor");
            goal_ = view->controller_->planningLayerEnabled("goal");
            cameraFrustum_ = view->controller_->planningLayerEnabled("camera");
            currentScan_ = view->controller_->planningLayerEnabled("scan");
            comparePlans_ = view->controller_->comparePlans();
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
        showPath_ = view->showPath_ && (!auxiliary_ || view->overlays_.contains("actual"));
        transparent_ = view->transparent_;
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        return new QOpenGLFramebufferObject(size, format);
    }

    void render() override
    {
        auto *gl = QOpenGLContext::currentContext()->extraFunctions();
        const auto size = framebufferObject()->size();
        gl->glViewport(0, 0, size.width(), size.height());
        if (transparent_)
            gl->glClearColor(0.f, 0.f, 0.f, 0.f);
        else
            gl->glClearColor(.030f, .046f, .071f, 1.f);
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
        const auto camera = cameraMatrix(size, pose);
        program_.setUniformValue("cameraMatrix", camera);
        program_.setUniformValue("opacity", 1.f);
        program_.setUniformValue("bodyPosition", body);
        program_.setUniformValue("colorRange", std::max(5.f, recording_->radius * .65f));
        program_.setUniformValue("pixelSize", pointSize_);
        program_.setUniformValue("solid", false);
        program_.setUniformValue("tfAxes", false);

        if (cloud_ && cloud_->buffer.isCreated()) {
            if (recording_->showMainCloud) {
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
        drawTopicOverlays(gl, camera);
        program_.setUniformValue("tfAxes", false);
        program_.setUniformValue("cameraMatrix", camera);
        program_.setUniformValue("opacity", 1.f);

        if (showPath_ && !recording_->poses.empty()) {
            const auto &poses = recording_->poses;
            const auto through = std::upper_bound(poses.begin(), poses.end(), stamp_,
                [](qint64 time, const replay::Pose &sample) { return time < sample.stamp; });
            program_.setUniformValue("solid", true);
            program_.setUniformValue("fixedColor", QVector3D(1.f, .78f, .35f));
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
    bool initialize()
    {
        if (!initialized_) {
            initialized_ = true;
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
    };

    void drawTopicOverlays(QOpenGLExtraFunctions *gl, const QMatrix4x4 &camera)
    {
        for (size_t i = 0; i < recording_->topicOverlays.size(); ++i) {
            const auto &overlay = recording_->topicOverlays[i];
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
                         const replay::TopicOverlay::Sample *sample, OverlayBuffer &gpu, bool comparison)
    {
        auto transform = camera;
        if (sample->frameLocked && sample->frameId != recording_->frameId) {
            if (!recording_->transforms)
                return;
            const auto pose = recording_->transforms->lookup(recording_->frameId, sample->frameId, stamp_);
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
        if (gpu.sample != sample) {
            buffer.allocate(overlay.points.data() + sample->range.first, count * int(sizeof(Point)));
            gpu.sample = sample;
        }
        const auto color = comparison ? QColor("#c5aaed") :
            (sample->color.isValid() ? sample->color : overlay.display.color);
        const float opacity = comparison ? .35f : float(color.alphaF());
        program_.setUniformValue("solid", true);
        program_.setUniformValue("tfAxes", overlay.display.kind == "tf");
        program_.setUniformValue("fixedColor", QVector3D(color.redF(), color.greenF(), color.blueF()));
        program_.setUniformValue("pixelSize", sample->size > 0.f ? sample->size
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
            const bool blend = opacity < 1.f;
            if (blend) {
                gl->glEnable(GL_BLEND);
                gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                gl->glDepthMask(GL_FALSE);
            }
            gl->glDrawArrays(primitive, 0, count);
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

    QMatrix4x4 cameraMatrix(const QSize &size, const std::optional<replay::Pose> &pose) const
    {
        constexpr float Radians = .01745329252f;
        const auto &preset = replay::CameraPresets[size_t(camera_)];
        const auto mode = pose ? preset.mode : replay::CameraPreset::OrbitMap;
        const auto &geometry = mode == replay::CameraPreset::OrbitMap ? replay::CameraPresets.front() : preset;
        const float ratio = float(size.width()) / std::max(1, size.height());
        QMatrix4x4 projection, view;
        if (mode == replay::CameraPreset::FirstPerson || mode == replay::CameraPreset::FirstPersonRaw) {
            auto orientation = pose->rotation;
            if (mode == replay::CameraPreset::FirstPerson) {
                const auto before = recording_->poseAt(stamp_ - 120'000'000);
                const auto after = recording_->poseAt(stamp_ + 120'000'000);
                if (before && after)
                    orientation = QQuaternion::slerp(before->rotation, after->rotation, .5f);
                auto stabilized = orientation.rotatedVector({1, 0, 0});
                const QVector3D tangent(-stabilized.y(), stabilized.x(), 0);
                if (tangent.lengthSquared() > 1e-6f) {
                    const auto up = QVector3D::crossProduct(stabilized, tangent).normalized();
                    orientation = QQuaternion::fromAxes(stabilized.normalized(), tangent.normalized(), up);
                }
            }
            const auto baseForward = orientation.rotatedVector({1, 0, 0});
            auto up = orientation.rotatedVector({0, 0, 1});
            auto forward = QQuaternion::fromAxisAndAngle(up, yaw_).rotatedVector(baseForward);
            const auto side = QVector3D::crossProduct(up, forward);
            const auto tilt = QQuaternion::fromAxisAndAngle(side, -pitch_);
            forward = tilt.rotatedVector(forward);
            up = tilt.rotatedVector(up);
            const auto eye = pose->position + .12f * baseForward + .4f * (-side * panX_ + up * panY_);
            projection.perspective(std::clamp(65.f * scale_, 12.f, 120.f), ratio, .035f,
                                   std::max(70.f, recording_->radius * 8.f));
            view.lookAt(eye, eye + forward, up);
            return projection * view;
        }

        float yaw = (yaw_ + geometry.yaw) * Radians;
        const float pitch = std::clamp(geometry.pitch + pitch_, -75.f, 86.f) * Radians;
        auto center = recording_->center;
        float distance = recording_->radius * 2.65f * scale_;
        if (mode != replay::CameraPreset::OrbitMap) {
            const auto forward = pose->rotation.rotatedVector({1, 0, 0});
            yaw += std::atan2(forward.y(), forward.x());
            if (mode == replay::CameraPreset::OrbitAircraft) {
                constexpr double Circle = 6.283185307179586;
                const double elapsed = double(stamp_ - recording_->frames.front().stamp) / 1.e9;
                yaw += float(std::fmod(elapsed, 9.) * Circle / 9.);
            }
            center = pose->position + forward * (geometry.id == std::string_view("rear") ? 3.f : 0.f);
            distance = std::clamp(recording_->radius * .5f, 6.f, 20.f) * scale_;
        }
        const QVector3D right(-std::sin(yaw), std::cos(yaw), 0);
        center += distance * (-right * panX_ + QVector3D(0,0,1) * panY_);
        const QVector3D direction(std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch));
        projection.perspective(46.f, ratio,
                               std::max(.01f, distance * .002f),
                               std::max(50.f, distance + recording_->radius * 5.f));
        view.lookAt(center + direction * distance, center, {0,0,1});
        return projection * view;
    }

    std::shared_ptr<const replay::Recording> recording_;
    QOpenGLShaderProgram program_;
    QOpenGLVertexArrayObject vao_;
    std::shared_ptr<SharedCloudVertices> cloud_;
    QOpenGLBuffer path_, marker_;
    std::vector<OverlayBuffer> overlays_, comparisons_;
    bool initialized_ = false, ready_ = false, needsStorage_ = false, showPath_ = true, transparent_ = false;
    int camera_ = 0;
    bool auxiliary_ = false, scan_ = true, currentScan_ = false;
    bool plan_ = true, corridor_ = true, goal_ = true, cameraFrustum_ = true, comparePlans_ = false;
    qint64 stamp_ = 0;
    float yaw_ = 0, pitch_ = 0, scale_ = 1, pointSize_ = 2, panX_ = 0, panY_ = 0;
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
    if (controller_) {
        connect(controller_, &ReplayController::positionChanged, this, &CloudView::update);
        currentRecording_ = controller_->recording();
        connect(controller_, &ReplayController::recordingChanged, this, [this] {
            const auto recording = controller_->recording();
            if (recording != currentRecording_.lock()) {
                currentRecording_ = recording;
                resetCamera();
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
    if (pointSize_ != size) {
        pointSize_ = size;
        emit pointSizeChanged();
        update();
    }
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
    yaw_ = std::remainder(yaw_ - .34f * dx, 360.f);
    pitch_ = std::clamp(pitch_ + .28f * dy, -75.f, 86.f);
    update();
}

void CloudView::pan(float dx, float dy)
{
    const auto pixels = std::max(1., height());
    panX_ += float(dx / pixels);
    panY_ += float(dy / pixels);
    update();
}

void CloudView::zoom(float steps)
{
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

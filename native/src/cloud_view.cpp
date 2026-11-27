#include "cloud_view.h"

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
    ink = solid ? fixedColor : rangeColor;
}
)glsl";

constexpr auto Fragment = R"glsl(
#version 330 core
in vec3 ink;
out vec4 pixel;
void main() { pixel = vec4(ink, 1.0); }
)glsl";

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
        }
        yaw_ = view->yaw_;
        pitch_ = view->pitch_;
        scale_ = view->scale_;
        pointSize_ = view->pointSize_;
        panX_ = view->panX_;
        panY_ = view->panY_;
        camera_ = view->camera_;
        showPath_ = view->showPath_;
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
        gl->glClearColor(.030f, .046f, .071f, 1);
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
        program_.setUniformValue("cameraMatrix", cameraMatrix(size, pose));
        program_.setUniformValue("bodyPosition", body);
        program_.setUniformValue("colorRange", std::max(5.f, recording_->radius * .65f));
        program_.setUniformValue("pixelSize", pointSize_);
        program_.setUniformValue("solid", false);

        if (recording_->showMainCloud) {
            upload(frame.end);
            bind(cloud_);
            const int from = recording_->replace ? frame.first : 0;
            gl->glDrawArrays(GL_POINTS, from, frame.end - from);
        }
        drawTopicOverlays(gl);

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
                && program_.link() && vao_.create() && cloud_.create() && path_.create() && marker_.create();
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

    void drawTopicOverlays(QOpenGLExtraFunctions *gl)
    {
        for (size_t i = 0; i < recording_->topicOverlays.size(); ++i) {
            const auto &overlay = recording_->topicOverlays[i];
            const auto *sample = overlay.sampleAt(stamp_);
            if (!sample || sample->range.first == sample->range.end)
                continue;
            auto &gpu = overlays_[i];
            auto &buffer = gpu.buffer;
            if (!buffer.isCreated()) {
                if (!buffer.create())
                    continue;
                buffer.setUsagePattern(QOpenGLBuffer::DynamicDraw);
            }
            bind(buffer);
            const int count = sample->range.end - sample->range.first;
            if (gpu.sample != sample) {
                buffer.allocate(overlay.points.data() + sample->range.first, count * int(sizeof(Point)));
                gpu.sample = sample;
            }
            const auto &color = overlay.display.color;
            program_.setUniformValue("solid", true);
            program_.setUniformValue("fixedColor", QVector3D(color.redF(), color.greenF(), color.blueF()));
            program_.setUniformValue("pixelSize", overlay.display.kind == "position" ? 8.f : pointSize_);
            gl->glDrawArrays(overlay.display.kind == "path" ? GL_LINE_STRIP : GL_POINTS, 0, count);
        }
    }

    void allocate()
    {
        overlays_.clear();
        overlays_.resize(recording_->topicOverlays.size());
        cloud_.bind();
        cloud_.setUsagePattern(QOpenGLBuffer::DynamicDraw);
        cloud_.allocate(recording_->showMainCloud ? int(recording_->points.size() * sizeof(Point)) : 0);
        uploaded_ = 0;

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
        if (through <= uploaded_)
            return;
        cloud_.bind();
        cloud_.write(uploaded_ * int(sizeof(Point)), recording_->points.data() + uploaded_,
                     (through - uploaded_) * int(sizeof(Point)));
        uploaded_ = through;
    }

    QMatrix4x4 cameraMatrix(const QSize &size, const std::optional<replay::Pose> &pose) const
    {
        constexpr float Radians = .01745329252f;
        float yaw = (yaw_ + (camera_ == 0 ? -115.f : 180.f)) * Radians;
        const float pitch = pitch_ * Radians;
        auto center = recording_->center;
        float distance = recording_->radius * 2.65f * scale_;
        if (camera_ == 1 && pose) {
            const auto forward = pose->rotation.rotatedVector({1, 0, 0});
            yaw += std::atan2(forward.y(), forward.x());
            center = pose->position + forward * 3.f;
            distance = std::clamp(recording_->radius * .5f, 6.f, 20.f) * scale_;
        }
        const QVector3D right(-std::sin(yaw), std::cos(yaw), 0);
        center += distance * (-right * panX_ + QVector3D(0,0,1) * panY_);
        const QVector3D direction(std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch));
        QMatrix4x4 projection, view;
        projection.perspective(46.f, float(size.width()) / std::max(1, size.height()),
                               std::max(.01f, distance * .002f),
                               std::max(50.f, distance + recording_->radius * 5.f));
        view.lookAt(center + direction * distance, center, {0,0,1});
        return projection * view;
    }

    std::shared_ptr<const replay::Recording> recording_;
    QOpenGLShaderProgram program_;
    QOpenGLVertexArrayObject vao_;
    QOpenGLBuffer cloud_, path_, marker_;
    std::vector<OverlayBuffer> overlays_;
    bool initialized_ = false, ready_ = false, needsStorage_ = false, showPath_ = true;
    int uploaded_ = 0, camera_ = 0;
    qint64 stamp_ = 0;
    float yaw_ = 0, pitch_ = 32, scale_ = 1, pointSize_ = 2, panX_ = 0, panY_ = 0;
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
        connect(controller_, &ReplayController::recordingChanged, this, &CloudView::resetCamera);
    }
    emit controllerChanged();
    update();
}

void CloudView::setCamera(int camera)
{
    if (camera_ != camera) {
        camera_ = camera;
        resetCamera();
        emit cameraChanged();
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
    pitch_ = camera_ == 1 ? 18.f : 32.f;
    scale_ = 1;
    update();
}

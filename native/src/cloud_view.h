#pragma once

#include "replay_controller.h"

#include <QQuickFramebufferObject>
#include <QPointer>

class CloudView : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(ReplayController *controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(int camera READ camera WRITE setCamera NOTIFY cameraChanged)
    Q_PROPERTY(float cameraZoom READ cameraZoom WRITE setCameraZoom NOTIFY cameraZoomChanged)
    Q_PROPERTY(bool auxiliary READ auxiliary WRITE setAuxiliary NOTIFY auxiliaryChanged)
    Q_PROPERTY(QStringList overlays READ overlays WRITE setOverlays NOTIFY overlaysChanged)
    Q_PROPERTY(float pointSize READ pointSize WRITE setPointSize NOTIFY pointSizeChanged)
    Q_PROPERTY(bool transparent READ transparent WRITE setTransparent NOTIFY transparentChanged)
    Q_PROPERTY(bool showPath READ showPath WRITE setShowPath NOTIFY showPathChanged)
    Q_PROPERTY(QSize captureSize READ captureSize WRITE setCaptureSize NOTIFY captureSizeChanged)
    Q_PROPERTY(int textureLimit READ textureLimit NOTIFY textureLimitChanged)
    Q_PROPERTY(QSize framebufferSize READ framebufferSize NOTIFY framebufferSizeChanged)
    Q_PROPERTY(QColor captureBackground READ captureBackground WRITE setCaptureBackground NOTIFY captureBackgroundChanged)
public:
    explicit CloudView(QQuickItem *parent = nullptr);
    Renderer *createRenderer() const override;

    ReplayController *controller() const { return controller_; }
    void setController(ReplayController *controller);
    int camera() const { return camera_; }
    void setCamera(int camera);
    float cameraZoom() const { return cameraZoom_; }
    void setCameraZoom(float zoom);
    bool auxiliary() const { return auxiliary_; }
    void setAuxiliary(bool auxiliary);
    QStringList overlays() const { return overlays_; }
    void setOverlays(const QStringList &overlays);
    float pointSize() const { return pointSize_; }
    void setPointSize(float size);
    bool transparent() const { return transparent_; }
    void setTransparent(bool transparent);
    bool showPath() const { return showPath_; }
    void setShowPath(bool visible);
    QSize captureSize() const { return captureSize_; }
    void setCaptureSize(const QSize &size);
    int textureLimit() const { return textureLimit_; }
    QSize framebufferSize() const { return framebufferSize_; }
    QColor captureBackground() const { return captureBackground_; }
    void setCaptureBackground(const QColor &color);
    Q_INVOKABLE void orbit(float dx, float dy);
    Q_INVOKABLE void pan(float dx, float dy);
    Q_INVOKABLE void zoom(float steps);
    Q_INVOKABLE void resetCamera();

signals:
    void controllerChanged();
    void cameraChanged();
    void cameraZoomChanged();
    void auxiliaryChanged();
    void overlaysChanged();
    void pointSizeChanged();
    void showPathChanged();
    void transparentChanged();
    void captureSizeChanged();
    void textureLimitChanged();
    void framebufferSizeChanged();
    void captureBackgroundChanged();

private:
    friend class CloudRenderer;
    QPointer<ReplayController> controller_;
    std::weak_ptr<const replay::Recording> currentRecording_;
    float yaw_ = 0, pitch_ = 0, scale_ = 1, pointSize_ = 1.5f;
    float panX_ = 0, panY_ = 0;
    int camera_ = 0;
    bool showPath_ = true, transparent_ = false, auxiliary_ = false;
    float cameraZoom_ = 1.f;
    QStringList overlays_;
    QSize captureSize_;
    QSize framebufferSize_;
    int textureLimit_ = 0;
    QColor captureBackground_;
};

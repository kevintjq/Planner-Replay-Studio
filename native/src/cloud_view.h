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
    Q_PROPERTY(QString targetFrame READ targetFrame WRITE setTargetFrame NOTIFY followSettingsChanged)
    Q_PROPERTY(int followMode READ followMode WRITE setFollowMode NOTIFY followSettingsChanged)
    Q_PROPERTY(double followLag READ followLag WRITE setFollowLag NOTIFY followSettingsChanged)
    Q_PROPERTY(double smoothing READ smoothing WRITE setSmoothing NOTIFY followSettingsChanged)
    Q_PROPERTY(bool opticalFrame READ opticalFrame WRITE setOpticalFrame NOTIFY followSettingsChanged)
    Q_PROPERTY(QString cameraStatus READ cameraStatus NOTIFY cameraStatusChanged)
    Q_PROPERTY(bool followingTarget READ followingTarget NOTIFY cameraStatusChanged)
    Q_PROPERTY(bool cloudVisible READ cloudVisible WRITE setCloudVisible NOTIFY viewStyleChanged)
    Q_PROPERTY(QColor cloudColor READ cloudColor WRITE setCloudColor NOTIFY viewStyleChanged)
    Q_PROPERTY(QColor pathColor READ pathColor WRITE setPathColor NOTIFY viewStyleChanged)
    Q_PROPERTY(QColor viewBackground READ viewBackground WRITE setViewBackground NOTIFY viewStyleChanged)
    Q_PROPERTY(QStringList hiddenDisplays READ hiddenDisplays WRITE setHiddenDisplays NOTIFY viewStyleChanged)
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
    QString targetFrame() const { return targetFrame_; }
    void setTargetFrame(const QString &frame);
    int followMode() const { return followMode_; }
    void setFollowMode(int mode);
    double followLag() const { return followLag_; }
    void setFollowLag(double seconds);
    double smoothing() const { return smoothing_; }
    void setSmoothing(double seconds);
    bool opticalFrame() const { return opticalFrame_; }
    void setOpticalFrame(bool optical);
    QString cameraStatus() const { return cameraStatus_; }
    bool followingTarget() const { return followingTarget_; }
    bool cloudVisible() const { return cloudVisible_; }
    void setCloudVisible(bool visible);
    QColor cloudColor() const { return cloudColor_; }
    void setCloudColor(const QColor &color);
    QColor pathColor() const { return pathColor_; }
    void setPathColor(const QColor &color);
    QColor viewBackground() const { return viewBackground_; }
    void setViewBackground(const QColor &color);
    QStringList hiddenDisplays() const { return hiddenDisplays_; }
    void setHiddenDisplays(const QStringList &ids);
    Q_INVOKABLE void orbit(float dx, float dy);
    Q_INVOKABLE void pan(float dx, float dy);
    Q_INVOKABLE void zoom(float steps);
    Q_INVOKABLE void resetCamera();
    // Recompute from source data and exact experiment time, not queued render diagnostics.
    QString exportValidationError(double experimentTime) const;

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
    void followSettingsChanged();
    void cameraStatusChanged();
    void viewStyleChanged();

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
    QString targetFrame_, cameraStatus_;
    int followMode_ = 0;
    double followLag_ = 0, smoothing_ = 0;
    bool opticalFrame_ = false, followingTarget_ = false, cloudVisible_ = true;
    QColor cloudColor_, pathColor_, viewBackground_;
    QStringList hiddenDisplays_;
};

#pragma once

#include "replay_controller.h"

#include <QQuickFramebufferObject>
#include <QPointer>

class CloudView : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(ReplayController *controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(int camera READ camera WRITE setCamera NOTIFY cameraChanged)
    Q_PROPERTY(float pointSize READ pointSize WRITE setPointSize NOTIFY pointSizeChanged)
    Q_PROPERTY(bool transparent READ transparent WRITE setTransparent NOTIFY transparentChanged)
    Q_PROPERTY(bool showPath READ showPath WRITE setShowPath NOTIFY showPathChanged)
public:
    explicit CloudView(QQuickItem *parent = nullptr);
    Renderer *createRenderer() const override;

    ReplayController *controller() const { return controller_; }
    void setController(ReplayController *controller);
    int camera() const { return camera_; }
    void setCamera(int camera);
    float pointSize() const { return pointSize_; }
    void setPointSize(float size);
    bool transparent() const { return transparent_; }
    void setTransparent(bool transparent);
    bool showPath() const { return showPath_; }
    void setShowPath(bool visible);
    Q_INVOKABLE void orbit(float dx, float dy);
    Q_INVOKABLE void pan(float dx, float dy);
    Q_INVOKABLE void zoom(float steps);
    Q_INVOKABLE void resetCamera();

signals:
    void controllerChanged();
    void cameraChanged();
    void pointSizeChanged();
    void showPathChanged();
    void transparentChanged();

private:
    friend class CloudRenderer;
    QPointer<ReplayController> controller_;
    float yaw_ = 0, pitch_ = 32, scale_ = 1, pointSize_ = 1.5f;
    float panX_ = 0, panY_ = 0;
    int camera_ = 0;
    bool showPath_ = true, transparent_ = false;
};

#pragma once
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>

namespace rviz { class VisualizationManager; }

// Indexed, full-history replay. This never substitutes record time for Header time.
class NativeBagReplay : public QObject {
public:
    using Reply = std::function<void(bool, const QJsonObject &, const QString &)>;
    explicit NativeBagReplay(rviz::VisualizationManager *manager,
                             std::function<void(QJsonObject)> event, QObject *parent = nullptr);
    ~NativeBagReplay() override;
    bool busy() const;
    bool indexed() const;
    bool synchronized() const;
    QString targetNs() const;
    // Shared message history is independent of the current camera's TF target.
    bool dataSynchronized() const;
    QString dataTargetNs() const;
    QJsonObject readiness(bool includeCurrentView = true) const;
    QJsonObject resources() const;
    void index(const QJsonObject &request, Reply reply);
    void seek(const QJsonObject &request, Reply reply);
    void advance(const QJsonObject &request, Reply reply);
    void cancel(const QString &reason);
    void invalidate();
private:
    class Private;
    std::unique_ptr<Private> d;
};

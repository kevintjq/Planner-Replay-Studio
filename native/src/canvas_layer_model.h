#pragma once
#include <QObject>
#include <QRectF>
#include <QStringList>
#include <QVariantList>
#include <functional>
#include "canvas_layer_state.h"

class ReplayController;

// 所有画布元素只保存项目坐标；拖动仅修改此模型，不重载数据或重建场景。
class CanvasLayerModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList layers READ layers NOTIFY catalogChanged)
    Q_PROPERTY(QStringList selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedId READ selectedId NOTIFY selectionChanged)
    Q_PROPERTY(bool layoutMode READ layoutMode WRITE setLayoutMode NOTIFY modeChanged)
    Q_PROPERTY(bool snapping READ snapping WRITE setSnapping NOTIFY modeChanged)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(int outputWidth READ outputWidth NOTIFY changed)
    Q_PROPERTY(int outputHeight READ outputHeight NOTIFY changed)
public:
    struct Binding {
        std::function<QVariantMap()> getState;
        std::function<void(const QVariantMap &)> storeState;
        std::function<QVariantList()> rawLayers;
        std::function<bool(const QString &)> exists;
        std::function<void(const QStringList &)> remove;
        std::function<QString(const QString &)> duplicate;
        std::function<void()> beginEdit;
        std::function<void(bool cancel)> endEdit;
        std::function<double()> duration;
        std::function<bool()> loading;
    };
    explicit CanvasLayerModel(ReplayController &replay);
    explicit CanvasLayerModel(Binding binding, QObject *parent = nullptr);
    QVariantList layers() const;
    const QStringList &selection() const { return selection_; }
    QString selectedId() const { return selection_.isEmpty() ? QString() : selection_.last(); }
    bool layoutMode() const { return layoutMode_; }
    bool snapping() const { return snapping_; }
    void setLayoutMode(bool value);
    void setSnapping(bool value);
    int revision() const { return revision_; }
    int outputWidth() const;
    int outputHeight() const;
    Q_INVOKABLE void setOutputSize(int width, int height);
    Q_INVOKABLE QVariantMap settings(const QString &id, double time = -1.) const;
    Q_INVOKABLE QRectF geometry(const QString &id, const QRectF &base, const QSizeF &canvas, double time) const;
    Q_INVOKABLE bool active(const QString &id, double time) const;
    Q_INVOKABLE void select(const QString &id, bool additive = false);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void beginEdit();
    Q_INVOKABLE void endEdit(bool cancel = false);
    Q_INVOKABLE void edit(const QString &id, const QVariantMap &changes);
    Q_INVOKABLE void reset(const QString &id);
    Q_INVOKABLE void removeSelected();
    Q_INVOKABLE void removeElements(const QStringList &ids);
    Q_INVOKABLE QString duplicate(const QString &id);
    Q_INVOKABLE QStringList supportedElementKinds() const;
    Q_INVOKABLE QString addElement(const QString &kind, const QVariantMap &properties = {});
    Q_INVOKABLE void editElement(const QString &id, const QVariantMap &changes);
    Q_INVOKABLE QVariantMap element(const QString &id, double time = -1.) const;
    Q_INVOKABLE QVariantList copySelected() const;
    Q_INVOKABLE QStringList pasteElements(const QVariantList &elements);
    Q_INVOKABLE void reorder(const QString &id, int destination);
    Q_INVOKABLE void moveLayer(const QString &id, int direction);
    Q_INVOKABLE void groupSelection();
    Q_INVOKABLE void ungroupSelection();
    Q_INVOKABLE void addKeyframe(const QString &id, double time, const QVariantMap &values);
    Q_INVOKABLE void removeKeyframe(const QString &id, double time);
    Q_INVOKABLE void removeInterval(const QString &id, double begin, double end);
    void refresh(bool catalog = true);
signals:
    void changed();
    void catalogChanged();
    void selectionChanged();
    void modeChanged();
private:
    static Binding replayBinding(ReplayController &replay);
    QVariantMap state() const;
    QVariantMap rawLayer(const QString &id) const;
    bool scientificElement(const QString &id) const;
    bool exists(const QString &id) const;
    void store(const QVariantMap &state, bool catalog = false);
    Binding binding_;
    QStringList selection_;
    int editDepth_ = 0;
    bool editCancelled_ = false;
    bool layoutMode_ = true, snapping_ = true;
    int revision_ = 0;
};

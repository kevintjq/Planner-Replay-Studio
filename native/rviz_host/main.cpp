// RViz/Qt5 独立宿主。Qt6 主界面只交换 JSON，不跨 ABI 传递 QObject 或 OGRE 对象。
#include "bag_replay.h"
#include <ros/ros.h>
#include <ros/master.h>
#include <rosbag/bag.h>
#include <rosbag/view.h>
#include <rviz/visualization_manager.h>
#include <rviz/display_factory.h>
#include <rviz/display_group.h>
#include <rviz/failed_display.h>
#include <rviz/render_panel.h>
#include <rviz/view_manager.h>
#include <rviz/view_controller.h>
#include <rviz/failed_view_controller.h>
#include <rviz/frame_manager.h>
#include <rviz/viewport_mouse_event.h>
#include <rviz/tool_manager.h>
#include <rviz/tool.h>
#include <rviz/properties/property_tree_model.h>
#include <rviz/properties/color_property.h>
#include <rviz/properties/enum_property.h>
#include <rviz/properties/editable_enum_property.h>
#include <rviz/properties/float_property.h>
#include <rviz/properties/int_property.h>
#include <rviz/properties/vector_property.h>
#include <rviz/properties/quaternion_property.h>
#include <rviz/properties/ros_topic_property.h>
#include <rviz/properties/tf_frame_property.h>
#include <rviz/properties/status_property.h>
#include <rviz/yaml_config_reader.h>
#include <rviz/yaml_config_writer.h>
#include <yaml-cpp/yaml.h>
#include <OgreRenderWindow.h>
#include <OgreCamera.h>
#include <OgrePixelFormat.h>
#include <OgreTextureManager.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreRenderTexture.h>
#include <OgreViewport.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <GL/gl.h>
#include <QApplication>
#include <QComboBox>
#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QProcess>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QUuid>
#include <QSet>
#include <QRegularExpression>
#include <cmath>
#include <algorithm>
#include <memory>
#include <limits>
#include <map>
#include <stdexcept>
#include <unistd.h>

namespace {
constexpr int MaxRequestBytes = 4 * 1024 * 1024;
[[noreturn]] void fail(const QString &text) { throw std::runtime_error(text.toStdString()); }
void require(bool test, const QString &text) { if (!test) fail(text); }
QJsonValue jsonValue(const QVariant &value) {
    if (!value.isValid()) return QJsonValue();
    if (value.canConvert<QColor>() && value.userType() == QMetaType::QColor) return value.value<QColor>().name(QColor::HexArgb);
    return QJsonValue::fromVariant(value);
}
rviz::Config mergedConfig(const rviz::Config &original,const rviz::Config &current) {
    rviz::Config result;
    if(original.getType()!=rviz::Config::Map||current.getType()!=rviz::Config::Map){result.copy(current);return result;}
    result.copy(original);
    for(auto it=current.mapIterator();it.isValid();it.advance())result.mapMakeChild(it.currentKey()).copy(mergedConfig(original.mapGetChild(it.currentKey()),it.currentChild()));
    return result;
}
QStringList unknownConfigPaths(const rviz::Config &source,const rviz::Config &native,const QString &prefix={}) {
    QStringList paths;if(source.getType()!=rviz::Config::Map)return paths;
    for(auto it=source.mapIterator();it.isValid();it.advance()) {
        if(it.currentKey()=="UAV Studio"||it.currentKey()=="Displays")continue;
        const auto path=prefix.isEmpty()?it.currentKey():prefix+" / "+it.currentKey();const auto known=native.mapGetChild(it.currentKey());
        if(!known.isValid())paths.append(path);else paths.append(unknownConfigPaths(it.currentChild(),known,path));
    }
    return paths;
}

class Host : public QObject {
public:
    Host(int output, const QString &initial, bool visible) : output_(output), hidden_(!visible) {
        qApp->installEventFilter(this);
        createScene(visible);
        connect(&notifications_, &QTimer::timeout, this, [this] {
            if (dirty_) { dirty_ = false; ++revision_; send({{"event", "changed"}, {"revision", QString::number(revision_)}}); }
        });
        notifications_.start(100);
        connect(&playback_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
            send({{"event", "bag_playback_finished"}, {"exit_code", code}, {"normal_exit", status == QProcess::NormalExit}});
        });
        connect(&playback_, &QProcess::readyReadStandardError, this, [this] { playbackLog_ += playback_.readAllStandardError(); if(playbackLog_.size() > 65536) playbackLog_ = playbackLog_.right(65536); });
        if (!initial.isEmpty()) load(initial);
        else {
            manager_->setFixedFrame("map");
            require(manager_->createDisplay("rviz/Grid", "Grid", true), "Default native Grid failed to initialize.");
            require(manager_->createDisplay("rviz/Axes", "Axes", true), "Default native Axes failed to initialize.");
        }
        send({{"event", "ready"}, {"protocol", 1}, {"backend", "ROS1/Noetic/librviz"}, {"qt", qVersion()},
            {"rendering", "native-rviz-window"}, {"embedded_in_main_window", false}});
        auto *input = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
        connect(input, &QSocketNotifier::activated, this, [this, input] {
            char buffer[8192]; const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count <= 0) { input->setEnabled(false); stopPlayback(); qApp->quit(); return; }
            input_ += QByteArray(buffer, int(count));
            if (input_.size() > MaxRequestBytes) { send({{"ok", false}, {"error", "Request exceeds 4 MiB"}}); input_.clear(); return; }
            for (;;) { const int end = input_.indexOf('\n'); if (end < 0) break; const auto line = input_.left(end); input_.remove(0, end+1); dispatch(line); }
        });
    }
    ~Host() override { stopPlayback(); releaseScene(); }
private:
    bool eventFilter(QObject *object,QEvent *event) override {
        if(event->type()==QEvent::Polish||event->type()==QEvent::Show)
            if(auto *widget=qobject_cast<QWidget *>(object);widget&&widget->isWindow()) {
                widget->setAttribute(Qt::WA_ShowWithoutActivating,true);
                if(hidden_)widget->setAttribute(Qt::WA_DontShowOnScreen,true);
            }
        return QObject::eventFilter(object,event);
    }
    void createScene(bool visible) {
        panel_=std::make_unique<rviz::RenderPanel>();
        panel_->setAttribute(Qt::WA_ShowWithoutActivating,true);
        panel_->setAttribute(Qt::WA_DontShowOnScreen,!visible);
        manager_=std::make_unique<rviz::VisualizationManager>(panel_.get());
        panel_->resize(1280, 720);
        panel_->setWindowTitle("UAV Studio — Native RViz Host (ROS 1 Noetic)");
        panel_->initialize(manager_->getSceneManager(), manager_.get());
        manager_->initialize();
        manager_->startUpdate();
        connect(manager_.get(), &rviz::VisualizationManager::preUpdate, this, [this] {
            // preUpdate precedes FrameManager::update. Coalesce a queued pass
            // after the manager finishes, including explicit indexed updates.
            if (viewsUpdateQueued_ || views_.empty()) return;
            viewsUpdateQueued_ = true;
            QTimer::singleShot(0, this, [this] { viewsUpdateQueued_ = false; updateViews(); });
        });
        replay_=std::make_unique<NativeBagReplay>(manager_.get(),[this](QJsonObject event){send(event);});
        panel_->show();
        if(!visible) panel_->hide();
        for (auto *model : {manager_->getDisplayTreeModel(), manager_->getViewManager()->getPropertyModel(), manager_->getToolManager()->getPropertyModel()}) {
            connect(model, &QAbstractItemModel::dataChanged, this, [this] { dirty_ = true; });
            connect(model, &QAbstractItemModel::rowsInserted, this, [this] { dirty_ = true; });
            connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { dirty_ = true; });
            connect(model, &rviz::PropertyTreeModel::propertyHiddenChanged, this, [this] { dirty_ = true; });
        }
    }
    void releaseScene() {
        releaseOutputTexture();clearViews();replay_.reset();manager_.reset();panel_.reset();
        ids_.clear();properties_.clear();displaySources_.clear();instanceIds_.clear();
    }
    struct ViewSlot {
        std::unique_ptr<rviz::RenderPanel> panel;
        std::unique_ptr<rviz::ViewController> controller;
        rviz::Config originalController;
        int pointerX=0,pointerY=0;
        QElapsedTimer lastPreview;
        ~ViewSlot() {
            // RenderPanel does not own its controller. Both cameras must be
            // destroyed while the shared SceneManager is still alive.
            if(panel)panel->setViewController(nullptr);
            controller.reset();panel.reset();
        }
    };
    static void validViewId(const QString &value) {
        static const QRegularExpression syntax("^[A-Za-z0-9_.:-]{1,100}$");
        require(value!="main"&&syntax.match(value).hasMatch(),"Additional view_id must be a stable ASCII identity other than main (1-100 characters).");
    }
    void clearViews() {views_.clear();viewOrder_.clear();pointerBefore_.clear();viewUpdateWall_.invalidate();viewUpdateRos_=ros::Time();}
    rviz::ViewController *viewController(const QString &viewId) {
        if(viewId=="main")return manager_->getViewManager()->getCurrent();
        const auto slot=views_.find(viewId);require(slot!=views_.end(),"Unknown native view_id; synchronize the native view list first.");
        return slot->second->controller.get();
    }
    rviz::RenderPanel *viewPanel(const QString &viewId) {
        if(viewId=="main")return panel_.get();
        const auto slot=views_.find(viewId);require(slot!=views_.end(),"Unknown native view_id; synchronize the native view list first.");
        return slot->second->panel.get();
    }
    std::unique_ptr<ViewSlot> makeView(const QString &viewId,const QString &plugin,const rviz::Config *config=nullptr) {
        auto slot=std::make_unique<ViewSlot>();
        slot->panel=std::make_unique<rviz::RenderPanel>();
        slot->panel->setAttribute(Qt::WA_ShowWithoutActivating,true);
        slot->panel->setAttribute(Qt::WA_DontShowOnScreen,true);
        slot->panel->resize(960,540);
        slot->panel->initialize(manager_->getSceneManager(),manager_.get());
        auto *factory=manager_->getViewManager();
        slot->controller.reset(factory->create(plugin));
        require(slot->controller&&!dynamic_cast<rviz::FailedViewController *>(slot->controller.get()),"Native view plugin failed to initialize.");
        if(config){slot->originalController.copy(*config);slot->controller->load(*config);}
        else if(auto *current=factory->getCurrent())slot->controller->mimic(current);
        slot->controller->setName(viewId);
        slot->panel->setViewController(slot->controller.get());
        slot->panel->show();slot->panel->hide();
        slot->panel->getRenderWindow()->setAutoUpdated(false);
        connect(slot->controller.get(),&rviz::ViewController::configChanged,this,[this]{dirty_=true;});
        return slot;
    }
    void updateViews() {
        if(!manager_||views_.empty()||(replay_&&replay_->busy()))return;
        const auto now=ros::Time::now();
        const float wall=viewUpdateWall_.isValid()?float(viewUpdateWall_.restart())/1000.f:0.f;
        if(!viewUpdateWall_.isValid())viewUpdateWall_.start();
        const float rosDelta=viewUpdateRos_.isZero()?0.f:float((now-viewUpdateRos_).toSec());
        viewUpdateRos_=now;
        for(auto &entry:views_)entry.second->controller->update(wall,rosDelta);
    }
    QJsonArray renderViews(bool includeOptions=false) {
        QJsonArray result;
        for(const auto &viewId:viewOrder_) {
            auto *view=viewController(viewId);
            result.append(QJsonObject{{"view_id",viewId},{"class",view->getClassId()},{"controller",node(view,{},includeOptions)}});
        }
        return result;
    }
    QJsonObject viewSnapshot(const QString &viewId) {
        rviz::Config current;auto *view=viewController(viewId);view->save(current);
        const auto saved=mergedConfig(views_.at(viewId)->originalController,current);
        rviz::YamlConfigWriter writer;const auto yaml=writer.writeString(saved);require(!writer.error(),writer.errorMessage());
        require(!yaml.isEmpty()&&yaml.toUtf8().size()<=MaxRequestBytes,"Native camera snapshot exceeds the Undo transport budget; the camera was not removed.");
        return {{"view_id",viewId},{"class",view->getClassId()},{"controller_yaml",yaml}};
    }
    QJsonObject syncViews(const QJsonObject &request) {
        require(request.value("views").isArray(),"views must be an array of {view_id,class} descriptors.");
        const auto requested=request.value("views").toArray();require(requested.size()<=16,"At most 16 additional native view slots are supported per shared scene.");
        QSet<QString> seen;QStringList order;std::map<QString,std::unique_ptr<ViewSlot>> staged;
        std::map<QString,rviz::Config> restored;
        bool changed=false;
        // Validate the complete list before allocating or removing any camera.
        for(const auto &item:requested) {
            require(item.isObject(),"Every native view descriptor must be an object.");
            const auto descriptor=item.toObject();const auto viewId=descriptor.value("view_id").toString();validViewId(viewId);
            require(!seen.contains(viewId),"Duplicate native view_id.");seen.insert(viewId);order.append(viewId);
            const auto plugin=descriptor.value("class").toString();
            require(manager_->getViewManager()->getFactory()->getDeclaredClassIds().contains(plugin),"Native view plugin is not installed.");
            if(descriptor.contains("controller_yaml")) {
                require(descriptor.value("controller_yaml").isString(),"Native controller snapshot must be YAML text.");
                const auto yaml=descriptor.value("controller_yaml").toString();
                require(!yaml.isEmpty()&&yaml.toUtf8().size()<=MaxRequestBytes,"Native controller snapshot is empty or exceeds the request budget.");
                rviz::YamlConfigReader reader;rviz::Config config;reader.readString(config,yaml,"Deleted native camera snapshot");
                require(!reader.error(),reader.errorMessage());QString savedClass;config.mapGetString("Class",&savedClass);
                require(config.getType()==rviz::Config::Map&&savedClass==plugin,"Native controller snapshot class does not match the requested installed plugin.");
                restored.emplace(viewId,config);
            }
        }
        QJsonArray removed;
        for(const auto &viewId:viewOrder_)if(!seen.contains(viewId))removed.append(viewSnapshot(viewId));
        require(QJsonDocument(removed).toJson(QJsonDocument::Compact).size()<=MaxRequestBytes-16384,
            "Deleted camera snapshots exceed the Undo transport budget; the cameras were not removed.");
        for(const auto &item:requested) {
            const auto descriptor=item.toObject();const auto viewId=descriptor.value("view_id").toString(),plugin=descriptor.value("class").toString();
            const auto existing=views_.find(viewId);
            if(existing==views_.end()||existing->second->controller->getClassId()!=plugin){
                const auto saved=restored.find(viewId);
                staged.emplace(viewId,makeView(viewId,plugin,saved==restored.end()?nullptr:&saved->second));changed=true;
            }
        }
        changed=changed||order!=viewOrder_;
        for(auto it=views_.begin();it!=views_.end();)if(!seen.contains(it->first)||staged.count(it->first)){pointerBefore_.remove(it->first);it=views_.erase(it);}else ++it;
        for(auto &entry:staged)views_.emplace(entry.first,std::move(entry.second));
        viewOrder_=order;
        if(changed){++configRevision_;dirty_=true;manager_->queueRender();}
        return {{"changed",changed},{"render_views",renderViews()},{"removed_views",removed},{"scene_id",sceneId_},{"scene_revision",QString::number(configRevision_)},
            {"shared_scene",true},{"independent_cameras",true},{"independent_view_visibility",false}};
    }
    QJsonObject viewReadiness(const QString &viewId) {
        auto result=replay_->readiness(false);
        auto *view=viewController(viewId);QJsonArray frames=result.value("required_frames").toArray();
        bool complete=result.value("required_tf_complete").toBool();
        std::function<void(rviz::Property *)> inspect=[&](rviz::Property *property) {
            if(auto *frame=qobject_cast<rviz::TfFrameProperty *>(property);frame&&!frame->getHidden()&&(frame->getViewFlags(1)&Qt::ItemIsEnabled)) {
                std::string error;const auto target=ros::Time::now();
                const bool available=!frame->getFrame().isEmpty()&&!manager_->getFrameManager()->frameHasProblems(frame->getFrameStd(),target,error)
                    &&!manager_->getFrameManager()->transformHasProblems(frame->getFrameStd(),target,error);
                complete=complete&&available;
                frames.append(QJsonObject{{"view_id",viewId},{"frame",frame->getFrame()},{"target_ns",QString::number(target.toNSec())},
                    {"available",available},{"error",QString::fromStdString(error)}});
            }
            for(int i=0;i<property->numChildren();++i)inspect(property->childAt(i));
        };inspect(view);
        result.insert("required_frames",frames);result.insert("required_tf_complete",complete);
        result.insert("native_scene_ready",result.value("native_scene_ready").toBool()&&complete);return result;
    }
    void send(QJsonObject value) {
        QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
        const char *data = bytes.constData(); qint64 remaining = bytes.size();
        while (remaining > 0) { const auto n = ::write(output_, data, size_t(remaining)); if (n <= 0) break; data += n; remaining -= n; }
    }
    QString id(rviz::Property *p) {
        if (ids_.contains(p)) return ids_.value(p);
        const auto result = QString::number(++nextId_);
        ids_.insert(p, result); properties_.insert(result, p);
        connect(p, &QObject::destroyed, this, [this, p, result] { ids_.remove(p); properties_.remove(result); });
        return result;
    }
    rviz::Property *property(const QJsonObject &request) {
        auto *p = properties_.value(request.value("property_id").toString());
        require(p, "Unknown or expired property_id; refresh tree after loading/deleting a display.");
        return p;
    }
    QString propertyViewId(rviz::Property *property) {
        // Derive ownership from the real native tree; names and caller-supplied
        // view_id are not evidence that a property belongs to a camera.
        for(auto *ancestor=property;ancestor;ancestor=ancestor->getParent()) {
            if(ancestor==manager_->getViewManager()->getCurrent())return "main";
            for(const auto &entry:views_)if(ancestor==entry.second->controller.get())return entry.first;
        }
        return {};
    }
    struct DisplaySource {rviz::Config original;QString instance;QStringList unknown;};
    DisplaySource &displaySource(rviz::Display *display,const rviz::Config *source=nullptr,bool fresh=false) {
        auto found=displaySources_.find(display);
        if(found!=displaySources_.end())return found.value();
        DisplaySource info;
        if(source)info.original.copy(*source);
        if(!fresh)info.original.mapGetChild("UAV Studio").mapGetString("Display ID",&info.instance);
        if(QUuid(info.instance).isNull()||instanceIds_.contains(info.instance))info.instance=QUuid::createUuid().toString(QUuid::WithoutBraces);
        instanceIds_.insert(info.instance);
        rviz::Config native;display->save(native);info.unknown=unknownConfigPaths(info.original,native);
        displaySources_.insert(display,info);
        connect(display,&QObject::destroyed,this,[this,display]{const auto removed=displaySources_.take(display);instanceIds_.remove(removed.instance);});
        return displaySources_[display];
    }
    void bindDisplaySources(rviz::DisplayGroup *group,const rviz::Config &source,bool fresh=false) {
        const auto displays=source.mapGetChild("Displays");
        for(int i=0;i<group->numDisplays();++i) {
            auto *display=group->getDisplayAt(i);const auto original=displays.listChildAt(i);displaySource(display,&original,fresh);
            if(auto *nested=qobject_cast<rviz::DisplayGroup *>(display))bindDisplaySources(nested,original,fresh);
        }
    }
    rviz::Config serializedDisplay(rviz::Display *display) {
        auto &source=displaySource(display);rviz::Config native;display->save(native);auto result=mergedConfig(source.original,native);
        auto metadata=result.mapMakeChild("UAV Studio");metadata.mapSetValue("Display ID",source.instance);metadata.mapSetValue("Scene ID",sceneId_);
        if(auto *group=qobject_cast<rviz::DisplayGroup *>(display)) {
            auto list=result.mapMakeChild("Displays");list.setType(rviz::Config::Empty);list.setType(rviz::Config::List);
            for(int i=0;i<group->numDisplays();++i)list.listAppendNew().copy(serializedDisplay(group->getDisplayAt(i)));
        }
        return result;
    }
    QJsonArray options(rviz::Property *p) {
        QJsonArray result;
        if (!qobject_cast<rviz::EnumProperty *>(p) && !qobject_cast<rviz::EditableEnumProperty *>(p)) return result;
        // 由原生 editor 发出 requestOptions；不能用硬编码枚举替代动态插件选项。
        std::unique_ptr<QWidget> editor(p->createEditor(panel_.get(), QStyleOptionViewItem{}));
        auto *combo = qobject_cast<QComboBox *>(editor.get());
        if (!combo && editor) combo = editor->findChild<QComboBox *>();
        if (combo) for(int i=0; i<combo->count(); ++i) result.append(combo->itemText(i));
        return result;
    }
    QJsonObject node(rviz::Property *p, const QJsonArray &path, bool includeOptions) {
        const auto value = p->getValue(); const auto flags = p->getViewFlags(1);
        const bool statusNode=qobject_cast<rviz::StatusProperty *>(p)!=nullptr;
        QJsonObject result{{"id", id(p)}, {"path", path}, {"name", p->getName()}, {"description", p->getDescription()},
            {"native_type", p->metaObject()->className()}, {"value_type", value.typeName() ? value.typeName() : "invalid"},
            {"value", jsonValue(value)}, {"display_value", p->getViewData(1, Qt::DisplayRole).toString()},
            {"hidden", p->getHidden()}, {"read_only", p->getReadOnly()||statusNode}, {"native_read_only",p->getReadOnly()},
            {"enabled", bool(flags & Qt::ItemIsEnabled)}, {"editable", bool(flags & Qt::ItemIsEditable) && !p->getReadOnly()&&!statusNode},
            {"checkable", bool(flags & Qt::ItemIsUserCheckable)}};
        if (auto *f = qobject_cast<rviz::FloatProperty *>(p)) { result.insert("minimum", double(f->getMin())); result.insert("maximum", double(f->getMax())); }
        if (auto *i = qobject_cast<rviz::IntProperty *>(p)) { result.insert("minimum", i->getMin()); result.insert("maximum", i->getMax()); }
        if (auto *v = qobject_cast<rviz::VectorProperty *>(p)) { const auto a=v->getVector(); result.insert("value", QJsonArray{a.x,a.y,a.z}); }
        if (auto *q = qobject_cast<rviz::QuaternionProperty *>(p)) { const auto a=q->getQuaternion(); result.insert("value", QJsonArray{a.x,a.y,a.z,a.w}); }
        if (auto *status = qobject_cast<rviz::StatusProperty *>(p)) { result.insert("status_level", int(status->getLevel())); result.insert("status", rviz::StatusProperty::statusWord(status->getLevel())); }
        if (auto *d = qobject_cast<rviz::Display *>(p)) {
            auto &source=displaySource(d);result.insert("plugin_class", d->getClassId()); result.insert("display_enabled", d->isEnabled()); result.insert("plugin_failed", dynamic_cast<rviz::FailedDisplay *>(d) != nullptr);
            result.insert("instance_id",source.instance);result.insert("scene_id",sceneId_);result.insert("view_scope","shared-native-scene");
            result.insert("source_id",sourceId_);result.insert("source_binding","host-active-source");result.insert("native_resource_scope","independent-Display-instance");
            result.insert("source_available",!sourceDetached_);result.insert("configured_enabled",d==manager_->getRootDisplayGroup()&&sourceDetached_?configuredRootEnabled_:d->getValue().toBool());
            result.insert("unrecognized_config_paths",QJsonArray::fromStringList(source.unknown));
            if(auto *parent=qobject_cast<rviz::Display *>(d->getParent()))result.insert("parent_instance_id",displaySource(parent).instance);
        }
        if (auto *topic = qobject_cast<rviz::RosTopicProperty *>(p)) {
            result.insert("message_type", topic->getMessageType());
            const auto name=topic->getValue().toString().trimmed();
            result.insert("topic_name",name.isEmpty()?name:QString::fromStdString(ros::names::resolve(name.toStdString())));
        }
        const bool enumType = qobject_cast<rviz::EnumProperty *>(p) || qobject_cast<rviz::EditableEnumProperty *>(p);
        if (enumType) { result.insert("options_available", true); result.insert("allow_custom", qobject_cast<rviz::EditableEnumProperty *>(p) != nullptr); if (includeOptions) result.insert("options", options(p)); }
        QJsonArray children; QHash<QString,int> occurrences;
        for (int i=0;i<p->numChildren();++i) { auto *child=p->childAt(i); auto next=path; next.append(QJsonObject{{"name", child->getName()}, {"occurrence", occurrences[child->getName()]++}}); children.append(node(child,next,includeOptions)); }
        result.insert("children", children); return result;
    }
    QJsonObject tree(bool includeOptions) {
        QJsonObject result{{"revision", QString::number(revision_)}, {"fixed_frame", manager_->getFixedFrame()},
            {"scene_id",sceneId_},{"scene_revision",QString::number(configRevision_)},{"source_id",sourceId_},
            {"source_available",!sourceDetached_},{"history",historyState()},
            {"independent_view_visibility",false},{"simultaneous_sources",false},{"independent_cameras",true},{"maximum_additional_views",16},
            {"active_tool",manager_->getToolManager()->getCurrentTool()?manager_->getToolManager()->getCurrentTool()->getClassId():QString()}};
        result.insert("displays", node(manager_->getDisplayTreeModel()->getRoot(), {}, includeOptions));
        result.insert("views", node(manager_->getViewManager()->getPropertyModel()->getRoot(), {}, includeOptions));
        result.insert("tools", node(manager_->getToolManager()->getPropertyModel()->getRoot(), {}, includeOptions));
        result.insert("render_views",renderViews(includeOptions));
        return result;
    }
    template<class Factory> QJsonArray classes(Factory *factory) {
        QJsonArray result; auto ids=factory->getDeclaredClassIds(); ids.sort();
        for (const auto &name:ids) result.append(QJsonObject{{"class",name},{"name",factory->getClassName(name)},{"package",factory->getClassPackage(name)},{"description",factory->getClassDescription(name)}});
        return result;
    }
    void activateTool(const QString &plugin) {
        auto *tools=manager_->getToolManager();
        require(tools->getFactory()->getDeclaredClassIds().contains(plugin),"Tool plugin is not installed.");
        rviz::Tool *selected=nullptr;
        for(int i=0;i<tools->numTools();++i)if(tools->getTool(i)->getClassId()==plugin){selected=tools->getTool(i);break;}
        if(!selected)selected=tools->addTool(plugin);
        require(selected&&selected->getClassId()==plugin,"Native tool could not be initialized.");
        tools->setCurrentTool(selected);dirty_=true;manager_->queueRender();
    }
    void load(const QString &file) {
        QFile original(file);require(original.open(QIODevice::ReadOnly),"Cannot read original native configuration.");
        require(original.size()<=4*1024*1024,"Native configuration exceeds 4 MiB source-preservation budget.");
        loadYaml(QString::fromUtf8(original.readAll()),file);
    }
    void loadYaml(const QString &yaml,const QString &sourceName="UAV Studio project snapshot") {
        require(yaml.toUtf8().size()<=4*1024*1024,"Native configuration exceeds 4 MiB source-preservation budget.");
        // RViz's reader does not retain the type of empty YAML collections.
        // Validate the original node, then normalize only a verified empty
        // sequence/null so malformed mappings/scalars remain rejected.
        bool emptyRenderViews=false;
        const auto rawConfig=YAML::Load(yaml.toStdString());
        if(rawConfig.IsMap()) {
            const auto rawMetadata=rawConfig["UAV Studio"];
            if(rawMetadata.IsDefined()&&rawMetadata.IsMap()) {
                const auto rawViews=rawMetadata["Render Views"];
                require(!rawViews.IsDefined()||rawViews.IsNull()||rawViews.IsSequence(),"Native Render Views metadata must be a list or null.");
                emptyRenderViews=rawViews.IsDefined()&&(rawViews.IsNull()||(rawViews.IsSequence()&&rawViews.size()==0));
            }
        }
        rviz::YamlConfigReader reader;rviz::Config config;reader.readString(config,yaml,sourceName);require(!reader.error(),reader.errorMessage());
        auto manager=config.mapGetChild("Visualization Manager");require(manager.isValid(),"Configuration has no Visualization Manager mapping.");
        const auto metadata=config.mapGetChild("UAV Studio");bool detached=false;metadata.mapGetBool("Source Detached",&detached);
        auto savedViews=metadata.mapGetChild("Render Views");QStringList restoredIds,restoredClasses;
        if(emptyRenderViews)savedViews.setType(rviz::Config::List);
        require(!savedViews.isValid()||savedViews.getType()==rviz::Config::List,"Native Render Views metadata must be a list or null.");
        require(savedViews.listLength()<=16,"Native configuration exceeds the 16 additional view budget.");
        for(int i=0;i<savedViews.listLength();++i) {
            const auto saved=savedViews.listChildAt(i);QString viewId,plugin;saved.mapGetString("View ID",&viewId);validViewId(viewId);
            require(!restoredIds.contains(viewId),"Native configuration has duplicate view identities.");
            saved.mapGetChild("Controller").mapGetString("Class",&plugin);
            require(manager_->getViewManager()->getFactory()->getDeclaredClassIds().contains(plugin),"Native configuration uses an unavailable view plugin.");
            restoredIds.append(viewId);restoredClasses.append(plugin);
        }
        // Persist removal across a fresh project/backend restart. An explicitly
        // bound runtime source takes precedence when importing just its layout.
        if(detached&&sourceId_=="ros-master"&&bagPath_.isEmpty()&&!replay_->indexed()){sourceDetached_=true;sourceId_="none";}
        if(replay_)replay_->invalidate();
        source_.copy(config);
        manager.mapGetBool("Enabled",&configuredRootEnabled_);
        clearViews();
        if(sourceDetached_){rviz::Config suspended;suspended.copy(manager);suspended.mapSetValue("Enabled",false);manager_->load(suspended);}
        else manager_->load(manager);
        for(int i=0;i<restoredIds.size();++i) {
            const auto controller=savedViews.listChildAt(i).mapGetChild("Controller");
            views_.emplace(restoredIds[i],makeView(restoredIds[i],restoredClasses[i],&controller));
        }
        viewOrder_=restoredIds;
        QString activeTool;metadata.mapGetString("Active Tool",&activeTool);
        if(!activeTool.isEmpty()&&manager_->getToolManager()->getFactory()->getDeclaredClassIds().contains(activeTool))activateTool(activeTool);
        metadata.mapGetString("Scene ID",&sceneId_);
        if(QUuid(sceneId_).isNull())sceneId_=QUuid::createUuid().toString(QUuid::WithoutBraces);
        originalYaml_.clear();metadata.mapGetString("Original Imported YAML",&originalYaml_);if(originalYaml_.isEmpty())originalYaml_=yaml;
        bindDisplaySources(manager_->getRootDisplayGroup(),manager);applySourceStatus();dirty_=true;++configRevision_;
    }
    void applySourceStatus() {
        std::function<void(rviz::Display *)> visit=[&](rviz::Display *display){
            if(sourceDetached_)display->setStatus(rviz::StatusProperty::Warn,"UAV Studio Source","Source removed. Configuration is retained; no source data is available.");
            else display->deleteStatus("UAV Studio Source");
            if(auto *group=qobject_cast<rviz::DisplayGroup *>(display))for(int i=0;i<group->numDisplays();++i)visit(group->getDisplayAt(i));
        };visit(manager_->getRootDisplayGroup());
    }
    void resumeSource() {
        if(!sourceDetached_)return;sourceDetached_=false;
        manager_->getRootDisplayGroup()->setEnabled(configuredRootEnabled_);applySourceStatus();dirty_=true;
    }
    QString snapshotYaml() {
        rviz::Config config;config.copy(source_);rviz::Config native;manager_->save(native);
        auto saved=mergedConfig(source_.mapGetChild("Visualization Manager"),native);
        if(sourceDetached_)saved.mapSetValue("Enabled",configuredRootEnabled_);
        auto list=saved.mapMakeChild("Displays");list.setType(rviz::Config::Empty);list.setType(rviz::Config::List);
        auto *root=manager_->getRootDisplayGroup();for(int i=0;i<root->numDisplays();++i)list.listAppendNew().copy(serializedDisplay(root->getDisplayAt(i)));
        config.mapMakeChild("Visualization Manager").copy(saved);
        auto metadata=config.mapMakeChild("UAV Studio");metadata.mapSetValue("Scene ID",sceneId_);metadata.mapSetValue("Original Imported YAML",originalYaml_);metadata.mapSetValue("Source Detached",sourceDetached_);
        auto savedViews=metadata.mapMakeChild("Render Views");savedViews.setType(rviz::Config::Empty);savedViews.setType(rviz::Config::List);
        for(const auto &viewId:viewOrder_) {
            auto saved=savedViews.listAppendNew();saved.mapSetValue("View ID",viewId);
            rviz::Config current;viewController(viewId)->save(current);
            saved.mapMakeChild("Controller").copy(mergedConfig(views_.at(viewId)->originalController,current));
        }
        if(auto *tool=manager_->getToolManager()->getCurrentTool())metadata.mapSetValue("Active Tool",tool->getClassId());
        rviz::YamlConfigWriter writer;const auto yaml=writer.writeString(config);require(!writer.error(),writer.errorMessage());return yaml;
    }
    QJsonObject historyState() const {
        qint64 bytes=0;for(const auto &entry:undo_)bytes+=entry.toUtf8().size();for(const auto &entry:redo_)bytes+=entry.toUtf8().size();
        return {{"can_undo",!undo_.isEmpty()},{"can_redo",!redo_.isEmpty()},{"undo_count",undo_.size()},{"redo_count",redo_.size()},
            {"snapshot_bytes",QString::number(bytes)},{"scope","native-display-configuration"},{"maximum_entries",40},{"maximum_bytes",QString::number(64*1024*1024)}};
    }
    void trimHistory() {
        auto bytes=[&]{qint64 total=0;for(const auto &entry:undo_)total+=entry.toUtf8().size();for(const auto &entry:redo_)total+=entry.toUtf8().size();return total;};
        while(undo_.size()+redo_.size()>40||bytes()>64*1024*1024){if(undo_.size()>1)undo_.removeFirst();else if(!redo_.isEmpty())redo_.removeFirst();else{undo_.clear();break;}}
    }
    QJsonObject resources() {
        int displays=0,groups=0,propertyCount=0,sceneNodes=0,cameras=0;QJsonObject movable;
        std::function<void(rviz::Property *)> properties=[&](rviz::Property *p){++propertyCount;if(auto *d=qobject_cast<rviz::Display *>(p)){++displays;if(qobject_cast<rviz::DisplayGroup *>(d))++groups;}for(int i=0;i<p->numChildren();++i)properties(p->childAt(i));};
        properties(manager_->getDisplayTreeModel()->getRoot());properties(manager_->getViewManager()->getPropertyModel()->getRoot());properties(manager_->getToolManager()->getPropertyModel()->getRoot());
        for(const auto &viewId:viewOrder_)properties(viewController(viewId));
        std::function<void(Ogre::Node *)> nodes=[&](Ogre::Node *n){++sceneNodes;for(unsigned short i=0;i<n->numChildren();++i)nodes(n->getChild(i));};nodes(manager_->getSceneManager()->getRootSceneNode());
        auto camera=manager_->getSceneManager()->getCameraIterator();while(camera.hasMoreElements()){camera.getNext();++cameras;}
        for(const auto &type:{"Entity","ManualObject","BillboardSet","BillboardChain","PointCloud"}){auto it=manager_->getSceneManager()->getMovableObjectIterator(type);int count=0;while(it.hasMoreElements()){it.getNext();++count;}movable.insert(type,count);}
        std::vector<std::string> frames;manager_->getTF2BufferPtr()->_getFrameStrings(frames);
        QFile proc("/proc/self/status");QString resident;if(proc.open(QIODevice::ReadOnly))for(const auto &line:proc.readAll().split('\n'))if(line.startsWith("VmRSS:"))resident=QString::fromLatin1(line.mid(6)).trimmed();
        QJsonArray subscriberTopics,publisherTopics;XmlRpc::XmlRpcValue arguments,response,payload;arguments[0]=ros::this_node::getName();
        const bool graphAvailable=ros::master::execute("getSystemState",arguments,response,payload,false)&&payload.getType()==XmlRpc::XmlRpcValue::TypeArray&&payload.size()==3;
        if(graphAvailable)for(int kind=0;kind<2;++kind)for(int i=0;i<payload[kind].size();++i){const auto &entry=payload[kind][i];if(entry.getType()!=XmlRpc::XmlRpcValue::TypeArray||entry.size()!=2||entry[1].getType()!=XmlRpc::XmlRpcValue::TypeArray)continue;
            for(int n=0;n<entry[1].size();++n)if(std::string(entry[1][n])==ros::this_node::getName()){auto &list=kind==0?publisherTopics:subscriberTopics;list.append(QString::fromStdString(std::string(entry[0])));break;}}
        return {{"scope","this-native-manager-and-indexed-replay"},{"displays",displays-1},{"groups",groups-1},{"properties",propertyCount},
            {"tracked_properties",properties_.size()},{"tracked_display_sources",displaySources_.size()},{"scene_nodes",sceneNodes},{"cameras",cameras},{"movable_objects",movable},
            {"additional_views",int(views_.size())},{"view_ids",QJsonArray::fromStringList(viewOrder_)},{"shared_scene",true},{"independent_view_visibility",false},
            {"pending_pointer_transactions",pointerBefore_.size()},
            {"tf_frames",int(frames.size())},{"output_texture_retained",!outputTexture_.isNull()},{"replay",replay_->resources()},
            {"playback_running",playback_.state()!=QProcess::NotRunning},{"source_id",sourceId_},{"source_available",!sourceDetached_},
            {"resident_memory",resident},{"gpu_memory_measured",false},{"history",historyState()},
            {"ros_graph_available",graphAvailable},{"owned_node_name",QString::fromStdString(ros::this_node::getName())},
            {"ros_subscriber_topics",subscriberTopics},{"ros_publisher_topics",publisherTopics}};
    }
    void stopPlayback() { if(playback_.state()!=QProcess::NotRunning) {playback_.terminate(); if(!playback_.waitForFinished(2000)) {playback_.kill(); playback_.waitForFinished(2000);}} }
    QImage captureImage(const QString &viewId="main") {
        // QWidget::grab 不能可靠抓取 OGRE 原生 GL 子窗口；读取真正渲染缓冲。
        auto *view=viewController(viewId);require(view&&view->getCamera(),"Native scene camera unavailable.");view->update(0.f,0.f);
        auto *window=viewPanel(viewId)->getRenderWindow();require(window,"Native render window unavailable.");
        const int width=int(window->getWidth()),height=int(window->getHeight());require(width>0&&height>0&&qint64(width)*height<40'000'000,"Invalid native preview dimensions.");
        QImage image(width,height,QImage::Format_RGBA8888);require(!image.isNull(),"Insufficient memory for preview.");image.fill(Qt::transparent);
        window->update();window->copyContentsToMemory(Ogre::PixelBox(size_t(width),size_t(height),1,Ogre::PF_BYTE_RGBA,image.bits()),Ogre::RenderTarget::FB_AUTO);
        return image;
    }
    void releaseOutputTexture() {
        if(outputTexture_.isNull())return;
        outputTexture_->getBuffer()->getRenderTarget()->removeAllViewports();
        const auto name=outputTexture_->getName();outputTexture_.setNull();Ogre::TextureManager::getSingleton().remove(name);
    }
    QJsonObject renderOutput(const QJsonObject &request) {
        const auto viewId=request.value("view_id").toString("main");auto *view=viewController(viewId);
        const auto target=request.value("target_ns").toString();
        require(!target.isEmpty()&&target==replay_->dataTargetNs()&&target==QString::number(ros::Time::now().toNSec())&&replay_->dataSynchronized(),
                "Native output requires an exact synchronized indexed target with all required TF and enabled Displays ready.");
        const auto readiness=viewReadiness(viewId);
        require(readiness.value("native_scene_ready").toBool(),"The requested native view has missing TF or is not ready at this exact experiment time.");
        view->update(0.f,0.f);
        const int width=request.value("width").toInt(),height=request.value("height").toInt();
        require(width>0&&height>0&&width<=16384&&height<=16384&&qint64(width)*height<=67108864,
                "Native output dimensions exceed the 64-megapixel memory budget or 16384-pixel axis limit.");
        QElapsedTimer elapsed;elapsed.start();
        // Activate the native GL context, then inspect the real device limits.
        auto *window=panel_->getRenderWindow();require(window,"Native render window unavailable.");window->update();
        GLint maximum=0,renderbufferMaximum=0,sampleMaximum=0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maximum);glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE,&renderbufferMaximum);glGetIntegerv(GL_MAX_SAMPLES,&sampleMaximum);
        require(maximum>0&&renderbufferMaximum>0,"Native GPU texture limits unavailable.");
        const int limit=std::min(maximum,renderbufferMaximum);
        require(width<=limit&&height<=limit,QString("Output exceeds this GPU's %1-pixel texture/renderbuffer limit; tiled native rendering is not implemented yet.").arg(limit));
        // Cap multisample attachments to a concrete resource budget. 8K still
        // renders at its true resolution; it never enlarges a preview bitmap.
        const int requestedSamples=qint64(width)*height<=16777216?std::min(4,int(sampleMaximum)):0;
        if(outputTexture_.isNull()||int(outputTexture_->getWidth())!=width||int(outputTexture_->getHeight())!=height||outputSamples_!=requestedSamples) {
            releaseOutputTexture();
            outputTexture_=Ogre::TextureManager::getSingleton().createManual("UAVStudioNativeOutput",Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
                Ogre::TEX_TYPE_2D,uint(width),uint(height),0,Ogre::PF_BYTE_RGBA,Ogre::TU_RENDERTARGET,nullptr,false,uint(requestedSamples));
            outputSamples_=requestedSamples;
        }
        auto *render=outputTexture_->getBuffer()->getRenderTarget();render->setAutoUpdated(false);render->removeAllViewports();
        require(view&&view->getCamera(),"No native scene camera.");
        auto *camera=view->getCamera();const auto aspect=camera->getAspectRatio();const bool autoAspect=camera->getAutoAspectRatio();auto *previous=camera->getViewport();
        struct RestoreCamera {Ogre::Camera *camera;Ogre::Real aspect;bool automatic;Ogre::Viewport *viewport;Ogre::RenderTexture *render;
            ~RestoreCamera(){render->removeAllViewports();camera->setAutoAspectRatio(automatic);camera->setAspectRatio(aspect);camera->_notifyViewport(viewport);}
        } restore{camera,aspect,autoAspect,previous,render};
        camera->setAutoAspectRatio(false);camera->setAspectRatio(Ogre::Real(width)/height);
        const auto background=window->getViewport(0)->getBackgroundColour();
        auto *viewport=render->addViewport(camera);viewport->setBackgroundColour(background);
        viewport->setOverlaysEnabled(false);viewport->setClearEveryFrame(true);
        QImage image(width,height,QImage::Format_RGBA8888);require(!image.isNull(),"Insufficient host memory for native output.");
        render->update();render->copyContentsToMemory(Ogre::PixelBox(size_t(width),size_t(height),1,Ogre::PF_BYTE_RGBA,image.bits()),Ogre::RenderTarget::FB_AUTO);
        // This baseline deliberately reports an opaque scene. It does not
        // pretend that arbitrary RViz materials support correct alpha output.
        image=image.convertToFormat(QImage::Format_RGB32);
        require(target==QString::number(ros::Time::now().toNSec())&&replay_->dataSynchronized(),"Native clock or readiness changed during output.");
        require(viewReadiness(viewId).value("native_scene_ready").toBool(),"Requested view TF changed during native output.");
        QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);require(image.save(&buffer,"PNG"),"Cannot encode native output.");
        require(bytes.size()<=128*1024*1024,"Encoded native output exceeds the 128 MiB transport budget.");
        return {{"capture","native-render-texture"},{"native_render_texture",true},{"format","png"},{"data_base64",QString::fromLatin1(bytes.toBase64())},
            {"width",width},{"height",height},{"view_id",viewId},{"clock_ns",target},{"target_ns",target},{"scene_id",sceneId_},{"scene_revision",QString::number(configRevision_)},
            {"native_scene_ready",true},{"required_tf_complete",true},{"experiment_playhead_synchronized",true},{"native_readiness",readiness},
            {"gpu_max_texture_size",maximum},{"gpu_max_renderbuffer_size",renderbufferMaximum},{"fsaa_samples",int(render->getFSAA())},
            {"elapsed_ms",elapsed.elapsed()},{"high_resolution_export",true},{"alpha_supported",false},{"vector",false},
            {"opaque",true},{"background_color",QColor::fromRgbF(background.r,background.g,background.b,1).name(QColor::HexArgb)},
            {"tiled",false},{"plugin_worker_completion_certified",false}};
    }
    QJsonObject execute(const QJsonObject &r) {
        const auto command=r.value("command").toString();
        if(command=="hello") return {{"protocol",1},{"backend","ROS1/Noetic/librviz"},{"rendering","native-rviz-window"},{"embedded_in_main_window",false},{"indexed_full_history_seek",true},{"seek_requires_explicit_time_basis",true},{"realtime_seek_certified",false},{"native_render_texture",true},{"per_view_protocol",true},{"maximum_additional_views",16},{"independent_view_visibility",false}};
        if(command=="bag.cancel") {replay_->cancel("Native replay canceled.");return {{"canceled",true}};}
        if(command=="bag.close") {
            // A new BufferCore is necessary: clear() retains static/dynamic cache
            // types and disabled plugins can otherwise retain their own history.
            replay_->cancel("Native bag source removed.");stopPlayback();const auto yaml=snapshotYaml();const bool visible=panel_->isVisible();
            sourceDetached_=true;sourceId_="none";bagPath_.clear();playbackLog_.clear();releaseScene();
            createScene(visible);loadYaml(yaml);lastPreview_.invalidate();
            return {{"source_removed",true},{"source_files_modified",false},{"native_scene_recreated",true},{"display_configuration_retained",true},
                {"resources",resources()},{"tree",tree(false)}};
        }
        require(!replay_->busy()||command=="tree"||command=="pointer"||command=="shutdown","Native history replay is active; wait for completion or cancel.");
        if(QStringList{"add","remove","remove_many","rename","duplicate","move","reset"}.contains(command)) {
            replay_->cancel("Native display configuration changed; replay again before certifying a synchronized frame.");++configRevision_;
        }
        if(command=="resources")return resources();
        if(command=="history")return historyState();
        if(command=="undo"||command=="redo") {
            auto &from=command=="undo"?undo_:redo_;auto &to=command=="undo"?redo_:undo_;
            require(!from.isEmpty(),"No native configuration history is available.");const auto current=snapshotYaml(),target=from.last();
            loadYaml(target);from.removeLast();to.append(current);trimHistory();historyGroup_.clear();return tree(false);
        }
        if(command=="plugins") {
            auto displays=classes(manager_->getDisplayFactory());
            for(int i=0;i<displays.size();++i) {auto d=displays[i].toObject(); auto types=manager_->getDisplayFactory()->getMessageTypes(d["class"].toString()).values(); types.sort(); d.insert("message_types",QJsonArray::fromStringList(types)); displays[i]=d;}
            return {{"displays",displays},{"views",classes(manager_->getViewManager()->getFactory())},{"tools",classes(manager_->getToolManager()->getFactory())},
                {"active_tool",manager_->getToolManager()->getCurrentTool()?manager_->getToolManager()->getCurrentTool()->getClassId():QString()}};
        }
        if(command=="tool") {activateTool(r.value("class").toString());return tree(false);}
        if(command=="tree") return tree(r.value("include_options").toBool());
        if(command=="views.sync")return syncViews(r);
        if(command=="pointer") {
            const auto viewId=r.value("view_id").toString("main");
            auto *view=viewController(viewId);auto *targetPanel=viewPanel(viewId);auto *window=targetPanel->getRenderWindow();require(view&&window&&window->getNumViewports()>0,"Native scene camera/viewport unavailable.");
            const auto kind=r.value("type").toString();require(QStringList{"press","move","release","wheel"}.contains(kind),"Unsupported native pointer event type.");
            const auto nx=r.value("x").toDouble(std::numeric_limits<double>::quiet_NaN()),ny=r.value("y").toDouble(std::numeric_limits<double>::quiet_NaN());
            require(std::isfinite(nx)&&std::isfinite(ny)&&nx>=-1&&nx<=2&&ny>=-1&&ny<=2,"Pointer positions must be finite normalized viewport coordinates.");
            const int button=r.value("button").toInt(),buttons=r.value("buttons").toInt(),modifiers=r.value("modifiers").toInt(),wheel=r.value("wheel_delta").toInt();
            require((button==0||button==1||button==2||button==4)&&buttons>=0&&buttons<=7&&std::abs(qint64(wheel))<=14400,"Invalid native pointer button or wheel state.");
            rviz::ViewportMouseEvent event;event.panel=targetPanel;event.viewport=window->getViewport(0);
            event.type=kind=="press"?QEvent::MouseButtonPress:kind=="release"?QEvent::MouseButtonRelease:kind=="wheel"?QEvent::Wheel:QEvent::MouseMove;
            event.x=qRound(nx*event.viewport->getActualWidth());event.y=qRound(ny*event.viewport->getActualHeight());
            auto &lastX=viewId=="main"?pointerX_:views_.at(viewId)->pointerX;auto &lastY=viewId=="main"?pointerY_:views_.at(viewId)->pointerY;
            event.last_x=kind=="press"?event.x:lastX;event.last_y=kind=="press"?event.y:lastY;lastX=event.x;lastY=event.y;
            event.acting_button=Qt::MouseButton(button);event.buttons_down=Qt::MouseButtons(buttons);event.modifiers=Qt::KeyboardModifiers(modifiers);event.wheel_delta=wheel;
            auto *tools=manager_->getToolManager();auto *tool=tools->getCurrentTool();
            // Use the real selected RViz Tool, including Select/Measure/Focus.
            // MoveCamera delegates to the same active native ViewController.
            int flags=0;if(tool)flags=tool->processMouseEvent(event);else view->handleMouseEvent(event);
            const auto toolClass=tool?tool->getClassId():QString();
            if((flags&rviz::Tool::Finished)&&tools->getDefaultTool())tools->setCurrentTool(tools->getDefaultTool());
            manager_->queueRender();dirty_=true;++configRevision_;
            return {{"handled_by",tool?"native-Tool":"native-ViewController"},{"view_id",viewId},{"class",view->getClassId()},{"tool_class",toolClass},
                {"active_tool",tools->getCurrentTool()?tools->getCurrentTool()->getClassId():QString()},
                {"scene_revision",QString::number(configRevision_)},{"clock_ns",QString::number(ros::Time::now().toNSec())}};
        }
        if(command=="transform") {
            bool ok=false;const auto ns=r.value("time_ns").toString().toULongLong(&ok);
            require(ok&&ns>0&&ns<=uint64_t(UINT32_MAX)*1000000000ULL+999999999ULL,"time_ns must be an exact positive ROS1 nanosecond string.");
            const auto frame=r.value("frame").toString();const auto target=r.value("target_frame").toString(manager_->getFixedFrame());
            require(!frame.isEmpty()&&!target.isEmpty(),"Specify real source and target TF frames.");
            ros::Time time;time.fromNSec(ns);
            const auto value=manager_->getTF2BufferPtr()->lookupTransform(target.toStdString(),frame.toStdString(),time);
            Ogre::Vector3 position;Ogre::Quaternion orientation;
            const bool exact=manager_->getFrameManager()->getTransform(frame.toStdString(),ros::Time(),position,orientation);
            auto *camera=viewController(r.value("view_id").toString("main"))->getCamera();const auto cameraPosition=camera->getDerivedPosition();
            return {{"time_ns",QString::number(ns)},{"source_frame",frame},{"target_frame",target},
                {"translation",QJsonArray{value.transform.translation.x,value.transform.translation.y,value.transform.translation.z}},
                {"rotation_xyzw",QJsonArray{value.transform.rotation.x,value.transform.rotation.y,value.transform.rotation.z,value.transform.rotation.w}},
                {"frame_manager_zero_time_available",exact},{"frame_manager_time_ns",QString::number(manager_->getFrameManager()->getTime().toNSec())},
                {"frame_manager_zero_time_translation",QJsonArray{position.x,position.y,position.z}},
                {"camera_world_position",QJsonArray{cameraPosition.x,cameraPosition.y,cameraPosition.z}}};
        }
        if(command=="options") { auto *p=property(r); return {{"property_id",id(p)},{"options",options(p)}}; }
        if(command=="load") {load(r["file"].toString());return tree(false);}
        if(command=="load_yaml") {loadYaml(r["yaml"].toString());return tree(false);}
        if(command=="save") {
            const auto yaml=snapshotYaml();
            if(r.contains("file")) {QSaveFile file(r["file"].toString()); const auto data=yaml.toUtf8(); require(file.open(QIODevice::WriteOnly)&&file.write(data)==data.size()&&file.commit(),"Cannot save native RViz config.");}
            return {{"yaml",yaml},{"writer","native-rviz-with-preserved-source"},{"unknown_properties_preserved",true},{"original_imported_yaml_preserved",!originalYaml_.isEmpty()}};
        }
        if(command=="set") {
            auto *p=property(r); const auto flags=p->getViewFlags(1); const auto value=r["value"];
            const auto editedView=propertyViewId(p);
            require(!sourceDetached_||p!=manager_->getRootDisplayGroup(),"Bind a source before enabling the native scene; retained Display configurations remain editable.");
            require(!p->getReadOnly()&&!qobject_cast<rviz::StatusProperty *>(p) && (flags & Qt::ItemIsEnabled) && ((flags & Qt::ItemIsEditable)||(flags & Qt::ItemIsUserCheckable)),"Property is read-only or disabled in native RViz.");
            if(auto *v=qobject_cast<rviz::VectorProperty *>(p)) {const auto a=value.toArray();require(a.size()==3,"Vector requires [x,y,z].");for(auto c:a)require(c.isDouble()&&std::isfinite(c.toDouble())&&std::abs(c.toDouble())<=std::numeric_limits<Ogre::Real>::max(),"Vector requires finite native-range numbers.");v->setVector(Ogre::Vector3(a[0].toDouble(),a[1].toDouble(),a[2].toDouble()));}
            else if(auto *q=qobject_cast<rviz::QuaternionProperty *>(p)) {const auto a=value.toArray();require(a.size()==4,"Quaternion requires [x,y,z,w].");for(auto c:a)require(c.isDouble()&&std::isfinite(c.toDouble())&&std::abs(c.toDouble())<=std::numeric_limits<Ogre::Real>::max(),"Quaternion requires finite native-range numbers.");q->setQuaternion(Ogre::Quaternion(a[3].toDouble(),a[0].toDouble(),a[1].toDouble(),a[2].toDouble()));}
            else {
                QVariant native=value.toVariant(); const auto old=p->getValue();
                if(qobject_cast<rviz::ColorProperty *>(p)) {const QColor color(value.toString());require(color.isValid(),"Invalid color.");native=color;}
                else if(old.userType()==QMetaType::Bool) {require(value.isBool(),"Boolean property requires true/false.");native=value.toBool();}
                else if(old.userType()==QMetaType::Int) {require(value.isDouble()&&std::isfinite(value.toDouble())&&std::floor(value.toDouble())==value.toDouble(),"Integer property requires an integer.");require(value.toDouble()>=std::numeric_limits<int>::min()&&value.toDouble()<=std::numeric_limits<int>::max(),"Integer out of range.");native=value.toInt();}
                else if(old.userType()==QMetaType::Float||old.userType()==QMetaType::Double) {
                    require(value.isDouble()&&std::isfinite(value.toDouble()),"Numeric property requires a finite number.");
                    if(old.userType()==QMetaType::Float)require(std::abs(value.toDouble())<=std::numeric_limits<float>::max(),"Numeric property exceeds its native float range.");
                    native=value.toDouble();
                }
                else require(value.isString(),"String property requires a string.");
                if(qobject_cast<rviz::EnumProperty *>(p)) require(options(p).contains(value),"Value is not among current native enum options.");
                p->setValue(native);
            }
            // Validation has completed. Camera edits change projection/TF
            // readiness, never the already restored experimental history.
            if(editedView.isEmpty())replay_->cancel("Native display configuration changed; replay again before certifying a synchronized frame.");
            else if(editedView=="main")lastPreview_.invalidate();
            else views_.at(editedView)->lastPreview.invalidate();
            ++configRevision_;manager_->queueRender();dirty_=true;
            return {{"property",node(p,{},false)},{"view_id",editedView},{"data_history_preserved",!editedView.isEmpty()},
                {"scene_revision",QString::number(configRevision_)}};
        }
        if(command=="add") {
            const auto plugin=r["class"].toString();require(manager_->getDisplayFactory()->getDeclaredClassIds().contains(plugin),"Plugin class is not installed.");
            rviz::DisplayGroup *group=nullptr;
            if(r.contains("parent_id")) {auto parent=r;parent["property_id"]=r["parent_id"];group=qobject_cast<rviz::DisplayGroup *>(property(parent));require(group,"Parent must be a DisplayGroup.");}
            auto *d=manager_->createDisplay(plugin,r["name"].toString(),r.value("enabled").toBool());require(d,"Display could not be created.");
            if(dynamic_cast<rviz::FailedDisplay *>(d)) {delete d;fail("Native plugin failed to instantiate.");}
            if(group) {qobject_cast<rviz::DisplayGroup *>(d->getParent())->takeDisplay(d);group->addDisplay(d);}
            displaySource(d);applySourceStatus();
            dirty_=true;return {{"display",node(d,{},false)}};
        }
        if(command=="remove_many") {
            const auto entries=r.value("property_ids").toArray();require(!entries.isEmpty(),"Select at least one Display to remove.");QSet<rviz::Display *> requested;
            for(const auto &entry:entries){auto item=r;item["property_id"]=entry;auto *display=qobject_cast<rviz::Display *>(property(item));require(display&&display!=manager_->getRootDisplayGroup(),"Every selection must be a non-root Display or Group.");requested.insert(display);}
            QList<rviz::Display *> roots;for(auto *display:requested){bool covered=false;for(auto *parent=display->getParent();parent;parent=parent->getParent())if(requested.contains(qobject_cast<rviz::Display *>(parent))){covered=true;break;}if(!covered)roots.append(display);}
            QJsonArray removed;std::function<void(rviz::Display *)> collect=[&](rviz::Display *display){removed.append(displaySource(display).instance);if(auto *group=qobject_cast<rviz::DisplayGroup *>(display))for(int i=0;i<group->numDisplays();++i)collect(group->getDisplayAt(i));};
            for(auto *display:roots)collect(display);for(auto *display:roots)delete display;
            manager_->queueRender();dirty_=true;return {{"removed_instance_ids",removed},{"removed_count",removed.size()},{"transaction","one-native-history-command"}};
        }
        if(command=="remove"||command=="rename"||command=="duplicate"||command=="move"||command=="reset") {
            auto *d=qobject_cast<rviz::Display *>(property(r));require(d&&d!=manager_->getRootDisplayGroup(),"Select a non-root Display.");
            if(command=="remove") {delete d;dirty_=true;return {};}
            if(command=="rename") {const auto name=r["name"].toString();require(!name.trimmed().isEmpty(),"Name cannot be empty.");d->setName(name);}
            if(command=="reset") d->reset();
            if(command=="move") {
                auto *parent=qobject_cast<rviz::DisplayGroup *>(d->getParent());require(parent,"Display parent is not a native Group.");
                auto *destination=parent;if(r.contains("parent_id")){auto request=r;request["property_id"]=r["parent_id"];destination=qobject_cast<rviz::DisplayGroup *>(property(request));require(destination,"Move destination must be a native Group.");}
                for(auto *ancestor=static_cast<rviz::Property *>(destination);ancestor;ancestor=ancestor->getParent())require(ancestor!=d,"A Group cannot be moved into itself or its descendants.");
                const int index=r.value("index").toInt(-1);require(index>=-1&&index<destination->numChildren()+(destination!=parent?1:0),"Invalid destination property index.");
                if(destination!=parent){require(parent->takeDisplay(d)==d,"Display could not be detached from its Group.");destination->addDisplay(d);}
                if(index>=0){int from=0;while(from<destination->numChildren()&&destination->childAt(from)!=d)++from;require(from<destination->numChildren(),"Display has no valid parent index.");destination->moveChild(from,index);}
            }
            if(command=="duplicate") {auto config=serializedDisplay(d);auto *copy=manager_->createDisplay(d->getClassId(),d->getName(),false);if(!copy||dynamic_cast<rviz::FailedDisplay *>(copy)){delete copy;fail("Duplicate plugin failed.");}copy->load(config);copy->setName(r.value("name").toString(d->getName()+" Copy"));auto *group=qobject_cast<rviz::DisplayGroup *>(d->getParent());if(group&&copy->getParent()!=group){qobject_cast<rviz::DisplayGroup *>(copy->getParent())->takeDisplay(copy);group->addDisplay(copy);}displaySource(copy,&config,true);if(auto *copyGroup=qobject_cast<rviz::DisplayGroup *>(copy))bindDisplaySources(copyGroup,config,true);d=copy;}
            dirty_=true;manager_->queueRender();return {{"display",node(d,{},false)}};
        }
        if(command=="view") {
            const auto plugin=r["class"].toString();require(manager_->getViewManager()->getFactory()->getDeclaredClassIds().contains(plugin),"View plugin is not installed.");
            manager_->getViewManager()->setCurrentViewControllerType(plugin);
            ++configRevision_;lastPreview_.invalidate();manager_->queueRender();dirty_=true;return tree(false);
        }
        if(command=="view.reset") {
            const auto viewId=r.value("view_id").toString("main");auto *view=viewController(viewId);require(view,"No active native ViewController.");
            view->reset();view->update(0.f,0.f);manager_->queueRender();dirty_=true;++configRevision_;
            return tree(false);
        }
        if(command=="window") {require(r["visible"].isBool(),"visible requires true/false.");hidden_=!r["visible"].toBool();panel_->setAttribute(Qt::WA_DontShowOnScreen,hidden_);panel_->setVisible(!hidden_);return {{"visible",panel_->isVisible()}};}
        if(command=="frame") {
            const auto viewId=r.value("view_id").toString("main");viewController(viewId);
            auto &previewTimer=viewId=="main"?lastPreview_:views_.at(viewId)->lastPreview;
            if(previewTimer.isValid()&&previewTimer.elapsed()<250)return {{"view_id",viewId},{"throttled",true},{"retry_after_ms",250-previewTimer.elapsed()}};
            QElapsedTimer elapsed;elapsed.start();previewTimer.restart();
            const int maximum=r.value("max_width").toInt(960);require(maximum>=160&&maximum<=1280,"Preview max_width must be 160–1280.");
            auto image=captureImage(viewId);if(image.width()>maximum)image=image.scaledToWidth(maximum,Qt::SmoothTransformation);
            QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);require(image.save(&buffer,"PNG"),"Cannot encode native preview.");
            const auto readiness=viewReadiness(viewId);
            return {{"capture","native-framebuffer-preview"},{"format","png"},{"data_base64",QString::fromLatin1(bytes.toBase64())},
                {"width",image.width()},{"height",image.height()},{"view_id",viewId},{"scene_id",sceneId_},{"scene_revision",QString::number(configRevision_)},{"clock_ns",QString::number(ros::Time::now().toNSec())},
                {"elapsed_ms",elapsed.elapsed()},{"high_resolution_export",false},{"experiment_playhead_synchronized",replay_->dataSynchronized()},
                {"synchronized_target_ns",replay_->dataTargetNs()},{"native_scene_ready",readiness.value("native_scene_ready")},
                {"required_tf_complete",readiness.value("required_tf_complete")},{"native_readiness",readiness},{"plugin_worker_completion_certified",false}};
        }
        if(command=="render_frame")return renderOutput(r);
        if(command=="capture") {
            const auto image=captureImage();
            require(image.save(r["file"].toString()),"Cannot save native framebuffer preview.");
            return {{"capture","native-framebuffer-preview"},{"width",image.width()},{"height",image.height()},{"high_resolution_export",false}};
        }
        if(command=="topics") {ros::master::V_TopicInfo topics;require(ros::master::getTopics(topics),"ROS master topic discovery failed.");QJsonArray list;for(const auto &t:topics)list.append(QJsonObject{{"name",QString::fromStdString(t.name)},{"type",QString::fromStdString(t.datatype)},{"source_id","ros:"+QString::fromStdString(ros::master::getURI())},{"source_kind","live-ros-master"},{"message_definition_available",false}});return {{"topics",list}};}
        if(command=="bag.open") {
            replay_->invalidate();
            stopPlayback();const auto path=r["file"].toString();rosbag::Bag bag(path.toStdString(),rosbag::bagmode::Read);rosbag::View view(bag);QJsonArray topics;
            sourceId_="bag:"+QFileInfo(path).canonicalFilePath();
            for(const auto *c:view.getConnections())topics.append(QJsonObject{{"name",QString::fromStdString(c->topic)},{"type",QString::fromStdString(c->datatype)},{"md5",QString::fromStdString(c->md5sum)},
                {"source_id",sourceId_},{"source_kind","ros1-bag"},{"connection_id",QString::number(c->id)},
                {"message_definition",QString::fromStdString(c->msg_def)},{"message_definition_available",!c->msg_def.empty()}});
            require(view.size()>0,"Bag contains no messages.");bagPath_=path;resumeSource();return {{"topics",topics},{"message_count",QString::number(view.size())},{"begin_ns",QString::number(view.getBeginTime().toNSec())},{"end_ns",QString::number(view.getEndTime().toNSec())},{"playback_started",false}};
        }
        if(command=="bag.play_from_start") {
            replay_->cancel("Record-clock playback replaced indexed experiment replay.");
            require(!bagPath_.isEmpty(),"Open a bag first.");stopPlayback();const auto executable=QStandardPaths::findExecutable("rosbag");require(!executable.isEmpty(),"rosbag executable not found.");
            bool simulated=false;ros::param::get("/use_sim_time",simulated);require(simulated,"Bag playback requires /use_sim_time=true. Start this host with --sim-time on a dedicated ROS master.");
            const double rate=r.value("rate").toDouble(1),duration=r.value("duration").toDouble(0);require(rate>0&&rate<=16&&duration>=0,"Invalid playback rate or duration.");
            manager_->resetTime();QStringList args{"play","--clock","--delay","1","--rate",QString::number(rate,'g',12)};if(duration>0)args<<"--duration"<<QString::number(duration,'g',12);args<<bagPath_;
            playbackLog_.clear();playback_.setProcessChannelMode(QProcess::SeparateChannels);playback_.setStandardOutputFile(QProcess::nullDevice());playback_.start(executable,args);require(playback_.waitForStarted(2000),playback_.errorString());
            return {{"started",true},{"from_beginning",true},{"clock","rosbag --clock"},{"random_seek_history_restoration",false}};
        }
        if(command=="bag.stop") {stopPlayback();return {{"stopped",true},{"stderr",QString::fromLocal8Bit(playbackLog_)}};}
        if(command=="shutdown") {replay_->cancel("Native host shutdown.");stopPlayback();QTimer::singleShot(0,qApp,&QApplication::quit);return {};}
        fail("Unknown command.");
    }
    void dispatch(const QByteArray &line) {
        QJsonParseError error;const auto document=QJsonDocument::fromJson(line,&error);const auto request=document.object();
        QJsonObject response{{"request_id",request.value("request_id")}};
        try {
            require(error.error==QJsonParseError::NoError&&document.isObject(),"Invalid JSON object.");
            const auto command=request.value("command").toString();
            if(command=="bag.index"||command=="bag.seek"||command=="bag.advance") {
                require(!replay_->busy(),"Another indexed replay job is active.");stopPlayback();
                auto reply=[this,request](bool ok,const QJsonObject &result,const QString &error){
                    QJsonObject response{{"request_id",request.value("request_id")},{"ok",ok}};
                    if(ok)response.insert("result",result);else response.insert("error",error);send(response);dirty_=true;
                };
                if(command=="bag.index"){sourceId_="bag:"+QFileInfo(request.value("file").toString()).canonicalFilePath();bagPath_=request.value("file").toString();resumeSource();replay_->index(request,reply);}
                else if(command=="bag.advance")replay_->advance(request,reply);else replay_->seek(request,reply);
                return;
            }
            const bool mutation=QStringList{"set","add","remove","remove_many","rename","duplicate","move","reset","view","view.reset","views.sync","tool","load","load_yaml"}.contains(command);
            const auto before=mutation?snapshotYaml():QString();
            const auto viewId=request.value("view_id").toString("main");
            if(command=="pointer")viewController(viewId);
            if(command=="pointer"&&request.value("type").toString()=="press")pointerBefore_[viewId]=snapshotYaml();
            const auto pointerBefore=command=="pointer"&&request.value("type").toString()=="wheel"?snapshotYaml():QString();
            auto result=execute(request);
            QString previous=before;
            if(command=="pointer"&&request.value("type").toString()=="release")previous=pointerBefore_.take(viewId);
            else if(!pointerBefore.isEmpty())previous=pointerBefore;
            if(!previous.isEmpty()&&previous!=snapshotYaml()) {
                const auto group=request.value("history_group").toString();
                if(group.isEmpty()||group!=historyGroup_)undo_.append(previous);
                redo_.clear();historyGroup_=group;trimHistory();
            }
            // Cache acknowledged edits atomically with their success response;
            // a disconnect before a later save must not lose the last edit.
            if(mutation||command=="undo"||command=="redo"||command=="bag.close"||
                (command=="pointer"&&QStringList{"release","wheel"}.contains(request.value("type").toString())))
                result.insert("configuration_yaml",snapshotYaml());
            result.insert("history",historyState());response.insert("result",result);response.insert("ok",true);
        }
        catch(const std::exception &e) {response.insert("ok",false);response.insert("error",QString::fromUtf8(e.what()));}
        send(response);
    }
    int output_; QByteArray input_; QTimer notifications_; QElapsedTimer lastPreview_; bool dirty_=true; quint64 revision_=0,nextId_=0,configRevision_=0;
    Ogre::TexturePtr outputTexture_;int outputSamples_=0;
    int pointerX_=0,pointerY_=0;
    bool sourceDetached_=false,configuredRootEnabled_=true,hidden_=false;
    QStringList undo_,redo_;QString historyGroup_;QHash<QString,QString> pointerBefore_;
    std::map<QString,std::unique_ptr<ViewSlot>> views_;QStringList viewOrder_;
    QElapsedTimer viewUpdateWall_;ros::Time viewUpdateRos_;bool viewsUpdateQueued_=false;
    QHash<rviz::Property *,QString> ids_; QHash<QString,rviz::Property *> properties_;
    QHash<rviz::Display *,DisplaySource> displaySources_;QSet<QString> instanceIds_;
    QString sceneId_=QUuid::createUuid().toString(QUuid::WithoutBraces),sourceId_="ros-master",originalYaml_;
    std::unique_ptr<rviz::RenderPanel> panel_; std::unique_ptr<rviz::VisualizationManager> manager_;
    std::unique_ptr<NativeBagReplay> replay_;
    rviz::Config source_; QProcess playback_; QByteArray playbackLog_; QString bagPath_;
};
}
int main(int argc,char **argv) {
    const int output=::dup(STDOUT_FILENO); if(output<0)return 2;
    // OGRE / ROS 及第三方插件可写 stdout；协议必须始终独占原 stdout 管道。
    ::dup2(STDERR_FILENO,STDOUT_FILENO);
    ros::init(argc,argv,"uav_studio_rviz_host",ros::init_options::AnonymousName);
    // Camera/Image and third-party Displays may own top-level auxiliary
    // windows. Removing one must never terminate the shared protocol host.
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    if(!ros::master::check()) {const QByteArray error="{\"event\":\"fatal\",\"error\":\"ROS master unavailable\"}\n";::write(output,error.constData(),size_t(error.size()));return 3;}
    QString initial;bool visible=true;
    for(int i=1;i<argc;++i) {const auto argument=QString::fromLocal8Bit(argv[i]);if(argument=="--sim-time")ros::param::set("/use_sim_time",true);else if(argument=="--hidden")visible=false;else if(initial.isEmpty())initial=argument;}
    try {Host host(output,initial,visible);return app.exec();}
    catch(const std::exception &e) {const auto error=QJsonDocument(QJsonObject{{"event","fatal"},{"error",QString::fromUtf8(e.what())}}).toJson(QJsonDocument::Compact)+'\n';::write(output,error.constData(),size_t(error.size()));return 1;}
}

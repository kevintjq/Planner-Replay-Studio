#include "bag_replay.h"
#include "header_layout.h"
#include <ros/ros.h>
#include <ros/callback_queue.h>
#include <rosbag/bag.h>
#include <rosbag/view.h>
#include <topic_tools/shape_shifter.h>
#include <tf2_msgs/TFMessage.h>
#include <tf2/buffer_core.h>
#include <visualization_msgs/MarkerArray.h>
#include <std_msgs/Header.h>
#include <rosgraph_msgs/Clock.h>
#include <rviz/visualization_manager.h>
#include <rviz/frame_manager.h>
#include <rviz/display_group.h>
#include <rviz/view_manager.h>
#include <rviz/view_controller.h>
#include <rviz/properties/property_tree_model.h>
#include <rviz/properties/ros_topic_property.h>
#include <rviz/properties/status_property.h>
#include <rviz/properties/tf_frame_property.h>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
void check(bool ok, const QString &error) { if (!ok) throw std::runtime_error(error.toStdString()); }
uint64_t ns(const QJsonValue &value)
{
    bool ok=false; const auto text=value.toString(); const auto n=text.toULongLong(&ok);
    check(ok && !text.startsWith('-') && n>0 && n<=uint64_t(UINT32_MAX)*1000000000ULL+999999999ULL,
          "Time must be an exact positive ROS1 absolute-nanosecond string."); return n;
}
ros::Time timeOf(uint64_t n) { ros::Time t; t.fromNSec(n); return t; }
bool firstHeader(const rosbag::MessageInstance &message)
{
    return native_replay::hasLeadingHeader(QString::fromStdString(message.getDataType()),
                                         QString::fromStdString(message.getMessageDefinition()));
}
std_msgs::Header leadingHeader(const rosbag::MessageInstance &message)
{
    check(message.size()>=16,"Truncated leading Header.");
    std::vector<uint8_t> bytes(message.size()); ros::serialization::OStream stream(bytes.data(),uint32_t(bytes.size()));message.write(stream);
    ros::serialization::IStream input(bytes.data(),uint32_t(bytes.size()));std_msgs::Header header;input.next(header);
    check(header.stamp.nsec<1000000000,"Invalid Header nanoseconds.");return header;
}
}

class NativeBagReplay::Private {
public:
    struct Event {
        std::shared_ptr<const rosbag::MessageInstance> message;
        uint64_t stamp=0, header=0;
        int child=-1; // TFMessage / MarkerArray split into their actual child Header clocks.
        bool transform=false, staticTransform=false;
        QString edge,frame;bool frameLocked=false;
        explicit Event(const rosbag::MessageInstance &m):message(std::make_shared<const rosbag::MessageInstance>(m)){}
    };
    struct Topic {QString type,md5,definition;bool latched=false;uint64_t count=0,first=UINT64_MAX,last=0,mapped=0;};
    enum Mode { Idle, Index, Prepare, Replay, Settle } mode=Idle;
    NativeBagReplay *owner;rviz::VisualizationManager *manager;
    std::function<void(QJsonObject)> notify;Reply reply;
    QTimer timer;QElapsedTimer elapsed,stage,progress;
    std::unique_ptr<rosbag::Bag> bag;std::unique_ptr<rosbag::View> view;
    std::unique_ptr<rosbag::View::iterator> iterator;
    std::vector<Event> events;
    std::map<QString,Topic> topics;
    ros::NodeHandle node;ros::Publisher clock;
    std::map<QString,ros::Publisher> publishers;
    std::vector<ros::Subscriber> observers;
    std::map<QString,uint64_t> received,sent,lastHeader,lastTime;
    std::map<QString,std::map<QString,uint64_t>> deliveredFrames;
    QJsonObject indexResult;
    QString basis,policy,path;QSet<QString> selectedTopics;int64_t offset=0;
    size_t cursor=0,end=0,tfCursor=0;std::vector<size_t> transforms;
    uint64_t target=0,indexedMessages=0,totalMessages=0,skipped=0,pendingCount=0;
    QString pendingTopic;int settled=0;bool valid=false,synced=false,updatesStopped=false,incremental=false;
    size_t startedCursor=0;
    QSet<qulonglong> injected;
    QJsonArray tfSamples;
    bool tfCoverage=true;
    std::map<QString,bool> usedTfSchema, sourceTfSchema;
    Private(NativeBagReplay *o,rviz::VisualizationManager *m,std::function<void(QJsonObject)> e):owner(o),manager(m),notify(std::move(e)) {
        timer.setInterval(1);QObject::connect(&timer,&QTimer::timeout,o,[this]{try{tick();}catch(const std::exception &e){finish(false,{},QString::fromUtf8(e.what()));}});
    }
    ~Private(){timer.stop();if(updatesStopped)manager->startUpdate();}
    void finish(bool ok,QJsonObject result={},QString error={}) {
        timer.stop();mode=Idle;pendingTopic.clear();
        if(updatesStopped){manager->startUpdate();updatesStopped=false;}
        if(!ok)synced=false;
        auto callback=std::move(reply);reply={};if(callback)callback(ok,result,error);
    }
    void drain() {
        for(auto *queue:{manager->getUpdateQueue(),manager->getThreadedQueue()})
            if(auto *concrete=dynamic_cast<ros::CallbackQueue *>(queue))concrete->callAvailable(ros::WallDuration(0));
    }
    void update() {QMetaObject::invokeMethod(manager,"onUpdate",Qt::DirectConnection);}
    void advance(uint64_t value) {
        ros::Time::setNow(timeOf(value));manager->getFrameManager()->syncTime(timeOf(value));
    }
    uint64_t experimentTime(uint64_t header,uint64_t record,const QString &topic) {
        if(basis=="record")return record;
        if(header)return header;
        if(policy=="reject")throw std::runtime_error(("No nonzero leading Header for "+topic+"; select an explicit untimed policy.").toStdString());
        if(policy=="skip") {++skipped;return 0;}
        check(record<=uint64_t(INT64_MAX),"Record timestamp cannot be mapped.");
        const auto mapped=int64_t(record)+offset;check(mapped>0,"Explicit time mapping produced nonpositive time.");
        ++topics[topic].mapped;return uint64_t(mapped);
    }
    void add(Event event,uint64_t header) {
        const auto topic=QString::fromStdString(event.message->getTopic());event.header=header;
        event.stamp=experimentTime(header,event.message->getTime().toNSec(),topic);if(!event.stamp)return;
        auto &info=topics[topic];++info.count;info.first=std::min(info.first,event.stamp);info.last=std::max(info.last,event.stamp);events.push_back(std::move(event));
    }
    void indexMessage(const rosbag::MessageInstance &message) {
        const auto topic=QString::fromStdString(message.getTopic());
        if(topic=="/clock"||(!selectedTopics.isEmpty()&&!selectedTopics.contains(topic))){++skipped;return;}
        auto &info=topics[topic];const auto type=QString::fromStdString(message.getDataType());
        check(info.type.isEmpty()||info.type==type,"A topic changes message type; choose separate sources.");
        info.type=type;info.md5=QString::fromStdString(message.getMD5Sum());info.definition=QString::fromStdString(message.getMessageDefinition());info.latched=message.isLatching();
        if(type=="tf2_msgs/TFMessage") {
            const auto data=message.instantiate<tf2_msgs::TFMessage>();check(bool(data),"Invalid TFMessage payload.");
            for(size_t i=0;i<data->transforms.size();++i){const auto &tf=data->transforms[i];Event event(message);event.child=int(i);event.transform=true;event.staticTransform=topic=="/tf_static";event.edge=QString::fromStdString(tf.header.frame_id+"\n"+tf.child_frame_id);add(std::move(event),tf.header.stamp.toNSec());}
        } else if(type=="visualization_msgs/MarkerArray") {
            const auto data=message.instantiate<visualization_msgs::MarkerArray>();check(bool(data),"Invalid MarkerArray payload.");
            for(size_t i=0;i<data->markers.size();++i){const auto &marker=data->markers[i];Event event(message);event.child=int(i);
                if(marker.action!=visualization_msgs::Marker::DELETE&&marker.action!=visualization_msgs::Marker::DELETEALL)event.frame=QString::fromStdString(marker.header.frame_id);
                event.frameLocked=marker.frame_locked;add(std::move(event),marker.header.stamp.toNSec());}
        } else {Event event(message);uint64_t stamp=0;if(firstHeader(message)){const auto header=leadingHeader(message);stamp=header.stamp.toNSec();event.frame=QString::fromStdString(header.frame_id);}add(std::move(event),stamp);}
    }
    void indexedDone() {
        check(!events.empty(),"No messages remain under this explicit time policy.");
        std::stable_sort(events.begin(),events.end(),[](const Event &a,const Event &b){return a.stamp<b.stamp;});
        transforms.clear();for(size_t i=0;i<events.size();++i)if(events[i].transform)transforms.push_back(i);
        sourceTfSchema.clear();
        for(const auto index:transforms) {
            const auto &event=events[index];const auto child=event.edge.section('\n',-1);
            const auto current=sourceTfSchema.find(child),used=usedTfSchema.find(child);
            check(current==sourceTfSchema.end()||current->second==event.staticTransform,"One child frame changes static/dynamic TF type inside the source; separate these experiments.");
            check(used==usedTfSchema.end()||used->second==event.staticTransform,"A child frame changed static/dynamic TF type across sources. Noetic clear() retains its cache class; restart only the native backend before indexing this source.");
            sourceTfSchema[child]=event.staticTransform;
        }
        QJsonArray list;for(const auto &entry:topics){const auto &t=entry.second;list.append(QJsonObject{{"name",entry.first},{"type",t.type},{"count",QString::number(t.count)},{"begin_ns",t.count?QString::number(t.first):QString()},{"end_ns",QString::number(t.last)},{"explicitly_mapped",QString::number(t.mapped)}});}
        indexResult={{"time_basis",basis},{"untimed_policy",policy},{"topics",list},{"begin_ns",QString::number(events.front().stamp)},{"end_ns",QString::number(events.back().stamp)},{"events",QString::number(events.size())},{"skipped",QString::number(skipped)},{"elapsed_ms",elapsed.elapsed()},{"payloads_kept_in_memory",false}};
        valid=true;finish(true,indexResult);
    }
    void inject(const Event &event) {
        const auto key=qulonglong(&event-events.data());if(injected.contains(key))return;
        const auto data=event.message->instantiate<tf2_msgs::TFMessage>();check(data&&size_t(event.child)<data->transforms.size(),"Invalid indexed TF child.");
        check(manager->getTF2BufferPtr()->setTransform(data->transforms[size_t(event.child)],"uav_studio_indexed_bag",event.staticTransform),"Native TF buffer rejected recorded transform.");
        injected.insert(key);
    }
    void warmTransforms(uint64_t current) {
        while(tfCursor<transforms.size()&&events[transforms[tfCursor]].stamp<=current) {
            const auto &event=events[transforms[tfCursor++]];if(event.stamp<=target)inject(event);
        }
        // One real future bracketing sample per edge, never newer than the target.
        QSet<QString> edges;
        for(size_t i=tfCursor;i<transforms.size();++i){const auto &event=events[transforms[i]];if(event.stamp>target)break;if(!event.staticTransform&&!edges.contains(event.edge)){inject(event);edges.insert(event.edge);}}
    }
    void prepare() {
        for(const auto &entry:sourceTfSchema)usedTfSchema[entry.first]=entry.second;
        drain();manager->resetTime();manager->getTF2BufferPtr()->clear();injected.clear();
        advance(events.front().stamp);update();manager->resetTime();
        manager->getFrameManager()->setSyncMode(rviz::FrameManager::SyncExact);
        // Initial static state belongs to the beginning of history. Later static
        // replacements must take effect at their explicit mapped experiment time.
        std::map<QString,const Event *> statics;
        for(const auto &event:events){if(event.stamp>events.front().stamp)break;if(event.staticTransform)statics[event.edge]=&event;}
        for(const auto &entry:statics)inject(*entry.second);
        tfCursor=0;cursor=0;sent.clear();received.clear();lastHeader.clear();lastTime.clear();deliveredFrames.clear();
        publishers.clear();observers.clear();
        for(const auto &entry:topics) {
            if(!entry.second.count)continue;
            topic_tools::ShapeShifter shape;const auto &t=entry.second;
            shape.morph(t.md5.toStdString(),t.type.toStdString(),t.definition.toStdString(),t.latched?"1":"0");
            publishers.emplace(entry.first,shape.advertise(node,entry.first.toStdString(),32,t.latched));
            const auto topic=entry.first;
            observers.push_back(node.subscribe<topic_tools::ShapeShifter>(topic.toStdString(),32,[this,topic](const topic_tools::ShapeShifter::ConstPtr &){++received[topic];}));
        }
        if(!clock)clock=node.advertise<rosgraph_msgs::Clock>("/clock",1,true);
        stage.restart();mode=Prepare;
    }
    void finalizeTransforms() {
        // Load only the two recorded bracketing transforms into a separate native
        // tf2 sampler. Rebuild the render buffer with history <= target and the
        // resulting interpolation at target. A future raw sample never becomes
        // the render buffer's "latest" pose (TF Display also performs zero-time
        // relative lookups outside FrameManager).
        tfSamples={};tfCoverage=true;
        manager->getTF2BufferPtr()->clear();injected.clear();
        std::map<QString,const Event *> statics;
        std::map<QString,std::pair<const Event *,const Event *>> brackets;
        for(const auto i:transforms) {
            const auto &event=events[i];
            if(event.staticTransform){if(event.stamp<=target)statics[event.edge]=&event;continue;}
            auto &pair=brackets[event.edge];
            if(event.stamp<=target)pair.first=&event;else if(!pair.second)pair.second=&event;
        }
        for(const auto &entry:statics)inject(*entry.second);
        for(const auto i:transforms){const auto &event=events[i];if(!event.staticTransform&&event.stamp<=target)inject(event);}
        for(const auto &entry:brackets) {
            const auto *left=entry.second.first,*right=entry.second.second;
            QJsonObject sampled{{"edge",entry.first},{"target_ns",QString::number(target)},
                {"left_ns",left?QString::number(left->stamp):QString()},
                {"right_ns",right?QString::number(right->stamp):QString()}};
            if(!left||(!right&&left->stamp!=target)) {
                sampled.insert("available",false);sampled.insert("reason",left?"No recorded right bracket; extrapolation refused.":"No recorded left bracket; extrapolation refused.");tfCoverage=false;
            } else if(left->stamp==target) {sampled.insert("available",true);sampled.insert("method","exact-recorded-transform");}
            else {
                const auto a=left->message->instantiate<tf2_msgs::TFMessage>()->transforms[size_t(left->child)];
                const auto b=right->message->instantiate<tf2_msgs::TFMessage>()->transforms[size_t(right->child)];
                tf2::BufferCore sampler(ros::Duration(double(right->stamp-left->stamp)/1e9+1));
                check(sampler.setTransform(a,"recorded-left",false)&&sampler.setTransform(b,"recorded-right",false),"Native TF sampler rejected recorded brackets.");
                const auto value=sampler.lookupTransform(a.header.frame_id,a.child_frame_id,timeOf(target));
                check(manager->getTF2BufferPtr()->setTransform(value,"uav_studio_native_tf2_interpolation",false),"Native render TF buffer rejected sampled target transform.");
                sampled.insert("available",true);sampled.insert("method","native-tf2-linear-translation-quaternion-slerp");
                sampled.insert("translation",QJsonArray{value.transform.translation.x,value.transform.translation.y,value.transform.translation.z});
                sampled.insert("rotation_xyzw",QJsonArray{value.transform.rotation.x,value.transform.rotation.y,value.transform.rotation.z,value.transform.rotation.w});
            }
            tfSamples.append(sampled);
        }
        advance(target);manager->getFrameManager()->update();manager->getFrameManager()->syncTime(timeOf(target));
    }
    void publish(const Event &event) {
        const auto topic=QString::fromStdString(event.message->getTopic());auto &publisher=publishers.at(topic);
        // ROS coalesces subscriptions in one process into a single transport link;
        // getNumSubscribers() cannot distinguish our observer from native plugins.
        // Publish every explicitly indexed topic and verify the real native status.
        pendingTopic=topic;pendingCount=received[topic]+1;stage.restart();
        if(event.child>=0&&event.transform) {
            const auto source=event.message->instantiate<tf2_msgs::TFMessage>();auto data=boost::make_shared<tf2_msgs::TFMessage>();data->transforms.push_back(source->transforms[size_t(event.child)]);publisher.publish(data);
        } else if(event.child>=0) {
            const auto source=event.message->instantiate<visualization_msgs::MarkerArray>();auto data=boost::make_shared<visualization_msgs::MarkerArray>();data->markers.push_back(source->markers[size_t(event.child)]);publisher.publish(data);
        } else publisher.publish(*event.message);
        ++sent[topic];lastHeader[topic]=event.header;lastTime[topic]=event.stamp;
        if(!event.frame.isEmpty())deliveredFrames[topic][event.frame]=event.frameLocked?0:event.stamp;
    }
    QJsonObject readiness(bool includeCurrentView = true) const {
        if(!valid||!target)return {{"native_scene_ready",false},{"required_tf_complete",false}};
        QJsonArray frames,issues,subscriptions;bool requiredTf=tfCoverage;
        const auto fixed=manager->getFixedFrame();auto *frameManager=manager->getFrameManager();
        const auto requireFrame=[&](const QString &frame,const QString &path){
            std::string error;
            const bool available=!frame.isEmpty()&&!frameManager->frameHasProblems(frame.toStdString(),timeOf(target),error)
                &&!frameManager->transformHasProblems(frame.toStdString(),timeOf(target),error);
            requiredTf=requiredTf&&available;
            frames.append(QJsonObject{{"property",path},{"frame",frame},{"fixed_frame",fixed},
                {"target_ns",QString::number(target)},{"available",available},{"error",QString::fromStdString(error)}});
        };
        requireFrame(fixed,"Global Options / Fixed Frame");
        auto *root=manager->getDisplayTreeModel()->getRoot();
        std::function<void(rviz::Property *,bool,QString)> inspect=[&](rviz::Property *property,bool active,QString path){
            if(auto *display=qobject_cast<rviz::Display *>(property);display&&property!=root)active=active&&display->isEnabled();
            if(!active)return;
            path+=(path.isEmpty()?QString():" / ")+property->getName();
            if(auto *frame=qobject_cast<rviz::TfFrameProperty *>(property))
                if(!frame->getHidden()&&(frame->getViewFlags(1)&Qt::ItemIsEnabled))requireFrame(frame->getFrame(),path);
            if(auto *status=qobject_cast<rviz::StatusProperty *>(property))
                if(status->getLevel()!=rviz::StatusProperty::Ok)
                    issues.append(QJsonObject{{"property",path},{"source","native-StatusProperty"},
                        {"level",int(status->getLevel())},{"status",rviz::StatusProperty::statusWord(status->getLevel())},
                        {"message",status->getViewData(1,Qt::DisplayRole).toString()}});
            if(auto *topic=qobject_cast<rviz::RosTopicProperty *>(property)) {
                const auto configured=topic->getValue().toString().trimmed();
                if(!configured.isEmpty()) {
                    const auto name=QString::fromStdString(ros::names::resolve(configured.toStdString()));
                    const auto found=sent.find(name);const auto count=found==sent.end()?0:found->second;
                    subscriptions.append(QJsonObject{{"property",path},{"topic",name},{"published",QString::number(count)},{"available",count>0}});
                    if(!count)issues.append(QJsonObject{{"property",path},{"source","indexed-topic-delivery"},
                        {"topic",name},{"message","No selected message was delivered to this enabled Display topic at the target."}});
                    // MessageFilter can silently retain the first message without
                    // creating a native error Status. Validate actual delivered
                    // Header frame dependencies as well as the plugin's status.
                    const auto delivered=deliveredFrames.find(name);
                    if(delivered!=deliveredFrames.end())for(const auto &dependency:delivered->second) {
                        const auto stamp=dependency.second?dependency.second:target;std::string error;
                        const bool available=!frameManager->frameHasProblems(dependency.first.toStdString(),timeOf(stamp),error)
                            &&!frameManager->transformHasProblems(dependency.first.toStdString(),timeOf(stamp),error);
                        if(!available){requiredTf=false;issues.append(QJsonObject{{"property",path},{"source","delivered-message-header-TF"},
                            {"topic",name},{"frame",dependency.first},{"time_ns",QString::number(stamp)},
                            {"message",QString::fromStdString(error)}});}
                    }
                }
            }
            for(int i=0;i<property->numChildren();++i)inspect(property->childAt(i),active,path);
        };
        inspect(root,true,{});
        if(includeCurrentView)if(auto *view=manager->getViewManager()->getCurrent())inspect(view,true,"Current View");
        return {{"required_tf_complete",requiredTf},{"required_frames",frames},{"display_issues",issues},
            {"display_topics",subscriptions},{"native_scene_ready",requiredTf&&issues.isEmpty()},
            {"plugin_worker_completion_certified",false}};
    }
    void tick() {
        if(progress.elapsed()>250){notify({{"event","replay_progress"},{"phase",mode==Index?"index":"seek"},{"processed",QString::number(mode==Index?indexedMessages:cursor)},{"total",QString::number(mode==Index?totalMessages:end)}});progress.restart();}
        if(mode==Index) {
            QElapsedTimer budget;budget.start();
            while(*iterator!=view->end()&&budget.elapsed()<6){indexMessage(**iterator);++*iterator;++indexedMessages;}
            if(*iterator==view->end())indexedDone();return;
        }
        if(mode==Prepare) {
            drain();bool ready=true;for(const auto &entry:publishers)if(entry.second.getNumSubscribers()==0)ready=false;
            if(stage.elapsed()>=500&&ready){mode=Replay;stage.restart();return;}
            check(stage.elapsed()<5000,"Native ROS subscribers did not connect.");return;
        }
        if(mode==Replay) {
            drain();
            if(!pendingTopic.isEmpty()) {
                if(received[pendingTopic]<pendingCount){check(stage.elapsed()<5000,"Timed out draining a replayed ROS message.");return;}
                drain();update();pendingTopic.clear();
            }
            QElapsedTimer budget;budget.start();
            while(cursor<end&&budget.elapsed()<5) {
                const auto &event=events[cursor++];advance(event.stamp);warmTransforms(event.stamp);publish(event);
                if(!pendingTopic.isEmpty())return;
            }
            // Install the exact interpolated TF before updating frame-locked
            // plugins at the target. Updating first leaves a real native Marker
            // transform error that RViz may retain even after TF becomes valid.
            if(cursor==end){advance(target);warmTransforms(target);finalizeTransforms();drain();update();rosgraph_msgs::Clock value;value.clock=timeOf(target);clock.publish(value);stage.restart();settled=0;mode=Settle;}
            return;
        }
        if(mode==Settle) {
            advance(target);drain();update();
            bool empty=true;for(auto *queue:{manager->getUpdateQueue(),manager->getThreadedQueue()})if(auto *q=dynamic_cast<ros::CallbackQueue *>(queue))empty=empty&&q->isEmpty();
            settled=empty?settled+1:0;
            if(stage.elapsed()<(incremental?0:100)||settled<3){check(stage.elapsed()<5000,"Native callbacks did not settle after replay.");return;}
            QJsonArray list;for(const auto &entry:sent)list.append(QJsonObject{{"name",entry.first},{"published",QString::number(entry.second)},{"observer_received",QString::number(received[entry.first])},{"last_header_ns",QString::number(lastHeader[entry.first])},{"last_experiment_ns",QString::number(lastTime[entry.first])}});
            const auto ready=readiness(),dataReady=readiness(false);synced=tfCoverage;
            finish(true,{{"target_ns",QString::number(target)},{"clock_ns",QString::number(ros::Time::now().toNSec())},{"time_basis",basis},{"untimed_policy",policy},{"history_from_beginning",true},
                {"replay_mode",incremental?"incremental-forward":"full-history-restore"},{"native_scene_reset",!incremental},{"events_delivered_this_request",QString::number(end-startedCursor)},
                {"history_events",QString::number(end)},{"topics",list},{"tf_coverage_complete",tfCoverage},{"tf_target_samples",tfSamples},{"future_raw_tf_in_render_buffer",false},{"elapsed_ms",elapsed.elapsed()},{"native_queues_drained",true},{"required_tf_complete",ready.value("required_tf_complete")},{"native_scene_ready",ready.value("native_scene_ready")},{"native_data_ready",dataReady.value("native_scene_ready")},{"native_readiness",ready},{"plugin_worker_completion_certified",false},{"realtime_playback_certified",false}});
        }
    }
};
NativeBagReplay::NativeBagReplay(rviz::VisualizationManager *manager,std::function<void(QJsonObject)> event,QObject *parent):QObject(parent),d(std::make_unique<Private>(this,manager,std::move(event))){}
NativeBagReplay::~NativeBagReplay()=default;
bool NativeBagReplay::busy()const{return d->mode!=Private::Idle;}
bool NativeBagReplay::indexed()const{return d->valid;}
bool NativeBagReplay::synchronized()const{return d->synced&&ros::Time::now().toNSec()==d->target&&d->readiness().value("native_scene_ready").toBool();}
QString NativeBagReplay::targetNs()const{return synchronized()?QString::number(d->target):QString();}
bool NativeBagReplay::dataSynchronized()const{return d->synced&&ros::Time::now().toNSec()==d->target&&d->readiness(false).value("native_scene_ready").toBool();}
QString NativeBagReplay::dataTargetNs()const{return dataSynchronized()?QString::number(d->target):QString();}
QJsonObject NativeBagReplay::readiness(bool includeCurrentView)const{return d->readiness(includeCurrentView);}
QJsonObject NativeBagReplay::resources()const {
    return {{"indexed",d->valid},{"busy",busy()},{"bag_open",bool(d->bag)},
        {"history_events",QString::number(d->events.size())},{"owned_topic_publishers",int(d->publishers.size())},
        {"owned_observers",int(d->observers.size())},{"clock_publisher",bool(d->clock)},
        {"selected_topics",int(d->selectedTopics.size())},{"delivered_topics",int(d->sent.size())}};
}
void NativeBagReplay::invalidate(){cancel("Native source or configuration changed.");d->valid=d->synced=false;d->events.clear();d->iterator.reset();d->view.reset();d->bag.reset();d->publishers.clear();d->observers.clear();}
void NativeBagReplay::cancel(const QString &reason){if(busy())d->finish(false,{},reason);d->synced=false;}
void NativeBagReplay::index(const QJsonObject &r,Reply reply) {
    check(!busy(),"A native replay job is already active.");invalidate();
    d->basis=r.value("time_basis").toString();d->policy=r.value("untimed_policy").toString("reject");
    check(d->basis=="header"||d->basis=="record","Choose header or record time explicitly.");
    check(d->policy=="reject"||d->policy=="skip"||d->policy=="map_record","Unknown untimed policy.");
    d->offset=0;if(d->policy=="map_record")d->offset=int64_t(ns(r.value("anchor_header_ns")))-int64_t(ns(r.value("anchor_record_ns")));
    d->path=r.value("file").toString();d->selectedTopics.clear();for(const auto &t:r.value("topics").toArray())d->selectedTopics.insert(t.toString());
    d->bag=std::make_unique<rosbag::Bag>(d->path.toStdString(),rosbag::bagmode::Read);d->view=std::make_unique<rosbag::View>(*d->bag);check(d->view->size()>0,"Bag has no messages.");
    d->iterator=std::make_unique<rosbag::View::iterator>(d->view->begin());d->topics.clear();d->indexedMessages=d->skipped=0;d->totalMessages=d->view->size();d->reply=std::move(reply);d->mode=Private::Index;d->elapsed.start();d->progress.start();d->timer.start();
}
void NativeBagReplay::seek(const QJsonObject &r,Reply reply) {
    check(!busy()&&indexed(),"Index a bag before seeking; a native replay job may already be active.");
    bool simulated=false;ros::param::get("/use_sim_time",simulated);check(simulated,"Indexed replay requires an owned simulated-time ROS master.");
    const auto target=ns(r.value("target_ns"));check(target>=d->events.front().stamp&&target<=d->events.back().stamp,"Target is outside this native experiment index.");d->target=target;
    d->end=size_t(std::upper_bound(d->events.begin(),d->events.end(),d->target,[](uint64_t n,const Private::Event &e){return n<e.stamp;})-d->events.begin());
    d->incremental=false;d->startedCursor=0;d->synced=false;d->manager->stopUpdate();d->updatesStopped=true;
    d->elapsed.start();d->progress.start();d->reply=std::move(reply);
    try {d->prepare();d->timer.start();}
    catch(const std::exception &error){d->finish(false,{},QString::fromUtf8(error.what()));}
}
void NativeBagReplay::advance(const QJsonObject &r,Reply reply) {
    check(!busy()&&indexed()&&synchronized(),"Incremental advance requires a completed synchronized native scene; use a full seek first.");
    const auto target=ns(r.value("target_ns"));check(target>=d->target&&target<=d->events.back().stamp,"Incremental advance only accepts a nondecreasing in-range experiment target.");
    d->target=target;d->end=size_t(std::upper_bound(d->events.begin(),d->events.end(),target,[](uint64_t n,const Private::Event &event){return n<event.stamp;})-d->events.begin());
    d->incremental=true;d->startedCursor=d->cursor;d->synced=false;d->manager->stopUpdate();d->updatesStopped=true;
    d->elapsed.start();d->stage.start();d->progress.start();d->reply=std::move(reply);d->mode=Private::Replay;d->timer.start();
}

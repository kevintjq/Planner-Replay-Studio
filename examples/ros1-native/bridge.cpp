#include "bridge_wire.h"

#include <QCoreApplication>
#include <QCoreApplication>
#include <QHostAddress>
#include <QTcpSocket>
#include <ros/ros.h>
#include <topic_tools/shape_shifter.h>
#include <array>
#include <utility>
#include <vector>

namespace {
bool send(QTcpSocket &socket, const QByteArray &message)
{
    if (socket.write(message) != message.size())
        return false;
    while (socket.bytesToWrite() > 0) {
        if (!socket.waitForBytesWritten(3000))
            return false;
    }
    return true;
}
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "planner_replay_bridge");
    QCoreApplication application(argc, argv);
    QCoreApplication application(argc, argv);
    ros::NodeHandle node, parameters("~");
    std::string cloud, odometry;
    int port = 0;
    parameters.param<std::string>("cloud_topic", cloud, "/cloud_registered");
    parameters.param<std::string>("odom_topic", odometry, "/Odometry");
    parameters.param("port", port, 8765);
    if (port < 1 || port > 65535) {
        ROS_ERROR("Invalid bridge port.");
        return 2;
    }
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, quint16(port));
    if (!socket.waitForConnected(10'000) || !send(socket, replay::bridge::Magic)) {
        ROS_ERROR_STREAM("Could not connect to Planner Replay: " << socket.errorString().toStdString());
        return 1;
    }
    const std::array<std::string, 4> topics{cloud, odometry, "/tf", "/tf_static"};
    std::vector<ros::Subscriber> subscriptions;
    subscriptions.reserve(topics.size());
    for (const auto &topic : topics) {
        const auto resolved = node.resolveName(topic);
        subscriptions.push_back(node.subscribe<topic_tools::ShapeShifter>(resolved, 8,
            [&, resolved](const topic_tools::ShapeShifter::ConstPtr &message) {
                try {
                    const auto size = message->size();
                    if (size > replay::bridge::MaxMessage)
                        throw std::runtime_error("Serialized message exceeds 32 MiB.");
                    QByteArray payload(int(size), '\0');
                    ros::serialization::OStream bytes(reinterpret_cast<uint8_t *>(payload.data()), size);
                    message->write(bytes);
                    const auto packet = replay::bridge::encode({ros::Time::now().toNSec(), QByteArray::fromStdString(resolved),
                        QByteArray::fromStdString(message->getDataType()), QByteArray::fromStdString(message->getMessageDefinition()),
                        std::move(payload)});
                    if (!send(socket, packet)) {
                        ROS_INFO("Planner Replay stopped receiving.");
                        ros::shutdown();
                    }
                } catch (const std::exception &error) {
                    ROS_WARN_STREAM_THROTTLE(2., error.what());
                }
            }));
    }
    ros::spin();
    return 0;
}

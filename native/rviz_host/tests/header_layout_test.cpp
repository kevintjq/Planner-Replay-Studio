#include "../header_layout.h"
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/PoseStamped.h>
#include <nav_msgs/Path.h>
#include <sensor_msgs/Image.h>
#include <std_msgs/Header.h>
#include <tf2_msgs/TFMessage.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
void expect(bool actual, bool expected, const char *name)
{
    ++checks;
    if (actual != expected) throw std::runtime_error(name);
}
template<class Message> void actualDefinition(bool expected, const char *name)
{
    expect(native_replay::hasLeadingHeader(
        QString::fromLatin1(ros::message_traits::DataType<Message>::value()),
        QString::fromLatin1(ros::message_traits::Definition<Message>::value())), expected, name);
}
}

int main()
{
    try {
        actualDefinition<std_msgs::Header>(true, "Header itself");
        actualDefinition<visualization_msgs::Marker>(true, "Actual Marker constants before Header");
        actualDefinition<geometry_msgs::PoseStamped>(true, "Actual PoseStamped leading Header");
        actualDefinition<nav_msgs::Path>(true, "Actual Path leading Header");
        actualDefinition<sensor_msgs::Image>(true, "Actual Image leading Header");
        actualDefinition<geometry_msgs::Pose>(false, "Actual Pose has no Header");
        actualDefinition<tf2_msgs::TFMessage>(false, "TFMessage only has nested Headers");
        actualDefinition<visualization_msgs::MarkerArray>(false, "MarkerArray only has nested Headers");
        expect(native_replay::hasLeadingHeader("custom/Stamped",
            "# comment\r\nuint8 FLAG=1 # ignored\r\nstring LABEL = example\r\nstd_msgs/Header header # stamp\r\n"),
            true, "Comments, CRLF, qualified Header, leading constants");
        expect(native_replay::hasLeadingHeader("custom/LateHeader", "uint8 seq\nHeader header\n"),
            false, "Real serialized field before Header must reject");
        expect(native_replay::hasLeadingHeader("custom/Nested", "geometry_msgs/Pose pose\nHeader header\n"),
            false, "Nested object before Header must reject");
        expect(native_replay::hasLeadingHeader("custom/OnlyConstants", "uint8 FLAG=1\n"),
            false, "Only constants have no payload Header");
        expect(native_replay::hasLeadingHeader("custom/Empty", "# nothing\n\n"),
            false, "Empty definition must reject");
        expect(native_replay::hasLeadingHeader("custom/Array", "Header[] headers\n"),
            false, "Header array has length prefix and is not a leading Header");
        expect(native_replay::hasLeadingHeader("custom/EmptyRoot", "uint8 FLAG=1\n====\nMSG: std_msgs/Header\nHeader header\n"),
            false, "Do not cross nested definition boundary after constants");
        expect(native_replay::hasLeadingHeader("custom/InvalidConstant", "geometry_msgs/Pose ignored=anything\nHeader header\n"),
            false, "Message fields cannot be skipped as primitive constants");
        std::cout << "Passed " << checks << " leading-Header layout checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}

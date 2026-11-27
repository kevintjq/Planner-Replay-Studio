#pragma once

#include <QImage>
#include <QString>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <functional>
#include <optional>

namespace replay {

QImage imageOf(const cv::Mat &mat);

class VideoReader {
public:
    explicit VideoReader(const QString &file);
    bool advance();
    bool jump(int index);
    bool select(double time, const std::function<bool()> &cancel = {});
    int index() const { return index_; }
    double duration() const;

    cv::Mat frame;
    double seconds = 0.;

private:
    struct Frame {
        cv::Mat image;
        double seconds;
        int index;
    };
    std::optional<Frame> decode();
    void use(Frame image);

    cv::VideoCapture video_;
    std::optional<Frame> next_;
    double fps_ = 30.;
    int index_ = -1;
    bool exhausted_ = false;
};

}

#pragma once

#include <QImage>
#include <QString>
#include <QSize>
#include <opencv2/core.hpp>
#include <functional>
#include <memory>
#include <optional>

namespace replay {

QImage imageOf(const cv::Mat &mat);

class VideoReader {
public:
    explicit VideoReader(const QString &file, const std::function<bool()> &cancel = {});
    ~VideoReader();
    VideoReader(const VideoReader &) = delete;
    VideoReader &operator=(const VideoReader &) = delete;
    bool advance();
    bool jump(int index);
    bool select(double time, const std::function<bool()> &cancel = {});
    int index() const { return index_; }
    // Ordinal is unknown (-1) after an unindexed seek; PTS remains authoritative.
    qint64 pts() const { return pts_; }
    qint64 originPts() const;
    int timeBaseNumerator() const;
    int timeBaseDenominator() const;
    qint64 timestampNs() const;
    bool hasSourcePts() const { return sourcePts_; }
    bool hasAudio() const;
    // Decoded source pixels are normalized to square pixels before display rotation.
    QSize codedSize() const { return codedSize_; }
    int sampleAspectNumerator() const { return sampleAspectNumerator_; }
    int sampleAspectDenominator() const { return sampleAspectDenominator_; }
    int displayRotation() const;
    bool selectNanoseconds(qint64 time, const std::function<bool()> &cancel = {});
    double duration() const;

    cv::Mat frame;
    double seconds = 0.;

private:
    struct Frame {
        cv::Mat image;
        double seconds;
        int index;
        qint64 pts;
        qint64 duration;
        bool sourcePts;
        QSize codedSize;
        int sampleAspectNumerator, sampleAspectDenominator;
    };
    std::optional<Frame> decode();
    void use(Frame image);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::optional<Frame> next_;
    int index_ = -1;
    qint64 pts_ = 0;
    qint64 frameDuration_ = 0;
    bool sourcePts_ = false;
    QSize codedSize_;
    int sampleAspectNumerator_ = 1, sampleAspectDenominator_ = 1;
};

}

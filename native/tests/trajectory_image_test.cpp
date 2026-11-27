#include "trajectory_image.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "Could not create fixture directory");
        const auto videoPath = directory.filePath("stationary.avi");

        cv::VideoWriter writer(videoPath.toStdString(), cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                               12., {240, 160});
        require(writer.isOpened(), "Could not create the moving-target video");
        cv::Mat target(20, 26, CV_8UC3);
        cv::RNG random(4);
        random.fill(target, cv::RNG::UNIFORM, 90, 245);
        for (int index = 0; index < 23; ++index) {
            cv::Mat image = cv::Mat::zeros(160, 240, CV_8UC3);
            target.copyTo(image(cv::Rect(32 + 3 * index, 68, 26, 20)));
            writer.write(image);
        }
        writer.release();

        const auto first = replay::readTrajectoryFrame(videoPath, .25);
        require(first.size() == QSize(240, 160) && first.pixelColor(50, 79).red() > 70,
                "Frame preview did not honor the source time");
        const auto track = replay::trackTrajectory(videoPath, .25, 1.25, {41./240, 68./160, 26./240, 20./160});
        require(track.frames.size() == 13 && !track.lostAt, "Tracking did not honor the selected time interval");
        require(std::abs(track.frames.front().seconds - .25) < 1e-6 &&
                std::abs(track.frames.back().seconds - 1.25) < 1e-6 &&
                std::abs(track.frames.back().bounds.center().x() - 90) <= 6,
                "Tracking did not find the end of the trajectory");
        const auto sampled = replay::sampleTrajectory(track, .25);
        require(sampled == std::vector<size_t>({0, 3, 6, 9, 12}), "Sampling did not honor the video timestamps");

        const auto rendered = replay::renderTrajectory(track, {0, 12}, 1., {});
        const auto edited = replay::renderTrajectory(track, {0, 12}, .5, {0});
        require(rendered.size() == QSize(240, 160) && edited.size() == rendered.size(), "PNG uses the wrong source resolution");
        require(rendered.pixelColor(47, 77).red() > 70 && edited.pixelColor(47, 77).red() < 20,
                "Removing the first frame left the original aircraft visible");
        require(rendered.pixelColor(90, 77).red() > 55 &&
                edited.pixelColor(90, 77).red() < rendered.pixelColor(90, 77).red() &&
                edited.pixelColor(90, 77).red() > 20,
                "Opacity did not change the retained frame");
        require(edited.save(directory.filePath("trajectory.png")), "Could not encode the original-resolution PNG");
        std::cout << "Tracked 13 frames and verified sampling, removal and opacity.\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

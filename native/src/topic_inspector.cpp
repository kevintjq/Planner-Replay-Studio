#include "topic_inspector.h"

#include "replay_controller.h"
#include "trajectory_controller.h"

#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <exception>

TopicInspector::TopicInspector(ReplayController &controller, PreviewImages &images)
    : replay_(controller), images_(images), current_(controller.recording())
{
    connect(&replay_, &ReplayController::recordingChanged, this, [this] {
        const auto latest = replay_.recording();
        if (current_ == latest)
            return;
        current_ = latest;
        cancelled_ = true;
        topic_.clear();
        page_ = 0;
        pageCount_ = 1;
        status_.clear();
        fields_.clear();
        hasImage_ = false;
        emit changed();
    });
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        working_ = false;
        if (!cancelled_) {
            const auto result = watcher_.result();
            if (result.message) {
                const auto &item = *result.message;
                const auto relative = double(item.stamp - current_->recordedOrigin) / 1.e9;
                status_ = tr("%1 · %2 s").arg(item.type, QString::number(relative, 'f', 3));
                fields_ = item.fields;
                page_ = pendingPage_;
                pageCount_ = std::max(pageCount_, item.arrayPageCount);
                hasImage_ = !item.image.isNull();
                if (hasImage_) {
                    images_.setImage(item.image);
                    ++revision_;
                }
            } else {
                status_ = result.error.isEmpty() ? tr("播放位置之前没有这一路消息。") : result.error;
                fields_.clear();
                page_ = 0;
                pageCount_ = 1;
                hasImage_ = false;
            }
        }
        emit changed();
    });
}

TopicInspector::~TopicInspector()
{
    cancelled_ = true;
    watcher_.waitForFinished();
}

QStringList TopicInspector::topics() const
{
    return current_->topics;
}

QUrl TopicInspector::preview() const
{
    return hasImage_ ? QUrl(QStringLiteral("image://ros-inspector/") + QString::number(revision_)) : QUrl();
}

void TopicInspector::inspect(const QString &topic)
{
    if (working_ || replay_.loading() || !current_->topics.contains(topic) || replay_.bagPath().isEmpty())
        return;
    topic_ = topic;
    stamp_ = current_->recordedOrigin + qint64(std::llround(replay_.position() * 1.e9));
    page_ = 0;
    pageCount_ = 1;
    fields_.clear();
    hasImage_ = false;
    read(0);
}

void TopicInspector::setArrayPage(int page)
{
    if (!working_ && !replay_.loading() && !topic_.isEmpty() && page >= 0 && page < pageCount_ && page != page_)
        read(page);
}

void TopicInspector::read(int page)
{
    const auto path = replay_.bagPath();
    pendingPage_ = page;
    cancelled_ = false;
    working_ = true;
    status_ = tr("正在读取录包消息…");
    emit changed();
    watcher_.setFuture(QtConcurrent::run([this, path, topic = topic_, stamp = stamp_, page]() -> Result {
        try {
            return {replay::inspectBagTopic(path, topic, stamp, [this] { return cancelled_.load(); }, page), {}};
        } catch (const std::exception &error) {
            return {{}, QString::fromUtf8(error.what())};
        }
    }));
}

void TopicInspector::cancel()
{
    if (working_) {
        cancelled_ = true;
        status_ = tr("已取消。");
        emit changed();
    }
}

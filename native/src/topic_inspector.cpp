#include "topic_inspector.h"

#include "replay_controller.h"
#include "trajectory_controller.h"

#include <QFileInfo>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace {
QString normalizedPath(const QUrl &url)
{
    if (!url.isLocalFile()) return {};
    const QFileInfo info(url.toLocalFile());
    return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
}
}

TopicInspector::TopicInspector(ReplayController &controller, PreviewImages &images)
    : replay_(controller), images_(images), current_(controller.recording())
{
    connect(&replay_, &ReplayController::recordingChanged, this, &TopicInspector::refreshRecording);
    connect(&replay_, &ReplayController::stateChanged, this, [this] {
        if (replay_.loading() || pendingOpenPath_.isEmpty()) return;
        const auto path = std::exchange(pendingOpenPath_, {});
        if (normalizedPath(QUrl::fromLocalFile(replay_.bagPath())) == path && !current_->topicCatalog.empty())
            setCatalog(current_->topicCatalog, path);
        else loadCatalog(QUrl::fromLocalFile(path));
    });
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        working_ = false;
        const auto result = watcher_.result();
        if (!cancelled_ && result.generation == generation_) {
            if (result.message) {
                message_ = *result.message;
                hasMessage_ = true;
                status_ = tr("%1 · Bag storage %2 ns").arg(message_.type, QString::number(message_.stamp));
                fields_ = message_.fields;
                page_ = pendingPage_;
                pageCount_ = std::max(pageCount_, message_.arrayPageCount);
                hasImage_ = !message_.image.isNull();
                if (hasImage_) { images_.setImage(message_.image); ++revision_; }
            } else {
                clearMessage();
                status_ = result.error.isEmpty() ? tr("播放位置之前没有这一路消息。") : result.error;
            }
        }
        emit changed();
    });
    connect(&catalogWatcher_, &QFutureWatcher<CatalogResult>::finished, this, [this] {
        catalogWorking_ = false;
        const auto result = catalogWatcher_.result();
        if (!catalogCancelled_ && result.generation == catalogGeneration_) {
            if (result.error.isEmpty()) setCatalog(result.entries, result.path);
            else { catalogStatus_ = result.error; emit catalogChanged(); }
        }
        if (!pendingCatalogPath_.isEmpty()) startCatalog();
        else emit catalogChanged();
    });
    connect(&samplingWatcher_, &QFutureWatcher<SamplingResult>::finished, this, [this] {
        samplingWorking_ = false;
        const auto result = samplingWatcher_.result();
        if (!samplingCancelled_ && result.generation == samplingGeneration_) {
            series_ = result.series;
            samplingStatus_ = result.error.isEmpty() ? series_.status : result.error;
        }
        emit samplingChanged();
    });
    if (!current_->topicCatalog.empty()) setCatalog(current_->topicCatalog, replay_.bagPath());
}

TopicInspector::~TopicInspector()
{
    cancelled_ = true; catalogCancelled_ = true; samplingCancelled_ = true;
    watcher_.waitForFinished(); catalogWatcher_.waitForFinished(); samplingWatcher_.waitForFinished();
}

void TopicInspector::clearMessage()
{
    message_ = {}; hasMessage_ = false; fields_.clear(); hasImage_ = false;
    page_ = 0; pageCount_ = 1;
}

void TopicInspector::refreshRecording()
{
    const auto latest = replay_.recording();
    if (current_ == latest) return;
    current_ = latest;
    cancelled_ = true; samplingCancelled_ = true;
    ++generation_; ++samplingGeneration_;
    topic_.clear(); type_.clear(); clearMessage(); status_.clear(); series_ = {};
    samplingStatus_.clear();
    if (!current_->topicCatalog.empty()) { setCatalog(current_->topicCatalog, replay_.bagPath()); pendingOpenPath_.clear(); }
    else if (pendingOpenPath_.isEmpty() && replay_.bagPath().isEmpty()) setCatalog({}, {});
    emit changed(); emit samplingChanged();
}

QStringList TopicInspector::topics() const
{
    QStringList result;
    for (const auto &value : catalog_) result.append(value.toMap().value("name").toString());
    if (catalog_.isEmpty() && catalogPath_.isEmpty() && current_) result = current_->topics;
    result.removeDuplicates(); result.sort();
    return result;
}

QString TopicInspector::sourcePath() const
{
    return catalogPath_.isEmpty() ? replay_.bagPath() : catalogPath_;
}

void TopicInspector::setCatalog(const std::vector<replay::TopicCatalogEntry> &entries, const QString &path)
{
    catalogCancelled_ = true; ++catalogGeneration_; pendingCatalogPath_.clear();
    catalogPath_ = path; catalog_.clear(); types_.clear();
    for (const auto &entry : entries) { catalog_.append(entry.toVariant()); types_.append(entry.type); }
    types_.removeDuplicates(); types_.sort();
    catalogStatus_ = entries.empty() ? tr("尚未索引 Bag 数据源。") :
        tr("%1 Topic/type · 存储时间统计；未过滤消息类型").arg(entries.size());
    emit catalogChanged(); emit changed();
}

void TopicInspector::openBag(const QUrl &url)
{
    const auto path = normalizedPath(url);
    if (path.isEmpty() || replay_.loading()) return;
    pendingOpenPath_ = path;
    cancelled_ = true; samplingCancelled_ = true; catalogCancelled_ = true;
    ++generation_; ++samplingGeneration_; ++catalogGeneration_;
    pendingCatalogPath_.clear(); clearMessage();
    replay_.openBag(url);
}

void TopicInspector::loadCatalog(const QUrl &url)
{
    const auto path = normalizedPath(url);
    if (path.isEmpty()) { catalogStatus_ = tr("请选择本地 ROS1 Bag 文件。"); emit catalogChanged(); return; }
    cancelled_ = true; samplingCancelled_ = true; catalogCancelled_ = true;
    ++generation_; ++samplingGeneration_; ++catalogGeneration_;
    topic_.clear(); type_.clear(); clearMessage(); series_ = {}; samplingStatus_.clear();
    pendingCatalogPath_ = path;
    catalog_.clear(); types_.clear(); catalogPath_ = path;
    catalogStatus_ = tr("正在后台索引全部 Topic…");
    emit changed(); emit catalogChanged(); emit samplingChanged();
    if (!catalogWorking_) startCatalog();
}

void TopicInspector::startCatalog()
{
    const auto path = std::exchange(pendingCatalogPath_, {});
    if (path.isEmpty()) return;
    catalogCancelled_ = false; catalogWorking_ = true;
    const auto generation = catalogGeneration_;
    emit catalogChanged();
    catalogWatcher_.setFuture(QtConcurrent::run([this, path, generation]() -> CatalogResult {
        try { return {replay::inspectBagCatalog(path, [this] { return catalogCancelled_.load(); }), {}, path, generation}; }
        catch (const std::exception &error) { return {{}, QString::fromUtf8(error.what()), path, generation}; }
    }));
}

QUrl TopicInspector::preview() const
{
    return hasImage_ ? QUrl(QStringLiteral("image://ros-inspector/") + QString::number(revision_)) : QUrl();
}

void TopicInspector::inspect(const QString &topic, const QString &type)
{
    qint64 stamp = std::numeric_limits<qint64>::max();
    if (normalizedPath(QUrl::fromLocalFile(replay_.bagPath())) == normalizedPath(QUrl::fromLocalFile(sourcePath()))) {
        if (current_->timelineTimeBasis == "header-with-storage-fallback") {
            status_ = tr("当前回放混合 Header 与存储时间，不能推断精确消息。请用存储时间纳秒显式查询。"); emit changed(); return;
        }
        stamp = current_->stampAt(replay_.position());
        if (current_->timelineTimeBasis == "header") {
            if (working_ || replay_.loading() || !topics().contains(topic)) return;
            topic_ = topic; type_ = type; stamp_ = stamp; selectionBasis_ = "header";
            clearMessage(); read(0); return;
        }
    }
    inspectAt(topic, QString::number(stamp), type);
}

void TopicInspector::inspectAt(const QString &topic, const QString &storageStampNs, const QString &type)
{
    bool ok = false;
    const auto stamp = storageStampNs.toLongLong(&ok);
    if (working_ || replay_.loading() || !topics().contains(topic) || sourcePath().isEmpty()) return;
    if (!ok || stamp < 0) { status_ = tr("消息查询时间必须是非负整数纳秒。"); emit changed(); return; }
    topic_ = topic; type_ = type; stamp_ = stamp; selectionBasis_ = "storage";
    clearMessage(); read(0);
}

void TopicInspector::setArrayPage(int page)
{
    if (!working_ && !replay_.loading() && !topic_.isEmpty() && page >= 0 && page < pageCount_ && page != page_) read(page);
}

void TopicInspector::read(int page)
{
    const auto path = sourcePath();
    pendingPage_ = page; cancelled_ = false; working_ = true;
    const auto generation = ++generation_;
    status_ = tr("正在读取录包消息…"); emit changed();
    watcher_.setFuture(QtConcurrent::run([this, path, topic = topic_, type = type_, stamp = stamp_, basis = selectionBasis_, page, generation]() -> Result {
        try { return {replay::inspectBagTopic(path, topic, stamp, [this] { return cancelled_.load(); }, page, type, basis), {}, generation}; }
        catch (const std::exception &error) { return {{}, QString::fromUtf8(error.what()), generation}; }
    }));
}

void TopicInspector::sampleNumeric(const QString &topic, const QString &fieldPath, const QString &timeBasis,
                                  const QString &firstStampNs, const QString &lastStampNs, int maxSamples, const QString &type)
{
    if (samplingWorking_ || sourcePath().isEmpty()) return;
    const auto path = sourcePath();
    samplingCancelled_ = false; samplingWorking_ = true;
    const auto generation = ++samplingGeneration_;
    series_ = {}; samplingStatus_ = tr("正在后台读取选中 Topic 的数值字段…"); emit samplingChanged();
    samplingWatcher_.setFuture(QtConcurrent::run([this, path, topic, fieldPath, timeBasis, firstStampNs, lastStampNs, maxSamples, type, generation]() -> SamplingResult {
        try { return {replay::sampleBagTopic(path, topic, fieldPath, timeBasis, firstStampNs, lastStampNs, maxSamples, type,
                         [this] { return samplingCancelled_.load(); }), {}, generation}; }
        catch (const std::exception &error) { return {{}, QString::fromUtf8(error.what()), generation}; }
    }));
}

void TopicInspector::cancel()
{
    cancelled_ = true; catalogCancelled_ = true; samplingCancelled_ = true;
    ++generation_; ++catalogGeneration_; ++samplingGeneration_; pendingCatalogPath_.clear(); pendingOpenPath_.clear();
    status_ = tr("已取消。"); samplingStatus_ = status_; catalogStatus_ = status_;
    emit changed(); emit catalogChanged(); emit samplingChanged();
}

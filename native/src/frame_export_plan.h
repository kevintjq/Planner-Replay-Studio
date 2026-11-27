#pragma once
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace replay {
struct FrameExportJob { double time; QString viewId, fileName; };
// 只生成可审查的任务列表，不读素材、不改变播放头、不创建文件。
QVector<FrameExportJob> planFrameExport(double duration, double currentTime,
    const QVariantList &markers, const QVariantList &views, const QVariantMap &settings);
}

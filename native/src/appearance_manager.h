#pragma once

#include <QObject>
#include <QPalette>
#include <QSettings>
#include <QVariantMap>
#include <memory>

class AppearanceManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)
    Q_PROPERTY(QVariantMap metrics READ metrics CONSTANT)
    Q_PROPERTY(QPalette palette READ palette NOTIFY changed)
public:
    enum Mode { Light, Dark, System };
    Q_ENUM(Mode)
    explicit AppearanceManager(QObject *parent = nullptr, const QString &settingsFile = {});
    int mode() const { return mode_; }
    bool dark() const;
    void setMode(int mode);
    QVariantMap colors() const;
    QVariantMap metrics() const;
    QPalette palette() const;
    // 合成内容使用独立配色，不能因切换软件外观而改变最终视频或图片。
    Q_INVOKABLE QVariantMap outputColors(const QString &scheme = QStringLiteral("dark")) const;
    static QVariantMap designColors(bool dark);
signals:
    void changed();
private:
    void apply();
    Mode mode_ = System;
    std::unique_ptr<QSettings> settings_;
};

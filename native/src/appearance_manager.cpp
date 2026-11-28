#include "appearance_manager.h"
#include "design_tokens.h"

#include <QGuiApplication>
#include <QStyleHints>

AppearanceManager::AppearanceManager(QObject *parent, const QString &settingsFile) : QObject(parent)
{
    settings_ = settingsFile.isEmpty() ? std::make_unique<QSettings>()
        : std::make_unique<QSettings>(settingsFile, QSettings::IniFormat);
    if (settingsFile.isEmpty() && !settings_->contains("appearance/mode")) {
        // 品牌迁移只复制旧偏好，不删除旧配置，旧版本仍可回退运行。
        QSettings legacy("Planner Replay", "Planner Replay Native");
        for (const auto &key : legacy.allKeys())
            if (!settings_->contains(key)) settings_->setValue(key, legacy.value(key));
        settings_->sync();
    }
    bool validMode = false;
    const auto mode = settings_->value("appearance/mode", int(System)).toInt(&validMode);
    if (validMode && mode >= Light && mode <= System)
        mode_ = Mode(mode);
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (mode_ == System) apply();
    });
    apply();
}

bool AppearanceManager::dark() const
{
    return mode_ == Dark || (mode_ == System && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
}

void AppearanceManager::setMode(int mode)
{
    if (mode < Light || mode > System || mode == mode_)
        return;
    mode_ = Mode(mode);
    settings_->setValue("appearance/mode", mode);
    settings_->sync();
    apply();
}

void AppearanceManager::apply()
{
    // 只更新调色板与绑定；不触及 ROS、时间轴、视频解码器或 OpenGL 资源。
    QGuiApplication::setPalette(palette());
    emit changed();
}

QVariantMap AppearanceManager::designColors(bool dark)
{
    // 保留 SVG 的布局与层级，按最新实测反馈提升文字、输入边界和选中状态对比度。
    QVariantMap colors{
        {"window", dark ? "#1E2229" : "#F2F4F7"},
        {"panel", dark ? "#252931" : "#FFFFFF"},
        {"control", dark ? "#363D48" : "#E9EDF2"},
        {"hover", dark ? "#424750" : "#DCE1E9"},
        {"pressed", dark ? "#4C535F" : "#D0D7E1"},
        {"input", dark ? "#363D48" : "#E9EDF2"},
        {"text", dark ? "#F1F3F7" : "#202630"},
        {"secondaryText", dark ? "#BFC8D5" : "#596579"},
        {"disabledText", dark ? "#777E89" : "#9AA2AF"},
        {"accent", dark ? "#82B4F5" : "#2868BC"},
        {"accentFill", dark ? "#35659F" : "#2868BC"},
        {"accentText", "#FFFFFF"},
        {"error", dark ? "#FFA69B" : "#A63B32"},
        {"selection", dark ? "#364F6E" : "#DCE9FA"},
        {"border", dark ? "#7F8C9D" : "#768398"},
        {"separator", dark ? "#414955" : "#DDE2E9"},
        {"preview", dark ? "#22272D" : "#E5EAEE"},
        {"timelineTrack", dark ? "#383C43" : "#E7E9EE"},
        {"videoClip", "#6487AE"}, {"selectedClip", dark ? "#82A8D6" : "#436FAD"},
        {"imageClip", "#AA936B"}, {"textClip", "#8C7CA5"}, {"cameraClip", "#579989"},
        {"excluded", dark ? "#E3978D" : "#BC534A"},
        {"excludedFill", dark ? "#7D4B48" : "#F2D9D5"},
        {"lineExcluded", dark ? "#BA9ADC" : "#8960AF"},
        {"lineExcludedFill", dark ? "#58476B" : "#E8DDF2"},
        {"thumbnail", dark ? "#303339" : "#F5F6F8"},
        {"scrollbar", dark ? "#626873" : "#B8BFCA"},
        {"tooltip", dark ? "#383C43" : "#FFFFFF"},
        {"transparent", "transparent"}
    };
    return colors;
}

QVariantMap AppearanceManager::colors() const { return designColors(dark()); }
QVariantMap AppearanceManager::metrics() const
{
    return {{"body", 13}, {"caption", 12}, {"section", 15}, {"title", 20},
        {"controlHeight", 30}, {"radius", 6}, {"panelRadius", 10}, {"gap", 12}, {"panelWidth", 276}};
}

QPalette AppearanceManager::palette() const
{
    const auto tokens = colors();
    QPalette palette;
    const auto color = [&](const char *key) { return QColor(tokens.value(QLatin1StringView(key)).toString()); };
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const bool disabled = group == QPalette::Disabled;
        for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::ToolTipText})
            palette.setColor(group, role, color(disabled ? "disabledText" : "text"));
        palette.setColor(group, QPalette::Window, color("window"));
        palette.setColor(group, QPalette::Base, color("input"));
        palette.setColor(group, QPalette::AlternateBase, color("control"));
        palette.setColor(group, QPalette::Button, color("control"));
        palette.setColor(group, QPalette::Light, color("hover"));
        palette.setColor(group, QPalette::Midlight, color("control"));
        palette.setColor(group, QPalette::Mid, color("border"));
        palette.setColor(group, QPalette::Dark, color("separator"));
        palette.setColor(group, QPalette::Highlight, color("accentFill"));
        palette.setColor(group, QPalette::HighlightedText, color("accentText"));
        palette.setColor(group, QPalette::ToolTipBase, color("tooltip"));
        palette.setColor(group, QPalette::Link, color("accent"));
        palette.setColor(group, QPalette::Accent, color("accent"));
        palette.setColor(group, QPalette::PlaceholderText, color("secondaryText"));
    }
    return palette;
}

QVariantMap AppearanceManager::outputColors(const QString &scheme) const
{
    auto tokens = designColors(scheme != QStringLiteral("light"));
    tokens.insert("sceneBackground", DesignTokens::SceneBackground);
    tokens.insert("gaugeBackground", scheme == "light" ? "#EFFFFFFF" : "#E626282D");
    tokens.insert("matteBlack", "#000000");
    tokens.insert("matteWhite", "#FFFFFF");
    tokens.insert("textOutline", "#141C2A");
    return tokens;
}

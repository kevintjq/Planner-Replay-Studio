#include "appearance_manager.h"
#include <QGuiApplication>
#include <QStyleHints>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <cmath>

double luminance(const QColor &c) {
    const auto linear = [](double x) { return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4); };
    return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
}
double contrast(const QVariantMap &tokens, const char *foreground, const char *background) {
    auto a = luminance(QColor(tokens.value(foreground).toString())), b = luminance(QColor(tokens.value(background).toString()));
    return (std::max(a, b) + .05) / (std::min(a, b) + .05);
}

void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir dir;
    try {
        const auto path = dir.filePath("settings.ini");
        AppearanceManager manager(nullptr, path);
        expect(manager.mode() == AppearanceManager::System, "Default must follow system");
        manager.setMode(AppearanceManager::Light);
        const auto metrics = manager.metrics(), output = manager.outputColors();
        expect(!manager.dark() && manager.colors().value("window") == "#F2F4F7", "Updated light tokens");
        manager.setMode(AppearanceManager::Dark);
        expect(manager.dark() && manager.colors().value("panel") == "#252931", "Updated dark tokens");
        for (bool dark : {false, true}) {
            const auto colors = AppearanceManager::designColors(dark);
            for (const auto *background : {"window", "panel", "input", "control", "hover", "selection", "tooltip"})
                expect(contrast(colors, "text", background) >= 4.5, "Normal text contrast below 4.5");
            for (const auto *background : {"window", "panel", "input", "control"})
                expect(contrast(colors, "secondaryText", background) >= 4.5, "Secondary text contrast below 4.5");
            expect(contrast(colors, "accentText", "accentFill") >= 4.5, "Selected button text contrast below 4.5");
            expect(contrast(colors, "border", "input") >= 3., "Input border contrast below 3");
        }
        expect(manager.metrics() == metrics && manager.outputColors() == output, "Theme must not change metrics or output");
        AppearanceManager restored(nullptr, path);
        expect(restored.mode() == AppearanceManager::Dark, "Preference must survive restart");
        manager.setMode(-1); manager.setMode(3);
        expect(manager.mode() == AppearanceManager::Dark, "Reject invalid mode");
        manager.setMode(AppearanceManager::System);
        expect(manager.dark() == (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark), "System scheme resolution");
        const auto lightOutput = manager.outputColors("light");
        expect(lightOutput != output, "Content may explicitly select its own style");
        manager.setMode(AppearanceManager::Light);
        expect(manager.outputColors("light") == lightOutput && manager.outputColors() == output, "Both output styles independent");
        std::cout << "Reference tokens, persistence, metrics and output independence passed.\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}

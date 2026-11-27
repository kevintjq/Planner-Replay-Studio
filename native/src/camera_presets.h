#pragma once

#include <array>

namespace replay {
struct CameraPreset {
    const char *id;
    const char *label;
    float yaw, pitch;
    enum Mode { OrbitMap, Follow, FirstPerson, FirstPersonRaw, OrbitAircraft } mode;
};

inline constexpr std::array CameraPresets{
    CameraPreset{"overview", "地图总览", -115.f, 32.f, CameraPreset::OrbitMap},
    CameraPreset{"rear", "后方跟随", 180.f, 18.f, CameraPreset::Follow},
    CameraPreset{"oblique", "高位斜视", 145.f, 44.f, CameraPreset::Follow},
    CameraPreset{"fpv", "稳定 FPV", 0.f, 0.f, CameraPreset::FirstPerson},
    CameraPreset{"fpv_raw", "原始 FPV", 0.f, 0.f, CameraPreset::FirstPersonRaw},
    CameraPreset{"front", "正前回望", 0.f, 20.f, CameraPreset::Follow},
    CameraPreset{"left", "左侧跟随", 90.f, 15.f, CameraPreset::Follow},
    CameraPreset{"right", "右侧跟随", -90.f, 15.f, CameraPreset::Follow},
    CameraPreset{"rear_left", "左后斜视", 135.f, 25.f, CameraPreset::Follow},
    CameraPreset{"rear_right", "右后斜视", -135.f, 25.f, CameraPreset::Follow},
    CameraPreset{"high", "高位近俯视", 145.f, 72.f, CameraPreset::Follow},
    CameraPreset{"low", "低位追踪", 147.f, 8.f, CameraPreset::Follow},
    CameraPreset{"orbit", "环绕观察", 180.f, 26.f, CameraPreset::OrbitAircraft}
};
}

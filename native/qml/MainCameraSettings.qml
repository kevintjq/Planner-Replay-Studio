import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: pane
    required property var controller
    readonly property var settings: controller.mainView
    readonly property string defaultTarget: "当前位姿 · Default pose"
    enabled: !controller.loading
    StudioLabel { Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap; text: "Target Frame · 跟随目标坐标系"; color: appearance.colors.secondaryText }
    StudioSearchComboBox {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: pane.width
        sourceModel: [pane.defaultTarget].concat(pane.controller.tfFrames)
        selectedText: pane.settings.targetFrame || pane.defaultTarget
        onChosen: value => pane.controller.setMainView({targetFrame: value === pane.defaultTarget ? "" : value})
    }
    StudioComboBox {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: pane.width
        model: ["预设跟随 · Preset", "仅位置 · Position", "位置与偏航 · Position + Yaw", "完整姿态 · Full Pose"]
        currentIndex: pane.settings.followMode ?? 0
        onActivated: index => pane.controller.setMainView({followMode: index})
    }
    GridLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: pane.width
        columns: pane.width < 360 ? 1 : 2
        StudioLabel { text: "跟随滞后（s）" }
        StudioTextField {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: pane.settings.followLag ?? 0
            validator: DoubleValidator { bottom: 0; top: 10; decimals: 3; locale: "C" }
            onEditingFinished: if (acceptableInput) pane.controller.setMainView({followLag: Number(text)})
        }
        StudioLabel { text: "平滑窗口（s）" }
        StudioTextField {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: pane.settings.smoothing ?? 0
            validator: DoubleValidator { bottom: 0; top: 10; decimals: 3; locale: "C" }
            onEditingFinished: if (acceptableInput) pane.controller.setMainView({smoothing: Number(text)})
        }
    }
    StudioCheckBox {
        id: optical
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: pane.width
        text: "Optical Frame · 光学坐标"
        checked: pane.settings.opticalFrame ?? false
        onClicked: pane.controller.setMainView({opticalFrame: checked})
        StudioToolTip { visible: optical.hovered; text: "仅用于 ROS 光学坐标约定：Z 向前，Y 向下。" }
    }
    StudioLabel {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: appearance.metrics.caption
        color: appearance.colors.secondaryText
        text: "跟随与平滑按实验时间解析；缺少目标 TF 时显示诊断。留空使用项目位姿源。"
    }
}

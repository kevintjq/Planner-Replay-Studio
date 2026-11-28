import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var inspector
    property string topic: ""
    property string messageType: ""
    property string selectedField: ""
    readonly property var fieldChoices: inspector.numericFields.map(row => row.path)
    spacing: 8

    StudioLabel { text: "Numeric field · 数值字段"; font.bold: true }
    StudioSearchComboBox {
        Layout.fillWidth: true
        sourceModel: root.fieldChoices
        selectedText: root.selectedField
        enabled: !root.inspector.samplingWorking
        onChosen: value => { root.selectedField = value; selectedText = value }
        Accessible.name: "Numeric field path · 数值字段路径"
    }
    StudioTextField {
        Layout.fillWidth: true
        text: root.selectedField
        placeholderText: "child.speed / samples[128].value"
        onEditingFinished: root.selectedField = text
        enabled: !root.inspector.samplingWorking
        Accessible.name: "Concrete numeric array path · 数值数组索引"
    }
    StudioComboBox {
        id: basis
        Layout.fillWidth: true
        model: ["Bag storage · 录包存储时间", "Header · 消息实验时间"]
        enabled: !root.inspector.samplingWorking
        Accessible.name: "Sampling time basis · 采样时间基准"
    }
    RowLayout {
        Layout.fillWidth: true
        StudioTextField { id: first; Layout.fillWidth: true; placeholderText: "Start ns · 留空全区间"; enabled: !root.inspector.samplingWorking; Accessible.name: "First absolute timestamp ns · 起点整数纳秒" }
        StudioTextField { id: last; Layout.fillWidth: true; placeholderText: "End ns · 留空全区间"; enabled: !root.inspector.samplingWorking; Accessible.name: "Last absolute timestamp ns · 终点整数纳秒" }
    }
    RowLayout {
        Layout.fillWidth: true
        StudioButton {
            Layout.fillWidth: true
            text: root.inspector.samplingWorking ? "Reading… · 后台采样中" : "Plot / Statistics · 曲线与统计"
            enabled: root.topic.length > 0 && root.selectedField.length > 0 && !root.inspector.samplingWorking
            onClicked: root.inspector.sampleNumeric(root.topic, root.selectedField, basis.currentIndex === 1 ? "header" : "storage", first.text.trim(), last.text.trim(), 20000, root.messageType)
        }
        StudioButton { text: "Cancel · 取消"; visible: root.inspector.samplingWorking; onClicked: root.inspector.cancel() }
    }
    StudioLabel { Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.inspector.samplingStatus; color: appearance.colors.secondaryText; visible: text.length > 0 }
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 170
        visible: root.inspector.numericSamples.length > 0
        color: appearance.colors.input
        border.color: appearance.colors.separator
        radius: appearance.metrics.radius
        Canvas {
            id: plot
            anchors.fill: parent
            anchors.margins: 10
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
                const rows = root.inspector.numericSamples
                if (!rows.length) return
                let low = Infinity, high = -Infinity
                for (const row of rows) { low = Math.min(low, row.value); high = Math.max(high, row.value) }
                const range = Math.max(1e-12, high - low)
                const duration = Math.max(1e-12, rows[rows.length - 1].seconds)
                ctx.strokeStyle = appearance.colors.separator; ctx.lineWidth = 1; ctx.beginPath()
                for (let i = 0; i <= 4; ++i) { const y = 8 + (height - 26) * i / 4; ctx.moveTo(0, y); ctx.lineTo(width, y) }
                ctx.stroke(); ctx.strokeStyle = appearance.colors.accent; ctx.lineWidth = 1.5; ctx.beginPath()
                for (let i = 0; i < rows.length; ++i) {
                    const x = rows[i].seconds / duration * width
                    const y = 8 + (height - 26) * (1 - (rows[i].value - low) / range)
                    if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y)
                }
                ctx.stroke(); ctx.fillStyle = appearance.colors.secondaryText; ctx.font = appearance.metrics.caption + "px sans-serif"
                ctx.fillText("0 s", 0, height - 1); ctx.fillText(duration.toPrecision(5) + " s", Math.max(0, width - 90), height - 1)
            }
        }
    }
    StudioLabel {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        visible: root.inspector.statistics.validCount > 0
        text: {
            const s = root.inspector.statistics
            if (!(s.validCount > 0)) return ""
            return "n=" + s.validCount + " · min " + Number(s.minimum).toPrecision(6) + " · max " + Number(s.maximum).toPrecision(6)
                + "\nMean " + Number(s.mean).toPrecision(6) + " · σ " + Number(s.standardDeviation).toPrecision(6)
                + "\n" + s.timeBasis + " · " + s.firstStampNs + " – " + s.lastStampNs + " ns"
                + "\nUnits are not supplied by ROS schema · 消息定义未提供单位"
        }
    }
    Connections { target: root.inspector; function onSamplingChanged() { plot.requestPaint() } }
    Connections { target: appearance; function onChanged() { plot.requestPaint() } }
}

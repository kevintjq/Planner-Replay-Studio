pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Configuration is owned by the project/controller. Every change is a patch;
// choosing a visual preset never replaces data, units, ranges or transforms.
ColumnLayout {
    id: root
    required property var controller
    required property var model
    required property string meterId
    property var topicInspector: null
    property var styleMap: ({})
    property var settings: ({})
    property var sourceMap: ({})
    property var sourceDraft: ({})
    property string validationMessage: ""
    property int dataRevision: 0
    // Supplied by the application's one AppearanceManager context object.
    // qmllint disable unqualified
    readonly property var uiColors: appearance.colors
    // qmllint enable unqualified
    readonly property var catalog: topicInspector ? topicInspector.catalog : []
    readonly property var selectedTopic: catalog.find(row => row.name === sourceDraft.topic && (!sourceDraft.type || row.type === sourceDraft.type)) || ({})
    readonly property var fields: selectedTopic.numericFields || []
    readonly property var sourceState: { const revision = dataRevision; return model && meterId ? model.sourceState(meterId) : ({}) }
    readonly property real unitScale: settings.unit === "km/h" ? 3.6 : 1
    signal sourceEdited(var changes)
    signal settingsEdited(var changes)
    signal styleEdited(var changes)
    signal presetRequested(string preset)
    signal copyStyleRequested()
    signal pasteStyleRequested()
    signal exportElementRequested(string format)
    signal exportCsvRequested()
    spacing: 8
    Layout.fillWidth: true
    onSourceMapChanged: sourceDraft = Object.assign({}, sourceMap)
    Component.onCompleted: sourceDraft = Object.assign({}, sourceMap)
    Connections { target: root.model; function onChanged() { ++root.dataRevision } }
    function style(key, fallback) { return styleMap[key] === undefined ? fallback : styleMap[key] }
    function setting(key, fallback) { return settings[key] === undefined ? fallback : settings[key] }
    function editStyle(key, value) { const patch = {}; patch[key] = value; styleEdited(patch) }
    function editSetting(key, value) { const patch = {}; patch[key] = value; settingsEdited(patch) }
    function editSource(key, value) { const patch = {}; patch[key] = value; sourceDraft = Object.assign({}, sourceDraft, patch) }
    function setAxis(index, value) {
        const axes = (sourceDraft.fields || ["", "", ""]).slice(); axes[index] = value
        sourceDraft = Object.assign({}, sourceDraft, { fields: axes, fieldPath: "" })
    }
    function applySource() {
        if (!sourceDraft.topic) { validationMessage = "Select a recorded Topic · 请选择录包中的 Topic"; return }
        if (sourceDraft.fieldPath && fields.indexOf(sourceDraft.fieldPath) < 0) { validationMessage = "The field must exist in the recorded schema · 字段必须存在于真实消息定义"; return }
        if (sourceDraft.fields && sourceDraft.fields.some(path => fields.indexOf(path) < 0)) { validationMessage = "Select all three real numeric fields · 请完整选择三个真实字段"; return }
        validationMessage = ""; sourceEdited(Object.assign({}, sourceDraft))
    }
    function validNs(value) { return /^-?[0-9]+$/.test(value.trim()) }
    function compareNs(a, b) {
        // BigInt is intentionally avoided: Qt's JS engine need not support it.
        a = a.trim().replace(/^\+/, ""); b = b.trim().replace(/^\+/, "")
        const an = a[0] === "-", bn = b[0] === "-"
        if (an !== bn) return an ? -1 : 1
        const aa = (an ? a.slice(1) : a).replace(/^0+/, "") || "0", bb = (bn ? b.slice(1) : b).replace(/^0+/, "") || "0"
        const result = aa.length === bb.length ? (aa === bb ? 0 : aa < bb ? -1 : 1) : aa.length < bb.length ? -1 : 1
        return an ? -result : result
    }
    function addRange(exclude) {
        const begin = rangeBegin.text.trim(), end = rangeEnd.text.trim()
        if (!validNs(begin) || !validNs(end) || compareNs(begin, end) >= 0) { validationMessage = "Use increasing exact integer nanoseconds · 请填写递增的整数纳秒"; return }
        const key = exclude ? "excludeRanges" : "includeRanges", ranges = (settings[key] || []).slice()
        ranges.push({ beginNs: begin, endNs: end }); editSetting(key, ranges); validationMessage = ""
    }
    function removeRange(key, index) { const ranges = (settings[key] || []).slice(); ranges.splice(index, 1); editSetting(key, ranges) }

    component SectionHeader: StudioButton {
        property bool expanded: false
        Layout.fillWidth: true
        onClicked: expanded = !expanded
        Accessible.description: expanded ? "Collapse section · 收起设置" : "Expand section · 展开设置"
    }
    component NumericSetting: RowLayout {
        id: numericRow
        property string label
        property real value
        property real minimum: -100000
        property real maximum: 100000
        property int decimals: 2
        signal edited(real result)
        Layout.fillWidth: true
        StudioLabel { text: numericRow.label; Layout.fillWidth: true; wrapMode: Text.Wrap }
        StudioTextField {
            id: field
            Layout.preferredWidth: 92
            text: Number(numericRow.value).toFixed(numericRow.decimals)
            validator: DoubleValidator { bottom: numericRow.minimum; top: numericRow.maximum; locale: "C" }
            Accessible.name: numericRow.label
            onEditingFinished: if (acceptableInput) numericRow.edited(Number(text))
        }
    }
    component InlineColor: RowLayout {
        id: colorRow
        property string label
        property string value
        signal edited(string result)
        Layout.fillWidth: true
        StudioLabel { text: colorRow.label; Layout.fillWidth: true; wrapMode: Text.Wrap }
        Rectangle { implicitWidth: 19; implicitHeight: 19; radius: 5; color: colorRow.value; border.color: root.uiColors.border }
        StudioTextField {
            Layout.preferredWidth: 106
            text: colorRow.value
            validator: RegularExpressionValidator { regularExpression: /^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/ }
            Accessible.name: colorRow.label
            onEditingFinished: if (acceptableInput) colorRow.edited(text)
        }
    }

    StudioLabel { text: "Speed Meter · 科研速度表"; font.bold: true; Layout.fillWidth: true }
    StudioLabel {
        Layout.fillWidth: true; wrapMode: Text.Wrap
        text: root.sourceState.working ? "Reading full original samples… · 后台读取完整原始数据" : root.sourceState.error || (root.sourceState.ready ? "Real source indexed · 真实来源已建索引" : "Choose a source · 请选择真实数据源")
        color: root.sourceState.error ? root.uiColors.error : root.uiColors.secondaryText
    }
    StudioLabel { Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.validationMessage; visible: text.length > 0; color: root.uiColors.error }

    SectionHeader { id: sourceSection; text: "Data Source · 数据来源"; expanded: true }
    ColumnLayout {
        visible: sourceSection.expanded; Layout.fillWidth: true; spacing: 7
        StudioLabel { text: "Topic · 真实录包话题" }
        StudioSearchComboBox {
            objectName: "speed-topic"; Layout.fillWidth: true
            sourceModel: root.catalog.map(row => row.name + " · " + row.type)
            selectedText: root.sourceDraft.topic ? root.sourceDraft.topic + " · " + (root.sourceDraft.type || root.selectedTopic.type || "") : ""
            onChosen: value => {
                const row = root.catalog.find(entry => entry.name + " · " + entry.type === value)
                if (row) root.sourceDraft = Object.assign({}, root.sourceDraft, { topic: row.name, type: row.type, fields: undefined, fieldPath: "" })
            }
        }
        StudioComboBox {
            Layout.fillWidth: true
            model: ["Standard velocity · 标准速度字段", "3 numeric fields · 三轴真实数值", "1 numeric field · 单个真实字段", "Position derivative · 真实位置差分（估算）"]
            currentIndex: root.sourceDraft.method === "position-difference" ? 3 : root.sourceDraft.fieldPath ? 2 : root.sourceDraft.fields ? 1 : 0
            onActivated: {
                const inputUnit = currentIndex === 3 ? "m" : root.sourceDraft.inputUnit === "m" ? "m/s" : root.sourceDraft.inputUnit || "m/s"
                if (currentIndex === 0) root.sourceDraft = Object.assign({}, root.sourceDraft, { method: "direct", fields: undefined, fieldPath: "", inputUnit: inputUnit })
                else if (currentIndex === 2) root.sourceDraft = Object.assign({}, root.sourceDraft, { method: "direct", fields: undefined, fieldPath: root.fields[0] || "", inputUnit: inputUnit })
                else root.sourceDraft = Object.assign({}, root.sourceDraft, { method: currentIndex === 3 ? "position-difference" : "direct", fieldPath: "", fields: root.sourceDraft.fields || ["", "", ""], inputUnit: inputUnit })
            }
        }
        Repeater {
            model: root.sourceDraft.fields ? 3 : 0
            delegate: RowLayout {
                required property int index; Layout.fillWidth: true
                StudioLabel { text: ["X", "Y", "Z"][parent.index]; Layout.preferredWidth: 18 }
                StudioSearchComboBox { Layout.fillWidth: true; sourceModel: root.fields; selectedText: root.sourceDraft.fields[parent.index] || ""; onChosen: value => root.setAxis(parent.index, value) }
            }
        }
        StudioSearchComboBox {
            visible: !!root.sourceDraft.fieldPath; Layout.fillWidth: true
            sourceModel: root.fields; selectedText: root.sourceDraft.fieldPath || ""
            onChosen: value => root.editSource("fieldPath", value)
        }
        StudioCheckBox { visible: !!root.sourceDraft.fieldPath; text: "Scalar is speed · 此标量为非负速率"; checked: root.sourceDraft.scalarIsSpeed ?? false; onToggled: root.editSource("scalarIsSpeed", checked) }
        RowLayout {
            Layout.fillWidth: true
            StudioLabel { text: "Input Unit · 源单位"; Layout.fillWidth: true }
            StudioComboBox { model: root.sourceDraft.method === "position-difference" ? ["m"] : ["m/s", "km/h"]; currentIndex: Math.max(0, model.indexOf(root.sourceDraft.inputUnit || model[0])); onActivated: root.editSource("inputUnit", currentText) }
        }
        StudioComboBox {
            Layout.fillWidth: true; model: ["Header · 消息实验时间", "Bag Storage · 录包存储时间"]
            currentIndex: root.sourceDraft.timeBasis === "storage" ? 1 : 0
            onActivated: root.editSource("timeBasis", currentIndex === 1 ? "storage" : "header")
        }
        StudioTextField { Layout.fillWidth: true; text: root.sourceDraft.sourceFrame || ""; placeholderText: "Source Frame · 留空使用真实消息 Frame"; onEditingFinished: root.editSource("sourceFrame", text.trim()) }
        StudioTextField { Layout.fillWidth: true; text: root.sourceDraft.targetFrame || ""; placeholderText: "Target Frame · 目标坐标系，需要真实 TF"; onEditingFinished: root.editSource("targetFrame", text.trim()) }
        NumericSetting { label: "Maximum gap · 最大有效间隔 s"; value: root.sourceDraft.maximumGapSeconds ?? .5; minimum: .001; maximum: 60; decimals: 3; onEdited: result => root.editSource("maximumGapSeconds", result) }
        StudioLabel { Layout.fillWidth: true; wrapMode: Text.Wrap; color: root.uiColors.secondaryText; text: "Missing Frame/TF/data → N/A. Directional modes use the configured axes; position differences are estimated.\n缺少 Frame、TF 或数据时显示 N/A；方向以配置坐标轴为准，位置差分明确为估算。" }
        StudioButton { Layout.fillWidth: true; text: "Apply Source · 应用真实来源"; enabled: root.catalog.length > 0 && !root.sourceState.working; onClicked: { if (!root.sourceDraft.timeBasis) root.editSource("timeBasis", "header"); if (!root.sourceDraft.inputUnit) root.editSource("inputUnit", root.sourceDraft.method === "position-difference" ? "m" : "m/s"); root.applySource() } }
        StudioComboBox {
            objectName: "speed-unit"; Layout.fillWidth: true; model: ["m/s", "km/h"]
            currentIndex: root.settings.unit === "km/h" ? 1 : 0; onActivated: root.editSetting("unit", currentText)
        }
        StudioComboBox {
            Layout.fillWidth: true; model: ["3D Speed · 三维合成", "Horizontal · 水平合成", "Vertical · 垂直分量", "X", "Y", "Z", "Custom · 自定义"]
            readonly property var modes: ["total", "horizontal", "vertical", "x", "y", "z", "custom"]
            currentIndex: Math.max(0, modes.indexOf(root.setting("mode", "total"))); onActivated: root.editSetting("mode", modes[currentIndex])
        }
    }

    SectionHeader { id: statisticsSection; text: "Statistics · 统计区间" }
    ColumnLayout {
        visible: statisticsSection.expanded; Layout.fillWidth: true
        StudioComboBox {
            Layout.fillWidth: true; model: ["Entire Flight · 全程", "Current Clip · 当前剪辑", "Custom Range · 自定义", "Elapsed Time · 已发生", "Multiple Ranges · 多段"]
            readonly property var modes: ["entire", "current_clip", "custom", "elapsed", "multiple"]
            currentIndex: Math.max(0, modes.indexOf(root.setting("statisticsMode", "entire"))); onActivated: root.editSetting("statisticsMode", modes[currentIndex])
        }
        StudioLabel { Layout.fillWidth: true; wrapMode: Text.Wrap; color: root.uiColors.secondaryText; text: "Exact experiment ns · 精确实验纳秒。重叠区间自动合并，排除优先；平均按真实时间加权，当前值独立跟随播放头。" }
        StudioTextField { id: rangeBegin; objectName: "speed-range-begin"; Layout.fillWidth: true; placeholderText: "Begin ns · 起点整数纳秒"; validator: RegularExpressionValidator { regularExpression: /^-?[0-9]+$/ } }
        StudioTextField { id: rangeEnd; objectName: "speed-range-end"; Layout.fillWidth: true; placeholderText: "End ns · 终点整数纳秒"; validator: RegularExpressionValidator { regularExpression: /^-?[0-9]+$/ } }
        RowLayout {
            StudioButton { text: "+ Include · 纳入"; Layout.fillWidth: true; onClicked: root.addRange(false) }
            StudioButton { text: "− Exclude · 排除"; Layout.fillWidth: true; onClicked: root.addRange(true) }
        }
        Repeater {
            model: ["includeRanges", "excludeRanges"]
            delegate: ColumnLayout {
                id: rangeGroup; required property string modelData; Layout.fillWidth: true
                StudioLabel { text: rangeGroup.modelData === "includeRanges" ? "Included · 纳入" : "Excluded · 排除" }
                Repeater {
                    model: root.settings[rangeGroup.modelData] || []
                    delegate: RowLayout {
                        required property var modelData; required property int index; Layout.fillWidth: true
                        StudioLabel { text: parent.modelData.beginNs + "\n– " + parent.modelData.endNs + " ns"; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                        StudioButton { text: "×"; Accessible.name: "Remove range · 删除区间"; onClicked: root.removeRange(rangeGroup.modelData, parent.index) }
                    }
                }
            }
        }
        RowLayout {
            StudioCheckBox { text: "Max."; checked: root.style("showMaximum", true); onToggled: root.editStyle("showMaximum", checked) }
            StudioCheckBox { text: "Cur."; checked: root.style("showCurrent", true); onToggled: root.editStyle("showCurrent", checked) }
            StudioCheckBox { text: "Avg."; checked: root.style("showAverage", true); onToggled: root.editStyle("showAverage", checked) }
        }
        StudioButton { text: "Export Full CSV · 导出完整原始数据与统计"; Layout.fillWidth: true; enabled: !!root.sourceState.ready && !root.model.exporting; onClicked: root.exportCsvRequested() }
    }

    SectionHeader { id: gaugeSection; text: "Gauge · 圆弧与量程"; expanded: true }
    ColumnLayout {
        visible: gaugeSection.expanded; Layout.fillWidth: true
        StudioComboBox {
            objectName: "speed-preset"; Layout.fillWidth: true
            model: ["Gradient Gauge · 渐变仪表", "Minimal Gauge · 极简", "Digital HUD · 数字", "Speed Curve · 速度曲线", "Publication Style · 论文", "Custom Style · 自定义"]
            readonly property var presets: ["gradient-gauge", "minimal-gauge", "digital-hud", "speed-curve", "publication", "custom"]
            currentIndex: Math.max(0, presets.indexOf(root.style("preset", "gradient-gauge")))
            onActivated: root.presetRequested(presets[currentIndex])
        }
        RowLayout {
            StudioComboBox { Layout.fillWidth: true; model: ["Auto · 自动量程", "Manual · 手动量程"]; currentIndex: root.settings.rangeMode === "manual" ? 1 : 0; onActivated: root.editSetting("rangeMode", currentIndex ? "manual" : "auto") }
            StudioCheckBox { text: "Lock · 锁定"; checked: root.settings.lockRange ?? false; onToggled: root.editSetting("lockRange", checked) }
        }
        NumericSetting { label: "Minimum · 最小 " + root.setting("unit", "m/s"); value: root.setting("rangeMinimumSI", 0) * root.unitScale; onEdited: result => root.editSetting("rangeMinimumSI", result / root.unitScale) }
        NumericSetting { label: "Maximum · 最大 " + root.setting("unit", "m/s"); value: root.setting("rangeMaximumSI", 10) * root.unitScale; onEdited: result => root.editSetting("rangeMaximumSI", result / root.unitScale) }
        StudioComboBox {
            Layout.fillWidth: true; model: ["Range Preset · 常用物理量程", "0–5 m/s", "0–10 m/s", "0–15 m/s", "0–20 m/s", "0–25 m/s", "0–30 m/s"]
            onActivated: if (currentIndex > 0) root.settingsEdited({ rangeMode: "manual", rangeMinimumSI: 0, rangeMaximumSI: currentIndex * 5 })
        }
        Repeater {
            model: [{key:"startAngle",label:"Start · 起始角 °",value:145,min:-720,max:720},{key:"sweepAngle",label:"Sweep · 覆盖角 °",value:250,min:1,max:360},{key:"radius",label:"Radius · 半径",value:202,min:40,max:225},{key:"arcWidth",label:"Arc Width · 圆弧宽度",value:12,min:1,max:50},{key:"majorTickCount",label:"Major ticks · 主刻度段",value:5,min:1,max:20},{key:"minorTickCount",label:"Minor divisions · 次刻度分段",value:4,min:1,max:10},{key:"tickLabelSize",label:"Tick label · 数字字号",value:20,min:6,max:40}]
            delegate: NumericSetting { required property var modelData; label: modelData.label; value: root.style(modelData.key,modelData.value); minimum: modelData.min; maximum: modelData.max; onEdited: result => root.editStyle(modelData.key,result) }
        }
        StudioComboBox {
            Layout.fillWidth: true; model: ["Marker · 标记", "Needle · 指针", "Dot · 圆点", "Progress Arc · 进度圆弧", "Off · 关闭"]
            readonly property var styles: ["marker", "needle", "dot", "progress", "none"]
            currentIndex: Math.max(0, styles.indexOf(root.style("indicatorStyle", "marker"))); onActivated: root.editStyle("indicatorStyle", styles[currentIndex])
        }
        InlineColor { label: "Indicator · 指示颜色"; value: root.style("indicatorColor", "#f5fbff"); onEdited: result => root.editStyle("indicatorColor", result) }
        RowLayout {
            StudioCheckBox { text: "Ticks · 刻度"; checked: root.style("showTicks",true); onToggled: root.editStyle("showTicks",checked) }
            StudioCheckBox { text: "Numbers · 数字"; checked: root.style("showTickLabels",true); onToggled: root.editStyle("showTickLabels",checked) }
        }
        RowLayout {
            StudioCheckBox { text: "Inner · 内环"; checked: root.style("showInnerRing",false); onToggled: root.editStyle("showInnerRing",checked) }
            StudioCheckBox { text: "Outer · 外环"; checked: root.style("showOuterRing",true); onToggled: root.editStyle("showOuterRing",checked) }
        }
        StudioLabel { text: "Gradient Stops · 角度渐变节点"; font.bold: true }
        Repeater {
            model: root.style("gradientStops", [{position:0,color:"#3588ff"},{position:.2,color:"#20d2ee"},{position:.4,color:"#29dc97"},{position:.6,color:"#eee66b"},{position:.8,color:"#ff9d48"},{position:1,color:"#ff535d"}])
            delegate: RowLayout {
                id: gradientRow; required property var modelData; required property int index; Layout.fillWidth: true
                StudioTextField { Layout.preferredWidth: 62; text: Math.round(gradientRow.modelData.position*100); validator: DoubleValidator {bottom:0;top:100;locale:"C"} Accessible.name:"Gradient position percent · 渐变位置百分比"; onEditingFinished: if(acceptableInput){const values=root.style("gradientStops",[]).length?root.styleMap.gradientStops.slice():[{position:0,color:"#3588ff"},{position:.2,color:"#20d2ee"},{position:.4,color:"#29dc97"},{position:.6,color:"#eee66b"},{position:.8,color:"#ff9d48"},{position:1,color:"#ff535d"}];values[gradientRow.index]=Object.assign({},values[gradientRow.index],{position:Number(text)/100});root.editStyle("gradientStops",values)} }
                InlineColor { Layout.fillWidth: true; label: "%"; value: gradientRow.modelData.color; onEdited: result => { const values=root.style("gradientStops",[{position:0,color:"#3588ff"},{position:.2,color:"#20d2ee"},{position:.4,color:"#29dc97"},{position:.6,color:"#eee66b"},{position:.8,color:"#ff9d48"},{position:1,color:"#ff535d"}]).slice();values[gradientRow.index]=Object.assign({},values[gradientRow.index],{color:result});root.editStyle("gradientStops",values) } }
            }
        }
    }

    SectionHeader { id: typographySection; text: "Typography · 文字" }
    ColumnLayout {
        visible: typographySection.expanded; Layout.fillWidth: true
        StudioTextField { Layout.fillWidth: true; text: root.style("fontFamily","Segoe UI"); placeholderText: "Font Family · 字体"; onEditingFinished: root.editStyle("fontFamily",text) }
        NumericSetting { label: "Decimal Places · 小数位"; value: root.style("decimalPlaces",2); minimum:0;maximum:8;decimals:0;onEdited: result => root.editStyle("decimalPlaces",result) }
        StudioTextField { Layout.fillWidth: true; text: root.style("title","Exp. Traj."); placeholderText:"Title · 标题"; onEditingFinished: root.editStyle("title",text) }
        StudioTextField { Layout.fillWidth: true; text: root.style("subtitle",""); placeholderText:"Subtitle · 副标题"; onEditingFinished: root.editStyle("subtitle",text) }
        StudioCheckBox { text:"Show Unit · 显示单位";checked:root.style("showUnit",true);onToggled:root.editStyle("showUnit",checked) }
        Repeater {
            model:[{key:"currentText",label:"Current · 当前值字号",size:70},{key:"maxText",label:"Maximum · 最大值字号",size:27},{key:"averageText",label:"Average · 平均值字号",size:27},{key:"titleText",label:"Title · 标题字号",size:27}]
            delegate:NumericSetting {required property var modelData;label:modelData.label;value:(root.styleMap[modelData.key]||{}).fontSize||modelData.size;minimum:4;maximum:160;onEdited:result=>root.editStyle(modelData.key,Object.assign({},root.styleMap[modelData.key]||{},{fontSize:result}))}
        }
        Repeater {
            model:[{key:"textColor",label:"Current · 当前值颜色",color:"#f7fafc"},{key:"maxColor",label:"Maximum · 最大值颜色",color:"#ff8269"},{key:"averageColor",label:"Average · 平均值颜色",color:"#61cfe4"},{key:"titleColor",label:"Title · 标题颜色",color:"#d7e1e9"}]
            delegate:InlineColor {required property var modelData;label:modelData.label;value:root.style(modelData.key,modelData.color);onEdited:result=>root.editStyle(modelData.key,result)}
        }
        NumericSetting { label:"Outline Width · 描边宽度";value:root.style("outlineWidth",0);minimum:0;maximum:12;onEdited:result=>root.editStyle("outlineWidth",result) }
        InlineColor {label:"Outline · 描边色";value:root.style("outlineColor","#18222c");onEdited:result=>root.editStyle("outlineColor",result)}
        StudioCheckBox {text:"Text Shadow · 文字阴影";checked:root.style("textShadow",true);onToggled:root.editStyle("textShadow",checked)}
    }

    SectionHeader { id: appearanceSection; text: "Appearance · 输出外观" }
    ColumnLayout {
        visible:appearanceSection.expanded;Layout.fillWidth:true
        StudioComboBox {Layout.fillWidth:true;model:["Transparent · 真透明","Translucent · 半透明","Solid · 纯色"];readonly property var modes:["transparent","translucent","solid"];currentIndex:Math.max(0,modes.indexOf(root.style("backgroundMode","transparent")));onActivated:root.editStyle("backgroundMode",modes[currentIndex])}
        InlineColor {label:"Background · 背景";value:root.style("backgroundColor","#202831");onEdited:result=>root.editStyle("backgroundColor",result)}
        Repeater {
            model:[{key:"backgroundOpacity",label:"Background Opacity · 背景",value:.65},{key:"arcOpacity",label:"Arc Opacity · 圆弧",value:1},{key:"indicatorOpacity",label:"Indicator Opacity · 指示",value:1},{key:"textOpacity",label:"Text Opacity · 文字",value:1},{key:"overallOpacity",label:"Overall Opacity · 整体",value:1}]
            delegate:ColumnLayout {id:opacityRow;required property var modelData;Layout.fillWidth:true;StudioLabel{text:opacityRow.modelData.label}StudioSlider{Layout.fillWidth:true;from:0;to:1;value:root.style(opacityRow.modelData.key,opacityRow.modelData.value);onPressedChanged:pressed?root.controller.canvas?.beginEdit():root.controller.canvas?.endEdit();onMoved:root.editStyle(opacityRow.modelData.key,value)}}
        }
        NumericSetting{label:"Border Width · 边框宽度";value:root.style("borderWidth",0);minimum:0;maximum:20;onEdited:result=>root.editStyle("borderWidth",result)}
        InlineColor{label:"Border · 边框";value:root.style("borderColor","#8193a2");onEdited:result=>root.editStyle("borderColor",result)}
        NumericSetting{label:"Corner Radius · 圆角";value:root.style("cornerRadius",28);minimum:0;maximum:100;onEdited:result=>root.editStyle("cornerRadius",result)}
        StudioCheckBox{text:"Panel Shadow · 面板阴影";checked:root.style("panelShadow",false);onToggled:root.editStyle("panelShadow",checked)}
        StudioLabel{Layout.fillWidth:true;wrapMode:Text.Wrap;color:root.uiColors.secondaryText;text:"Output colours are independent of Light/Dark UI. Presets change appearance only.\n输出配色独立于软件亮暗主题；预设仅改变外观。"}
        RowLayout{StudioButton{text:"Copy Style · 复制";Layout.fillWidth:true;onClicked:root.copyStyleRequested()}StudioButton{text:"Paste · 粘贴";Layout.fillWidth:true;onClicked:root.pasteStyleRequested()}}
        RowLayout{Repeater{model:["PNG","TIFF","SVG","PDF"];delegate:StudioButton{required property string modelData;text:modelData;Layout.fillWidth:true;onClicked:root.exportElementRequested(modelData)}}}
    }
}

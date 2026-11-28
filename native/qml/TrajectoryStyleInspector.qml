import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var controller
    readonly property var row: controller.canvasLayers.find(layer => layer.id === controller.canvas.selectedId) || ({})
    readonly property var line: controller.settings.edits?.line_style || ({})
    readonly property bool goal: row.sourceId === "goal"
    readonly property var marker: controller.settings.edits?.[goal ? "goal_style" : "start_style"] || ({})
    readonly property bool endpoint: row.sourceId === "start" || goal
    Layout.fillWidth: true
    function editMarker(key,value) { const patch = {}; patch[key]=value; controller.setEndpointStyle(goal,patch) }
    ColumnLayout {
        Layout.fillWidth: true; visible: root.row.sourceId === "line"
        StudioLabel { text: "Trajectory Line · 轨迹线"; font.bold: true }
        StudioLabel { text: "Width · 线宽（源图像素）" }
        StudioSlider { Layout.fillWidth: true; from: .1; to: 30; value: root.line.width ?? 2; onPressedChanged: pressed ? root.controller.canvas.beginEdit() : root.controller.canvas.endEdit(); onMoved: root.controller.setLineStyle({width:value}) }
        StudioTextField { Layout.fillWidth: true; text: Number(root.line.width ?? 2).toFixed(2); validator: DoubleValidator { bottom:.1;top:512;locale:"C" } onEditingFinished: if(acceptableInput)root.controller.setLineStyle({width:Number(text)}) }
        StudioLabel { text: "Color · 颜色 #RRGGBB" }
        StudioTextField { Layout.fillWidth: true; text: root.line.color || "#54c8ff"; validator: RegularExpressionValidator { regularExpression:/^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/ } onEditingFinished: if(acceptableInput)root.controller.setLineStyle({color:text}) }
        StudioComboBox { Layout.fillWidth: true; model:["Solid · 实线","Dash · 虚线","Dot · 点线","Dash Dot · 点划线"];currentIndex:root.line.pattern??0;onActivated:root.controller.setLineStyle({pattern:currentIndex}) }
        StudioLabel { text: "Line Opacity · 轨迹不透明度" }
        StudioSlider { Layout.fillWidth:true;from:0;to:1;value:root.line.opacity??1;onPressedChanged:pressed?root.controller.canvas.beginEdit():root.controller.canvas.endEdit();onMoved:root.controller.setLineStyle({opacity:value}) }
    }
    ColumnLayout {
        Layout.fillWidth:true;visible:root.endpoint
        StudioLabel { text:root.goal?"Goal · 终点":"Start · 起点";font.bold:true }
        StudioLabel { Layout.fillWidth:true;wrapMode:Text.Wrap;color:appearance.colors.secondaryText;text:"Linked to real trajectory time. Transform changes presentation only.\n关联真实轨迹时间；画布变换只调整展示位置。" }
        StudioLabel { text:"Diameter · 圆形直径（源图像素）" }
        StudioSlider { Layout.fillWidth:true;from:1;to:120;value:root.marker.size??14;onPressedChanged:pressed?root.controller.canvas.beginEdit():root.controller.canvas.endEdit();onMoved:root.editMarker("size",value) }
        StudioTextField { Layout.fillWidth:true;text:Number(root.marker.size??14).toFixed(2);validator:DoubleValidator{bottom:.1;top:2048;locale:"C"} onEditingFinished:if(acceptableInput)root.editMarker("size",Number(text)) }
        StudioLabel { text:"Fill · 填充色（#AARRGGBB 支持透明）" }
        StudioTextField { Layout.fillWidth:true;text:root.marker.fill||(root.goal?"#ef5350":"#37bd74");onEditingFinished:root.editMarker("fill",text) }
        StudioLabel { text:"Stroke · 描边色" }
        StudioTextField { Layout.fillWidth:true;text:root.marker.stroke||"#ffffff";onEditingFinished:root.editMarker("stroke",text) }
        StudioLabel { text:"Stroke Width · 描边粗细" }
        StudioSlider { Layout.fillWidth:true;from:0;to:15;value:root.marker.stroke_width??1;onPressedChanged:pressed?root.controller.canvas.beginEdit():root.controller.canvas.endEdit();onMoved:root.editMarker("stroke_width",value) }
        StudioCheckBox { text:"Label · 显示文字";checked:root.marker.show_text??false;onToggled:root.editMarker("show_text",checked) }
        StudioTextField { Layout.fillWidth:true;text:root.marker.text||(root.goal?"Goal":"Start");onEditingFinished:root.editMarker("text",text) }
        StudioCheckBox { text:"Timestamp · 实际时间";checked:root.marker.show_time??false;onToggled:root.editMarker("show_time",checked) }
    }
}

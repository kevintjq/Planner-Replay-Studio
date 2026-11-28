import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var controller
    readonly property var model: controller.canvas
    readonly property var element: { const revision=model.revision; return model.element(model.selectedId) }
    readonly property var style: element.style || ({})
    signal elementAdded(string id)
    Layout.fillWidth:true;spacing:7
    function editStyle(key,value) { const patch={};patch[key]=value;model.editElement(model.selectedId,{style:patch}) }
    function editStyles(patch) { model.editElement(model.selectedId,{style:patch}) }
    FileDialog {
        id:imageDialog;title:"Add Image · 添加图片";nameFilters:["Images (*.png *.jpg *.jpeg *.svg *.tif *.tiff *.webp)"]
        onAccepted: { const id=root.model.addElement("image",{source:selectedFile.toString(),name:"Image · 图片"});if(id.length)root.elementAdded(id) }
    }
    StudioLabel { text:"Scientific Elements · 科研元素";font.bold:true }
    GridLayout {
        columns:2;Layout.fillWidth:true
        Repeater {
            model:[{kind:"text",label:"Text · 文字"},{kind:"image",label:"Image · 图片"},{kind:"arrow",label:"Arrow · 箭头"},{kind:"circle",label:"Circle · 圆形"},{kind:"rectangle",label:"Rectangle · 矩形"}]
            delegate:StudioButton { required property var modelData;Layout.fillWidth:true;Layout.minimumWidth:0;text:modelData.label;onClicked: { if(modelData.kind==="image")imageDialog.open();else { const id=root.model.addElement(modelData.kind,{name:modelData.label,text:"Experiment · 实验"});if(id.length)root.elementAdded(id) } } }
        }
    }
    ColumnLayout {
        visible:!!root.element.scientific;Layout.fillWidth:true
        StudioTextField { visible:root.element.kind==="text";Layout.fillWidth:true;text:root.element.text||"";Accessible.name:"Text · 文字内容";onEditingFinished:root.model.editElement(root.model.selectedId,{text:text}) }
        StudioLabel { text:"Text Color · 文字颜色";visible:root.element.kind==="text" }
        StudioTextField { Layout.fillWidth:true;visible:root.element.kind==="text";text:root.style.color||"#1f2937";validator:RegularExpressionValidator{regularExpression:/^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/} onEditingFinished:if(acceptableInput)root.editStyle("color",text) }
        StudioLabel { text:"Fill · 填充色 #AARRGGBB";visible:root.element.kind!=="text"&&root.element.kind!=="image" }
        StudioTextField { Layout.fillWidth:true;visible:root.element.kind!=="text"&&root.element.kind!=="image";text:root.style.fill||"#00000000";onEditingFinished:root.editStyle("fill",text) }
        StudioLabel { text:"Stroke · 描边颜色";visible:root.element.kind!=="text"&&root.element.kind!=="image" }
        StudioTextField { Layout.fillWidth:true;visible:root.element.kind!=="text"&&root.element.kind!=="image";text:root.style.stroke||"#2563eb";onEditingFinished:root.editStyle("stroke",text) }
        StudioLabel { text:"Stroke Width · 描边宽度";visible:root.element.kind!=="text"&&root.element.kind!=="image" }
        StudioSlider { Layout.fillWidth:true;visible:root.element.kind!=="text"&&root.element.kind!=="image";from:0;to:15;value:root.style.strokeWidth??4;onPressedChanged:pressed?root.model.beginEdit():root.model.endEdit();onMoved:root.editStyle("strokeWidth",value) }
        ColumnLayout {
            Layout.fillWidth:true;visible:root.element.kind==="text"
            StudioTextField { Layout.fillWidth:true;text:root.style.fontFamily||"Segoe UI";Accessible.name:"Font Family · 字体";onEditingFinished:root.editStyle("fontFamily",text) }
            StudioLabel { text:"Font Size · 字号" }
            StudioSlider { Layout.fillWidth:true;from:8;to:160;value:root.style.fontSize??36;onPressedChanged:pressed?root.model.beginEdit():root.model.endEdit();onMoved:root.editStyle("fontSize",value) }
            StudioCheckBox { text:"Bold · 粗体";checked:(root.style.fontWeight??400)>=600;onToggled:root.editStyle("fontWeight",checked?700:400) }
            StudioCheckBox { text:"Italic · 斜体";checked:root.style.italic??false;onToggled:root.editStyle("italic",checked) }
            StudioComboBox { Layout.fillWidth:true;model:["Left · 左对齐","Center · 居中","Right · 右对齐"];currentIndex:Math.max(0,["left","center","right"].indexOf(root.style.alignment||"left"));onActivated:root.editStyle("alignment",["left","center","right"][currentIndex]) }
            StudioCheckBox {
                text:"Text Outline · 文字描边";checked:(root.style.textStrokeWidth??0)>0
                onToggled:root.editStyles({textStrokeWidth:checked?2:0,textStrokeColor:checked&&(!root.style.textStrokeColor||root.style.textStrokeColor==="#00000000")?"#ffffff":(root.style.textStrokeColor||"#00000000")})
            }
            StudioTextField {
                Layout.fillWidth:true;enabled:(root.style.textStrokeWidth??0)>0;text:root.style.textStrokeColor||"#00000000"
                Accessible.name:"Text Outline Color · 文字描边颜色";placeholderText:"#RRGGBB / #AARRGGBB"
                validator:RegularExpressionValidator{regularExpression:/^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/}
                onEditingFinished:if(acceptableInput)root.editStyle("textStrokeColor",text)
            }
            StudioLabel { text:"Outline Width · 描边宽度 "+Number(root.style.textStrokeWidth??0).toFixed(1)+" px" }
            StudioSlider { Layout.fillWidth:true;enabled:(root.style.textStrokeWidth??0)>0;from:.25;to:20;value:root.style.textStrokeWidth??.25;onPressedChanged:pressed?root.model.beginEdit():root.model.endEdit();onMoved:root.editStyle("textStrokeWidth",value) }
            StudioCheckBox { text:"Text Shadow · 文字阴影";checked:(root.style.textShadowOpacity??0)>0;onToggled:root.editStyle("textShadowOpacity",checked ? .35 : 0) }
            StudioTextField {
                Layout.fillWidth:true;enabled:(root.style.textShadowOpacity??0)>0;text:root.style.textShadowColor||"#000000"
                Accessible.name:"Text Shadow Color · 文字阴影颜色";placeholderText:"#RRGGBB / #AARRGGBB"
                validator:RegularExpressionValidator{regularExpression:/^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/}
                onEditingFinished:if(acceptableInput)root.editStyle("textShadowColor",text)
            }
            StudioLabel { text:"Shadow Opacity · 阴影不透明度 "+Math.round(Number(root.style.textShadowOpacity??0)*100)+"%" }
            StudioSlider { Layout.fillWidth:true;enabled:(root.style.textShadowOpacity??0)>0;from:.01;to:1;value:root.style.textShadowOpacity??.01;onPressedChanged:pressed?root.model.beginEdit():root.model.endEdit();onMoved:root.editStyle("textShadowOpacity",value) }
            StudioLabel { text:"Shadow Softness · 阴影柔化 "+Number(root.style.textShadowBlur??4).toFixed(1)+" px" }
            StudioSlider { Layout.fillWidth:true;enabled:(root.style.textShadowOpacity??0)>0;from:0;to:32;value:root.style.textShadowBlur??4;onPressedChanged:pressed?root.model.beginEdit():root.model.endEdit();onMoved:root.editStyle("textShadowBlur",value) }
            StudioLabel { text:"Shadow Offset X · 阴影水平偏移 "+Number(root.style.textShadowOffsetX??3).toFixed(1)+" px" }
            StudioSlider { Layout.fillWidth:true;enabled:(root.style.textShadowOpacity??0)>0;from:-64;to:64;value:root.style.textShadowOffsetX??3;onPressedChanged:pressed?root.model.beginEdit():root.model.endEdit();onMoved:root.editStyle("textShadowOffsetX",value) }
            StudioLabel { text:"Shadow Offset Y · 阴影垂直偏移 "+Number(root.style.textShadowOffsetY??3).toFixed(1)+" px" }
            StudioSlider { Layout.fillWidth:true;enabled:(root.style.textShadowOpacity??0)>0;from:-64;to:64;value:root.style.textShadowOffsetY??3;onPressedChanged:pressed?root.model.beginEdit():root.model.endEdit();onMoved:root.editStyle("textShadowOffsetY",value) }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: editor
    required property var controller
    required property var canvas
    readonly property var model: controller.canvas
    readonly property string selectedId: model.selectedId
    readonly property var item: canvas.layerItems[selectedId] || null
    readonly property var settings: { const revision=model.revision;return model.settings(selectedId) }
    readonly property var evaluated: { const revision=model.revision;return model.settings(selectedId,controller.position) }
    readonly property var frame: item ? {x:item.x/canvas.width,y:item.y/canvas.height,width:item.width/canvas.width,height:item.height/canvas.height} : {x:0,y:0,width:0,height:0}
    readonly property var videoLayers: controller.videoLayers || []
    property bool pixels: true
    spacing:8
    function setRect(key,value) {
        if(!item||!isFinite(value))return
        const vertical=key==="y"||key==="height",dimension=vertical?model.outputHeight:model.outputWidth
        const n=value/(pixels?dimension:100),changes={};changes[key]=n
        if(settings.aspectLocked!==false && (key==="width"||key==="height")) {
            const other=key==="width"?"height":"width";changes[other]=frame[other]*n/Math.max(.000001,frame[key])
        }
        model.edit(selectedId,changes)
    }
    StudioLabel { Layout.fillWidth:true;text:item?(settings.name||item.layerName):"Select a layer on the canvas.\n在画布或 Layers 中选中图层。";wrapMode:Text.Wrap;font.bold:true }
    StudioTextField { Layout.fillWidth:true;visible:!!editor.item;text:editor.settings.name||editor.item?.layerName||"";Accessible.name:"Layer Name · 图层名称";onEditingFinished:editor.model.edit(editor.selectedId,{name:text}) }
    RowLayout {
        visible:!!editor.item
        StudioCheckBox { text:"Visible · 显示";checked:editor.settings.visible!==false;onToggled:editor.model.edit(editor.selectedId,{visible:checked}) }
        StudioCheckBox { text:"Lock · 锁定";checked:editor.settings.locked??false;onToggled:editor.model.edit(editor.selectedId,{locked:checked}) }
    }
    RowLayout {
        visible: !!editor.item; Layout.fillWidth: true
        StudioButton { text: "Duplicate · 复制"; onClicked: editor.model.duplicate(editor.selectedId) }
        StudioButton { text: "Remove · 删除"; onClicked: editor.model.removeSelected() }
    }
    ColumnLayout {
        visible:!!editor.item;enabled:!editor.settings.locked;Layout.fillWidth:true
        StudioButton { text:"Transform · 变换";Layout.fillWidth:true;onClicked:transformFields.visible=!transformFields.visible }
        ColumnLayout {
            id:transformFields;Layout.fillWidth:true
            StudioComboBox { model:["Pixels · 项目像素","Percent · 画布比例 %"];currentIndex:editor.pixels?0:1;onActivated:editor.pixels=currentIndex===0;Layout.fillWidth:true }
            Repeater {
                model:[{key:"x",label:"Position X · 水平"},{key:"y",label:"Position Y · 垂直"},{key:"width",label:"Width · 宽度"},{key:"height",label:"Height · 高度"}]
                delegate:RowLayout {
                    required property var modelData
                    Layout.fillWidth:true
                    StudioLabel { text:modelData.label;Layout.fillWidth:true }
                    StudioTextField {
                        objectName:"transform-"+modelData.key;Layout.preferredWidth:92
                        text:(editor.frame[modelData.key]*(editor.pixels?(modelData.key==="y"||modelData.key==="height"?editor.model.outputHeight:editor.model.outputWidth):100)).toFixed(editor.pixels?2:3)
                        validator:DoubleValidator { bottom:modelData.key==="width"||modelData.key==="height" ? .01 : -100000;top:100000;locale:"C" }
                        onEditingFinished:editor.setRect(modelData.key,Number(text))
                    }
                }
            }
            RowLayout {
                StudioLabel { text:"Scale · 缩放 %";Layout.fillWidth:true }
                StudioTextField { Layout.preferredWidth:92;text:((editor.settings.scale??1)*100).toFixed(2);validator:DoubleValidator{bottom:1;top:10000;locale:"C"} onEditingFinished:{const scale=Number(text)/100,ratio=scale/(editor.settings.scale??1);editor.model.edit(editor.selectedId,{width:editor.frame.width*ratio,height:editor.frame.height*ratio,scale:scale})} }
            }
            RowLayout {
                StudioLabel { text:"Rotation · 旋转 °";Layout.fillWidth:true }
                StudioTextField { objectName:"transform-rotation";Layout.preferredWidth:92;text:(editor.evaluated.rotation??0).toFixed(2);validator:DoubleValidator{bottom:-36000;top:36000;locale:"C"} onEditingFinished:editor.model.edit(editor.selectedId,{rotation:Number(text)}) }
            }
            StudioSlider { Layout.fillWidth:true;from:-180;to:180;value:editor.evaluated.rotation??0;onPressedChanged:pressed?editor.model.beginEdit():editor.model.endEdit();onMoved:editor.model.edit(editor.selectedId,{rotation:value}) }
            StudioComboBox {
                Layout.fillWidth:true
                model:["Anchor · Center 中心","Top Left 左上","Top Center 顶部","Top Right 右上","Left Center 左中","Right Center 右中","Bottom Left 左下","Bottom Center 底部","Bottom Right 右下"]
                readonly property var anchorChoices:[[.5,.5],[0,0],[.5,0],[1,0],[0,.5],[1,.5],[0,1],[.5,1],[1,1]]
                currentIndex:Math.max(0,anchorChoices.findIndex(a=>a[0]===(editor.settings.anchorX??.5)&&a[1]===(editor.settings.anchorY??.5)))
                onActivated:editor.model.edit(editor.selectedId,{anchorX:anchorChoices[currentIndex][0],anchorY:anchorChoices[currentIndex][1]})
            }
            StudioCheckBox { text:"Lock Aspect Ratio · 锁定宽高比";checked:editor.settings.aspectLocked!==false;onToggled:editor.model.edit(editor.selectedId,{aspectLocked:checked}) }
            ColumnLayout {
                StudioCheckBox { text:"Flip H · 水平翻转";checked:editor.settings.flipH??false;onToggled:editor.model.edit(editor.selectedId,{flipH:checked}) }
                StudioCheckBox { text:"Flip V · 垂直翻转";checked:editor.settings.flipV??false;onToggled:editor.model.edit(editor.selectedId,{flipV:checked}) }
            }
            StudioButton { text:"Reset Transform · 重置变换";onClicked:editor.model.reset(editor.selectedId);Layout.fillWidth:true }
        }
        StudioButton { text:"Appearance · 外观";Layout.fillWidth:true;onClicked:appearanceFields.visible=!appearanceFields.visible }
        ColumnLayout {
            id:appearanceFields;Layout.fillWidth:true
            StudioLabel { text:"Overall Opacity · 图层不透明度" }
            StudioSlider { objectName:"layer-opacity";Layout.fillWidth:true;from:0;to:1;value:editor.evaluated.opacity??editor.item?.baseOpacity??1;onPressedChanged:pressed?editor.model.beginEdit():editor.model.endEdit();onMoved:editor.model.edit(editor.selectedId,{opacity:value}) }
            StudioComboBox { Layout.fillWidth:true;model:["Transparent · 透明背景","Translucent · 半透明背景","Solid · 纯色背景"];currentIndex:editor.settings.backgroundMode==="solid"?2:editor.settings.backgroundMode==="translucent"?1:0;onActivated:editor.model.edit(editor.selectedId,{backgroundMode:["transparent","translucent","solid"][currentIndex]}) }
            StudioTextField { Layout.fillWidth:true;placeholderText:"Background Color · 背景色 #RRGGBB";text:editor.settings.backgroundColor||"";onEditingFinished:editor.model.edit(editor.selectedId,{backgroundColor:text}) }
            StudioLabel { text:"Background Opacity · 背景透明度" }
            StudioSlider { Layout.fillWidth:true;from:0;to:1;value:editor.settings.backgroundOpacity??1;onPressedChanged:pressed?editor.model.beginEdit():editor.model.endEdit();onMoved:editor.model.edit(editor.selectedId,{backgroundOpacity:value}) }
            StudioLabel { Layout.fillWidth:true;wrapMode:Text.Wrap;color:appearance.colors.secondaryText;text:"Video pixels remain opaque until a real foreground mask is applied.\n普通视频像素仍不透明；背景透明设置只控制额外底板。";visible:editor.item?.layerName!==undefined&&editor.videoLayers.some(v=>v.id===editor.selectedId) }
        }
        ColumnLayout {
            Layout.fillWidth:true;visible:editor.videoLayers.some(v=>v.id===editor.selectedId)
            StudioLabel { text:"Video Fit / Crop · 视频适配与裁剪" }
            StudioComboBox { Layout.fillWidth:true;model:["Fit · 适应","Fill · 填充","Stretch · 拉伸","Original Size · 原始尺寸"];currentIndex:Math.max(0,["fit","fill","stretch","original"].indexOf(editor.settings.fit||"fit"));onActivated:editor.model.edit(editor.selectedId,{fit:["fit","fill","stretch","original"][currentIndex]}) }
            Repeater {
                model:["Left · 左","Top · 上","Right · 右","Bottom · 下"]
                delegate:RowLayout {
                    required property string modelData;required property int index
                    Layout.fillWidth:true
                    readonly property var crop:editor.settings.crop||editor.videoLayers.find(v=>v.id===editor.selectedId)?.crop||[0,0,1,1]
                    StudioLabel { text:modelData;Layout.fillWidth:true }
                    StudioTextField { Layout.preferredWidth:92;text:((index<2?parent.crop[index]:1-parent.crop[index])*100).toFixed(2);validator:DoubleValidator{bottom:0;top:99;locale:"C"} onEditingFinished:{const crop=parent.crop.slice();crop[index]=index<2?Number(text)/100:1-Number(text)/100;if(crop[2]>crop[0]&&crop[3]>crop[1])editor.model.edit(editor.selectedId,{crop:crop})} }
                }
            }
            StudioButton { text:"Reset Crop · 重置裁剪";onClicked:editor.model.edit(editor.selectedId,{crop:[0,0,1,1]}) }
        }
        StudioButton { text:"Visibility / Keyframes · 显示与关键帧";Layout.fillWidth:true;onClicked:timeFields.visible=!timeFields.visible }
        ColumnLayout {
            id:timeFields;visible:false;Layout.fillWidth:true
            StudioLabel { Layout.fillWidth:true;wrapMode:Text.Wrap;text:"Ranges in experiment seconds · 实验秒区间\n1–2; 4–6; 8.125–10.250" }
            StudioTextField { Layout.fillWidth:true;placeholderText:"Empty = all times · 留空始终显示";text:(editor.settings.visibilityRanges||[]).map(r=>r.join("–")).join("; ");onEditingFinished:{const ranges=[];let ok=true;for(const part of text.split(";")){if(!part.trim().length)continue;const r=part.trim().split(/[–,~]/).map(Number);if(r.length!==2||!isFinite(r[0])||!isFinite(r[1])||r[0]<0||r[1]<=r[0]){ok=false;break}ranges.push(r)}if(ok)editor.model.edit(editor.selectedId,{visibilityRanges:ranges,visibilityRangesEnabled:ranges.length>0})} }
            StudioButton { text:"Add Keyframe · 当前时刻添加关键帧";Layout.fillWidth:true;onClicked:editor.model.addKeyframe(editor.selectedId,controller.position,Object.assign({},editor.frame,{rotation:editor.evaluated.rotation??0,opacity:editor.evaluated.opacity??1})) }
            Repeater {
                model:editor.settings.keyframes||[]
                delegate:RowLayout { required property var modelData;StudioButton{text:modelData.time.toFixed(3)+" s";onClicked:controller.seek(modelData.time)}StudioButton{text:"−";onClicked:editor.model.removeKeyframe(editor.selectedId,modelData.time)} }
            }
        }
    }
}

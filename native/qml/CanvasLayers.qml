import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id:panel
    readonly property bool ownsDeleteKey: true
    required property var controller
    readonly property var canvas:controller.canvas
    signal layerSelected(string id)
    spacing:5
    StudioLabel { text:"Layers · 画布图层";font.bold:true }
    ListView {
        id:list;objectName:"canvasLayersList";Layout.fillWidth:true;Layout.preferredHeight:Math.max(100,Math.min(360,contentHeight));clip:true
        model:panel.canvas.layers;boundsBehavior:Flickable.StopAtBounds
        Keys.onDeletePressed: panel.canvas.removeSelected()
        ScrollBar.vertical:StudioScrollBar{}
        delegate:Rectangle {
            id:row
            required property var modelData
            required property int index
            width:list.width;height:36;radius:5
            color:panel.canvas.selection.indexOf(modelData.id)>=0?appearance.colors.selection:appearance.colors.transparent
            border.width:rowMove.pressed?1:0;border.color:appearance.colors.accent
            RowLayout {
                anchors.fill:parent;anchors.margins:3;spacing:3
                StudioButton { text:row.modelData.visible===false?"○":"●";implicitWidth:26;Accessible.name:"Visibility · 图层显隐";onClicked:panel.canvas.edit(row.modelData.id,{visible:row.modelData.visible===false}) }
                StudioLabel { Layout.fillWidth:true;text:row.modelData.name;elide:Text.ElideMiddle }
                StudioButton { text:row.modelData.locked?"▣":"◇";implicitWidth:26;Accessible.name:"Lock · 锁定图层";onClicked:panel.canvas.edit(row.modelData.id,{locked:!row.modelData.locked}) }
            }
            MouseArea {
                id:rowMove;x:31;width:Math.max(0,parent.width-62);height:parent.height
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape:pressed?Qt.ClosedHandCursor:Qt.OpenHandCursor
                property real initialY
                onPressed:mouse=>{list.forceActiveFocus();initialY=mapToItem(list,mouse.x,mouse.y).y;if(mouse.button!==Qt.RightButton||panel.canvas.selection.indexOf(row.modelData.id)<0)panel.canvas.select(row.modelData.id,!!(mouse.modifiers&(Qt.ControlModifier|Qt.ShiftModifier)));panel.layerSelected(row.modelData.id);if(mouse.button===Qt.RightButton)layerMenu.popup()}
                onReleased:mouse=>{if(mouse.button!==Qt.LeftButton)return;const p=mapToItem(list,mouse.x,mouse.y);if(Math.abs(p.y-initialY)>5)panel.canvas.reorder(row.modelData.id,Math.floor((p.y+list.contentY)/36))}
            }
        }
    }
    Menu {
        id: layerMenu; palette: StudioPalette {}
        MenuItem { text: "Duplicate · 复制"; onTriggered: panel.canvas.duplicate(panel.canvas.selectedId) }
        MenuItem { text: "Bring to Front · 置顶"; onTriggered: panel.canvas.moveLayer(panel.canvas.selectedId,-2) }
        MenuItem { text: "Send to Back · 置底"; onTriggered: panel.canvas.moveLayer(panel.canvas.selectedId,2) }
        MenuItem { text: "Group · 分组"; onTriggered: panel.canvas.groupSelection() }
        MenuItem { text: "Ungroup · 取消分组"; onTriggered: panel.canvas.ungroupSelection() }
        MenuItem { text: "Remove · 删除选中图层"; onTriggered: panel.canvas.removeSelected() }
    }
    RowLayout {
        Layout.fillWidth:true;enabled:panel.canvas.selection.length>0
        StudioButton { Layout.fillWidth:true;Layout.minimumWidth:0;text:"↑";Accessible.name:"Move Forward · 上移一层";onClicked:panel.canvas.moveLayer(panel.canvas.selectedId,-1) }
        StudioButton { Layout.fillWidth:true;Layout.minimumWidth:0;text:"↓";Accessible.name:"Move Backward · 下移一层";onClicked:panel.canvas.moveLayer(panel.canvas.selectedId,1) }
        StudioButton { Layout.fillWidth:true;Layout.minimumWidth:0;text:"Copy";Accessible.name:"Duplicate · 复制图层";onClicked:panel.canvas.duplicate(panel.canvas.selectedId) }
        StudioButton { Layout.fillWidth:true;Layout.minimumWidth:0;text:"−";Accessible.name:"Delete · 删除图层";onClicked:panel.canvas.removeSelected() }
    }
    GridLayout {
        columns: 2
        Layout.fillWidth:true;enabled:panel.canvas.selection.length>0
        StudioButton { Layout.fillWidth:true;text:"Front · 顶";onClicked:panel.canvas.moveLayer(panel.canvas.selectedId,-2) }
        StudioButton { Layout.fillWidth:true;text:"Back · 底";onClicked:panel.canvas.moveLayer(panel.canvas.selectedId,2) }
        StudioButton { Layout.fillWidth:true;text:"Group · 分组";onClicked:panel.canvas.groupSelection() }
        StudioButton { Layout.fillWidth:true;text:"Ungroup · 解组";onClicked:panel.canvas.ungroupSelection() }
    }
}

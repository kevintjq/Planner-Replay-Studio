import QtQuick

Rectangle {
    id: canvas
    required property var controller
    property bool interactive: true
    property var layerItems: ({})
    property var guideX: []
    property var guideY: []
    property bool showSafeArea: false
    signal layerSelected(string id)
    function registerLayer(id,item) { if(!id.length)return; const next=Object.assign({},layerItems);next[id]=item;layerItems=next }
    function unregisterLayer(id,item) { if(layerItems[id]!==item)return;const next=Object.assign({},layerItems);delete next[id];layerItems=next }
    function selectionFrames() {
        const rows=[]
        for(const id of controller.canvas.selection) {
            const item=layerItems[id]
            if(item && !item.layerSettings.locked)rows.push({id:id,x:item.x/width,y:item.y/height,width:item.width/width,height:item.height/height})
        }
        return rows
    }
    function clearGuides() { guideX=[];guideY=[] }
    function snapDelta(id,rows,dx,dy) {
        clearGuides()
        if(!controller.canvas.snapping || !rows.length)return Qt.point(dx,dy)
        const row=rows.find(r=>r.id===id) || rows[0], xs=[0,.5,1],ys=[0,.5,1]
        for(const key of Object.keys(layerItems)) {
            if(rows.some(r=>r.id===key))continue
            const item=layerItems[key]
            if(!item || !item.visible)continue
            xs.push(item.x/width,(item.x+item.width/2)/width,(item.x+item.width)/width)
            ys.push(item.y/height,(item.y+item.height/2)/height,(item.y+item.height)/height)
        }
        let bx=6/width,by=6/height,nx=dx,ny=dy,gx=null,gy=null
        for(const edge of [row.x,row.x+row.width/2,row.x+row.width])for(const target of xs)if(Math.abs(edge+dx-target)<bx){bx=Math.abs(edge+dx-target);nx=target-edge;gx=target}
        for(const edge of [row.y,row.y+row.height/2,row.y+row.height])for(const target of ys)if(Math.abs(edge+dy-target)<by){by=Math.abs(edge+dy-target);ny=target-edge;gy=target}
        guideX=gx===null?[]:[gx];guideY=gy===null?[]:[gy]
        return Qt.point(nx,ny)
    }
    function nudge(dx,dy) {
        const rows=selectionFrames();controller.canvas.beginEdit()
        for(const row of rows)controller.canvas.edit(row.id,{x:row.x+dx/controller.canvas.outputWidth,y:row.y+dy/controller.canvas.outputHeight,width:row.width,height:row.height})
        controller.canvas.endEdit()
    }
    Keys.onPressed: event => {
        if(!interactive || captureActive)return
        const step=(event.modifiers&Qt.ShiftModifier)?10:1
        if(event.key===Qt.Key_Delete||event.key===Qt.Key_Backspace)controller.canvas.removeSelected()
        else if(event.key===Qt.Key_Left)nudge(-step,0)
        else if(event.key===Qt.Key_Right)nudge(step,0)
        else if(event.key===Qt.Key_Up)nudge(0,-step)
        else if(event.key===Qt.Key_Down)nudge(0,step)
        else if(event.key===Qt.Key_Escape)controller.canvas.clearSelection()
        else{event.accepted=false;return}event.accepted=true
    }
    property var captureOptions: ({})
    readonly property bool captureActive: Object.keys(captureOptions).length > 0
    function includesLayer(id) { return !captureActive || captureOptions.scope !== "layers" || (captureOptions.layerIds ?? []).indexOf(id) >= 0 }
    clip: true
    MouseArea { anchors.fill:parent;z:-1000;enabled:canvas.interactive&&!canvas.captureActive;onClicked: { canvas.forceActiveFocus();canvas.controller.canvas.clearSelection() } }
    Repeater { model:canvas.interactive&&!canvas.captureActive?canvas.guideX:[];Rectangle { required property var modelData;x:modelData*canvas.width;width:1;height:canvas.height;z:200000;color:appearance.colors.accent } }
    Repeater { model:canvas.interactive&&!canvas.captureActive?canvas.guideY:[];Rectangle { required property var modelData;y:modelData*canvas.height;height:1;width:canvas.width;z:200000;color:appearance.colors.accent } }
    Rectangle { visible:canvas.showSafeArea&&canvas.interactive&&!canvas.captureActive;z:90000;x:parent.width*.05;y:parent.height*.05;width:parent.width*.9;height:parent.height*.9;color:"transparent";border.color:appearance.colors.secondaryText }

}

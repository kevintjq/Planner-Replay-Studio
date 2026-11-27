import QtQuick

Window {
    width: 1280
    height: 720
    minimumWidth: 1280
    minimumHeight: 720
    maximumWidth: 1280
    maximumHeight: 720
    title: "Planner Replay Studio · 导出"
    visible: true

    ReplayCanvas {
        objectName: "batchCanvas"
        anchors.fill: parent
        controller: replay
        exporter: movie
        interactive: false
    }
}

// Spear.Widgets — EyeDiagram: TraceSource(2 シンボル幅のトレース)を重ね描きするアイパターン (要件 §9.3)
// 判定点(1/4, 3/4)の縦線と、levels に与えたレベルの横線を引く。値の意味(±1/±3 等)は App が決める。
import QtQuick
import Spear.Theme
import Spear.AppFw

Item {
    id: root
    required property TraceSource source
    property string title: "EYE"
    property double yMin: -4.5
    property double yMax: 4.5
    property var levels: [-3, -1, 1, 3]         // 横の参照線
    property var decisionPoints: [0.25, 0.75]  // トレース長に対する判定点の位置
    property string unit: ""
    readonly property int traces: eye.traces
    readonly property int axisW: 44

    Rectangle { anchors.fill: parent; color: Theme.bg; border.color: Theme.line; border.width: 1 }
    Text { x: Theme.pad; y: 4; text: root.title; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
    Text { anchors.right: parent.right; anchors.rightMargin: Theme.pad; y: 4
           text: eye.traces + " tr  " + (root.source ? root.source.tracesPerSecond.toFixed(0) + "/s" : "")
           color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
    Item {
        id: plot
        x: root.axisW; y: 24; width: parent.width - root.axisW - Theme.pad; height: parent.height - y - Theme.pad
        // 参照レベル(横線)
        Repeater {
            model: root.levels
            delegate: Item {
                required property var modelData
                readonly property double yy: plot.height * (1 - (modelData - root.yMin) / (root.yMax - root.yMin))
                Rectangle { x: 0; y: Math.round(parent.yy); width: plot.width; height: 1; color: modelData === 0 ? Theme.gridMajor : Theme.grid }
                Text { x: -root.axisW + 4; y: Math.round(parent.yy) - height / 2; width: root.axisW - 8; horizontalAlignment: Text.AlignRight
                       text: (modelData > 0 ? "+" : "") + modelData + root.unit; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            }
        }
        // 判定点(縦線)
        Repeater {
            model: root.decisionPoints
            delegate: Rectangle { required property var modelData; x: Math.round(plot.width * modelData); y: 0; width: 1; height: plot.height; color: Theme.amber; opacity: 0.7 }
        }
        Rectangle { x: Math.round(plot.width / 2); y: 0; width: 1; height: plot.height; color: Theme.gridMajor }
        EyeDiagramItem {
            id: eye
            anchors.fill: parent
            source: root.source
            yMin: root.yMin; yMax: root.yMax
            traceColor: Theme.trace; recentColor: "#c8ffc8"
        }
    }
}

// Spear.Widgets — SpectrumView: Spectrum + Waterfall の再利用部品 (要件 §9.3)
// 任意の ViewSource(= 任意の stream の view processor)に接続できる。App のページはこれを置くだけ。
// 上: 読み出し行、中: スペクトラム(グリッド付き)、下: ウォーターフォール。
import QtQuick
import Spear.Theme
import Spear.AppFw
import Spear.Input

Item {
    id: page
    required property ViewSource source
    property double centerFreq: 0          // 表示用(軸ラベル・CENTER 欄)。App が Core の状態を渡す
    property bool maxHold: true
    property bool centerEditable: true
    property double markerHz: NaN          // 中心からのオフセット位置にマーカー線(選局チャネル等)
    property string markerLabel: ""
    // 複数マーカー: [{hz, label, color}] (多チャネル App 用。label/color は省略可)
    property var markers: []
    property bool compact: false           // 狭い場所用: 読み出し行を省き、周波数軸ラベルを間引く
    signal askFreq()
    signal askRef()
    readonly property double dbMin: source ? source.dbMin : -110
    readonly property double dbMax: source ? source.dbMax : -20
    readonly property int divisions: 10
    readonly property double span: spec.sampleRate > 0 ? spec.sampleRate : 0
    readonly property double peakDb: spec.peakDb
    readonly property double noiseFloorDb: spec.noiseFloorDb

    // ---- 読み出し行 ----
    Row {
        id: readouts
        visible: !page.compact
        x: Theme.pad; y: 6; spacing: Theme.pad
        ValueField { label: "CENTER"; value: Theme.fmtFreq(page.centerFreq); width: 250; height: 52; editable: page.centerEditable; onActivated: page.askFreq() }
        Readout { label: "SPAN"; value: Theme.fmtRate(page.span).replace(" Msps", " MHz"); width: 170 }
        Readout { label: "RBW"; value: spec.sampleRate > 0 ? ((spec.sampleRate / 1024) / 1e3).toFixed(2) + " kHz" : "--"; width: 150 }
        ValueField { label: "REF / DIV"; value: page.dbMax.toFixed(0) + " / " + ((page.dbMax - page.dbMin) / page.divisions).toFixed(0) + " dB"; width: 180; height: 52
                     valueColor: Theme.text; onActivated: page.askRef() }
        Readout { label: "PEAK"; value: spec.peakDb > -900 ? spec.peakDb.toFixed(1) + " dBFS" : "--"; valueColor: Theme.green; width: 180 }
        Readout { label: "PEAK OFFSET"; value: spec.peakDb > -900 ? Theme.fmtHz(spec.peakOffsetHz) : "--"; width: 190 }
        Readout { label: "NOISE FLOOR"; value: spec.noiseFloorDb > -900 ? spec.noiseFloorDb.toFixed(1) + " dBFS" : "--"; width: 190 }
        Readout { label: "AVG"; value: page.source ? page.source.averaging.toString() : "-"; width: 80 }
        Readout { label: "FPS"; value: spec.fps.toFixed(0); width: 80 }
        Rectangle {
            width: 110; height: 52; color: spec.discontinuity ? Theme.red : "transparent"; border.color: Theme.lineDim; border.width: 1
            Text { anchors.centerIn: parent; text: "DISC"; color: spec.discontinuity ? "#000000" : Theme.lineDim
                   font.family: Theme.mono; font.pixelSize: Theme.fsLarge; font.bold: true }
        }
    }

    // ---- スペクトラム ----
    Item {
        id: specArea
        x: 0; y: page.compact ? 4 : readouts.y + readouts.height + 6
        width: parent.width; height: Math.round((parent.height - y) * 0.42)
        readonly property int axisW: 92
        readonly property int axisH: 26
        Item {
            id: plot
            x: specArea.axisW; y: 4
            width: specArea.width - specArea.axisW - Theme.pad; height: specArea.height - specArea.axisH - 4
            Rectangle { anchors.fill: parent; color: Theme.bg; border.color: Theme.line; border.width: 1 }
            // グリッド: 縦横 10 分割、中央線は明るく
            Repeater {
                model: page.divisions - 1
                delegate: Rectangle { required property int index; x: 0; y: Math.round(plot.height * (index + 1) / page.divisions); width: plot.width; height: 1
                                      color: (index + 1) === page.divisions / 2 ? Theme.gridMajor : Theme.grid }
            }
            Repeater {
                model: page.divisions - 1
                delegate: Rectangle { required property int index; y: 0; x: Math.round(plot.width * (index + 1) / page.divisions); width: 1; height: plot.height
                                      color: (index + 1) === page.divisions / 2 ? Theme.gridMajor : Theme.grid }
            }
            // マーカー(例: 復調チャネルの位置)。琥珀の縦線 + ラベル
            Rectangle {
                visible: !isNaN(page.markerHz) && page.span > 0
                x: Math.round(plot.width * (0.5 + page.markerHz / page.span)); y: 0; width: 2; height: plot.height; color: Theme.amber
                Text { x: 4; y: 2; text: page.markerLabel; color: Theme.amber; font.family: Theme.mono; font.pixelSize: Theme.fsSmall; font.bold: true }
            }
            Repeater {
                model: page.markers
                delegate: Rectangle {
                    required property var modelData
                    visible: page.span > 0
                    x: Math.round(plot.width * (0.5 + modelData.hz / page.span)); y: 0; width: 1; height: plot.height
                    color: modelData.color !== undefined ? modelData.color : Theme.cyan; opacity: 0.8
                    Text { x: 3; y: plot.height - height - 2; text: modelData.label !== undefined ? modelData.label : ""
                           color: parent.color; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                }
            }
            SpectrumItem {
                id: spec
                anchors.fill: parent; anchors.margins: 1
                source: page.source
                dbMin: page.dbMin; dbMax: page.dbMax
                maxHold: page.maxHold
                traceColor: Theme.trace; maxHoldColor: Theme.maxHold
            }
        }
        // dB 軸(プロットが低いときは 1 つおき)
        Repeater {
            model: page.divisions + 1
            delegate: Text {
                required property int index
                visible: plot.height >= 260 || index % 2 === 0
                x: 0; width: specArea.axisW - 6; horizontalAlignment: Text.AlignRight
                y: plot.y + Math.round(plot.height * index / page.divisions) - height / 2
                text: (page.dbMax - (page.dbMax - page.dbMin) * index / page.divisions).toFixed(0)
                color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall
            }
        }
        // 周波数軸(中心からのオフセット)
        Repeater {
            model: page.divisions + 1
            delegate: Text {
                required property int index
                readonly property double span: page.span
                visible: !page.compact || index % 5 === 0
                x: plot.x + Math.round(plot.width * index / page.divisions) - width / 2
                y: plot.y + plot.height + 4
                text: index === page.divisions / 2 ? Theme.fmtFreq(page.centerFreq) : Theme.fmtHz(-span / 2 + span * index / page.divisions)
                color: index === page.divisions / 2 ? Theme.text : Theme.textDim
                font.family: Theme.mono; font.pixelSize: Theme.fsSmall
            }
        }
    }

    // ---- ウォーターフォール ----
    Item {
        x: specArea.axisW; y: specArea.y + specArea.height + 4
        width: specArea.width - specArea.axisW - Theme.pad; height: parent.height - y - 4
        Rectangle { anchors.fill: parent; color: Theme.bg; border.color: Theme.line; border.width: 1 }
        WaterfallItem { id: wf; anchors.fill: parent; anchors.margins: 1; source: page.source }
        Text { anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 6
               text: wf.rowsPerSecond > 0 ? (wf.rowsVisible / wf.rowsPerSecond).toFixed(0) + " s" : ""
               color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
        // 中心線
        Rectangle { x: Math.round(parent.width / 2); y: 1; width: 1; height: parent.height - 2; color: Theme.gridMajor; opacity: 0.6 }
    }
    // 経過秒ラベル(左)
    Text { x: 8; y: specArea.y + specArea.height + 8; text: "0 s"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
}

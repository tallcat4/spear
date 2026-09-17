// ADS-B のページ (1920×1200)
//   上 : radio.rx のスペクトラム(8 MHz、1090 MHz にマーカー。アンテナと利得の確認用)
//   中 : 左 航空機表(タップで選択)/ 右 ミニマップ(埋め込み地図、位置のある全機が収まるよう自動フィット。ドラッグ / ピンチで手動、FIT で戻す)
//   下 : 最後に受理したフレームの振幅窓(プリアンブル 8 µs + データ)/ 読み出し(レート・機数・CRC・drops・DSP)
// LO / rate は App が決める(1090 MHz − loOffset、8 Msps)ので操作キーは置かない。GPS は無いので地図は機体の位置だけから決める。
import QtQuick
import Spear.Theme
import Spear.Widgets
import Spear.Input

Item {
    id: page
    required property var app
    required property var ui

    readonly property var st: app.stats
    readonly property var list: app.aircraft
    readonly property var sortKeys: ["seen", "call", "alt", "msgs"]

    readonly property var softKeys: [
        { label: "SORT " + app.sortKey.toUpperCase(), action: "sort" },
        { label: "MAP FIT", action: "fit", active: mapItem.autoFit },
        { label: "ZOOM −", action: "zoomout" }, { label: "ZOOM +", action: "zoomin" },
        { label: "FIX 1BIT", action: "fix", active: app.fixBits },
        { label: "CLEAR", action: "clear" }, { label: "DIAG", action: "diag" } ]
    function softKey(action) {
        switch (action) {
        case "sort": app.sortKey = sortKeys[(sortKeys.indexOf(app.sortKey) + 1) % sortKeys.length]; break
        case "fit": mapItem.autoFit = !mapItem.autoFit; break
        case "zoomout": mapItem.zoom(1.5); break
        case "zoomin": mapItem.zoom(1 / 1.5); break
        case "fix": app.fixBits = !app.fixBits; break
        case "clear": app.clearTable(); break
        case "diag": ui.showDiagnostics(); break
        }
    }
    function ageColor(a) { return a < 5 ? Theme.green : (a < 30 ? Theme.text : Theme.textDim) }
    function fmtAlt(v) { return v < 0 ? "--" : String(v) }
    function fmtNum(v, d) { return v < 0 ? "--" : v.toFixed(d) }

    // ---- 上: radio.rx ----
    SpectrumView {
        id: wide
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        height: Math.round(parent.height * 0.22)
        source: app.view; centerFreq: sys.centerFreq; centerEditable: false
        markerHz: 1090e6 - sys.centerFreq; markerLabel: "1090"
        onAskRef: ui.askRef(app.view.dbMax, (v) => { app.view.dbMin = v - 90; app.view.dbMax = v })
    }

    // ---- 中: 表 + 極座標 ----
    Item {
        id: middle
        anchors.top: wide.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: Theme.pad; anchors.topMargin: 6
        height: Math.round(page.height * 0.56)

        // 列定義(名前, 幅, キー, 右寄せ)
        readonly property var cols: [
            ["ICAO", 90, "hex", false], ["CALLSIGN", 120, "callsign", false], ["SQ", 66, "squawk", false], ["ALT ft", 90, "alt", true],
            ["GS kt", 76, "gs", true], ["TRK", 66, "trk", true], ["VR", 80, "vr", true], ["LAT", 110, "lat", true], ["LON", 116, "lon", true],
            ["MSGS", 70, "msgs", true], ["AGE", 60, "age", true], ["RSSI", 70, "rssi", true] ]
        function cell(row, c) {
            const k = c[2], v = row[k]
            switch (k) {
            case "alt": return row.ground ? "GND" : fmtAlt(v)
            case "gs": return fmtNum(v, 0)
            case "trk": return v < 0 ? "--" : v.toFixed(0) + (row.heading ? "h" : "")
            case "vr": return row.hasVr ? (v > 0 ? "+" : "") + v : "--"
            case "lat": return row.hasPos ? v.toFixed(4) : "--"
            case "lon": return row.hasPos ? v.toFixed(4) : "--"
            case "msgs": return v.toFixed(0)
            case "age": return v.toFixed(0) + "s"
            case "rssi": return v.toFixed(0)
            default: return v === "" ? "--" : String(v)
            }
        }

        Rectangle {
            id: tableBox
            x: 0; y: 0; width: 1030; height: parent.height
            color: Theme.panel; border.color: Theme.line; border.width: 1
            Row {
                id: header
                x: Theme.pad; y: 6; spacing: 0
                Repeater {
                    model: middle.cols
                    delegate: Text {
                        required property var modelData
                        width: modelData[1]; text: modelData[0]; horizontalAlignment: modelData[3] ? Text.AlignRight : Text.AlignLeft
                        color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall; rightPadding: 8
                    }
                }
            }
            Rectangle { x: 1; y: 28; width: parent.width - 2; height: 1; color: Theme.line }
            ListView {
                id: table
                x: Theme.pad; y: 32; width: parent.width - 2 * Theme.pad; height: parent.height - y - 6
                clip: true
                model: page.list
                boundsBehavior: Flickable.StopAtBounds
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    readonly property bool sel: modelData.icao === app.selectedIcao
                    width: table.width; height: 34
                    color: sel ? "#141414" : (index % 2 ? Theme.panel : Theme.bg)
                    border.color: sel ? Theme.amber : "transparent"; border.width: sel ? 1 : 0
                    Row {
                        x: 0; y: 6; spacing: 0
                        Repeater {
                            model: middle.cols
                            delegate: Text {
                                required property var modelData
                                readonly property var row: parent.parent.modelData
                                width: modelData[1]; horizontalAlignment: modelData[3] ? Text.AlignRight : Text.AlignLeft; rightPadding: 8
                                text: middle.cell(row, modelData)
                                color: modelData[2] === "age" ? page.ageColor(row.age)
                                     : (modelData[2] === "callsign" ? Theme.textBright : (modelData[2] === "hex" && row.hasPos ? Theme.green : Theme.text))
                                font.family: Theme.mono; font.pixelSize: Theme.fsBase
                            }
                        }
                    }
                    MouseArea { anchors.fill: parent; onClicked: app.selectedIcao = (app.selectedIcao === modelData.icao ? 0 : modelData.icao) }
                }
            }
            Text { anchors.centerIn: table; visible: page.list.length === 0; text: "NO AIRCRAFT"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsLarge }
        }

        // ミニマップ(北が上。位置のある全機を含むように自動フィット。地図は Natural Earth + OurAirports をバイナリに埋め込み)
        Rectangle {
            id: mapBox
            x: tableBox.width + Theme.pad; y: 0; width: parent.width - x; height: parent.height
            color: Theme.bg; border.color: Theme.line; border.width: 1
            clip: true
            MapItem {
                id: mapItem
                anchors.fill: parent; anchors.margins: 1
                aircraft: page.list
                selectedIcao: app.selectedIcao
                landColor: "#161616"; coastColor: Theme.line; lineColor: Theme.gridMajor; textColor: Theme.text; dimColor: Theme.textDim
                aircraftColor: Theme.green; selectedColor: Theme.amber; airportColor: Theme.cyan; fontFamily: Theme.mono
                onSelectedChanged: app.selectedIcao = selectedIcao
                // タッチ: ドラッグでパン、タップで選択、ピンチでズーム(操作すると自動フィットが切れる。FIT で戻す)
                PinchArea {
                    anchors.fill: parent
                    property real startSpan: 0
                    onPinchStarted: startSpan = mapItem.spanKm
                    onPinchUpdated: (p) => { if (p.scale > 0) mapItem.zoom((startSpan / p.scale) / mapItem.spanKm) }
                    MouseArea {
                        anchors.fill: parent
                        property real lx: 0; property real ly: 0; property real moved: 0
                        onPressed: (m) => { lx = m.x; ly = m.y; moved = 0 }
                        onPositionChanged: (m) => { if (!pressed) return; mapItem.pan(m.x - lx, m.y - ly); moved += Math.abs(m.x - lx) + Math.abs(m.y - ly); lx = m.x; ly = m.y }
                        onReleased: (m) => { if (moved < 8) { const i = mapItem.icaoAt(m.x, m.y); app.selectedIcao = (i === app.selectedIcao ? 0 : i) } }
                    }
                }
            }
            Text { x: Theme.pad; y: 4
                   text: "MAP  N up  " + (mapItem.autoFit ? "AUTO FIT" : "MANUAL") + "  " + mapItem.spanKm.toFixed(0) + " km  " + mapItem.centerLat.toFixed(2) + " " + mapItem.centerLon.toFixed(2)
                         + (mapItem.positioned === 0 ? "   (no position yet)" : "")
                   color: mapItem.autoFit ? Theme.textDim : Theme.amber; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            Text { anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 6; anchors.rightMargin: Theme.pad
                   text: mapItem.mapLoaded ? "Natural Earth · OurAirports" : "MAP DATA MISSING"; color: mapItem.mapLoaded ? Theme.textDim : Theme.red
                   font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
        }
    }

    // ---- 下: 振幅窓 + 読み出し ----
    Item {
        id: lower
        anchors.top: middle.bottom; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: Theme.pad; anchors.topMargin: 6
        EyeDiagram {
            id: pulses
            x: 0; y: 0; width: Math.round(parent.width * 0.40); height: parent.height
            source: app.trace
            title: "LAST FRAME  |x|  preamble 8 µs + " + (page.st.frames > 0 && app.lastFrame.hex !== undefined ? (app.lastFrame.hex.length * 4) + " bit PPM" : "PPM") + "  (120 µs, normalized)"
            yMin: 0; yMax: 1.1; levels: [0, 0.5, 1]; decisionPoints: [16 / 240]
        }
        Rectangle {
            x: pulses.width + Theme.pad; y: 0; width: parent.width - x; height: parent.height
            color: Theme.panel; border.color: Theme.line; border.width: 1
            Column {
                x: Theme.pad; y: Theme.pad; spacing: 6
                Row {
                    spacing: 6
                    Rectangle {
                        width: 150; height: 52; color: page.st.framesPerS > 0 ? Theme.green : (page.st.preamblesPerS > 0 ? Theme.cyan : Theme.gray)
                        Text { anchors.centerIn: parent; text: page.st.framesPerS > 0 ? "FRAMES" : (page.st.preamblesPerS > 0 ? "NOISE" : "IDLE"); color: "#000000"
                               font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
                    }
                    Readout { label: "FRAMES / s"; value: page.st.framesPerS.toFixed(0); width: 130; height: 52; valueColor: Theme.green }
                    Readout { label: "PREAMBLES / s"; value: page.st.preamblesPerS.toFixed(0); width: 150; height: 52 }
                    Readout { label: "AIRCRAFT / POS"; value: page.st.aircraft.toFixed(0) + " / " + page.st.withPos.toFixed(0); width: 160; height: 52; valueColor: Theme.textBright }
                    Readout { label: "ES / ALL-CALL / AP"; value: page.st.df17.toFixed(0) + " / " + page.st.df11.toFixed(0) + " / " + page.st.dfAp.toFixed(0); width: 240; height: 52 }
                    Readout { label: "CRC BAD / FIXED"; value: page.st.crcBad.toFixed(0) + " / " + page.st.fixed.toFixed(0); width: 170; height: 52 }
                }
                Row {
                    spacing: 6
                    Readout { label: "LAST FRAME"; width: 470; height: 52; valueSize: Theme.fsBase
                              value: app.lastFrame.hex !== undefined ? app.lastFrame.hex + "  DF" + app.lastFrame.df + " " + app.lastFrame.icao + (app.lastFrame.fixed ? " FIX" : "") : "--"
                              valueColor: Theme.text }
                    Readout { label: "RSSI / SNR dB"; value: app.lastFrame.rssi !== undefined ? app.lastFrame.rssi.toFixed(0) + " / " + app.lastFrame.snr.toFixed(0) : "--"; width: 150; height: 52 }
                    Readout { label: "POS OK / REJ"; value: page.st.positions.toFixed(0) + " / " + page.st.rejected.toFixed(0); width: 150; height: 52; valueColor: page.st.rejected > 0 ? Theme.amber : Theme.text }
                    Readout { label: "DROPS / DSP"; value: page.st.drops.toFixed(0) + " / " + (page.st.dspLoad * 100).toFixed(0) + "%"; width: 150; height: 52
                              valueColor: page.st.drops > 0 ? Theme.red : (page.st.dspLoad > 0.8 ? Theme.amber : Theme.text) }
                }
                Text { text: "LO " + Theme.fmtFreq(sys.centerFreq) + "  offset " + Theme.fmtHz(app.loOffsetHz) + "  8 Msps  4 samples/chip   events new/pos/lost " + page.st.eventsNew.toFixed(0) + "/" + page.st.eventsPos.toFixed(0) + "/" + page.st.eventsLost.toFixed(0)
                       color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            }
        }
    }
}

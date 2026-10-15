// JR TRAIN RADIO のページ (1920×1200)
//   上 : 帯域スペクトラム(jr_train.band 240 kS/s、チャネルマーカー付き)
//   中 : チャネルのタイル(フレーム同期・EVM・周波数誤差。タップで選択)
//   下 : 選択チャネルのコンスタレーション / 読み出し / 録音
// 受信の可否はフレーム同期(マーカー 2/3)で判断する。搬送波ロックは空きチャネルでも隣接波の漏れで立つことがあるので補助表示。
import QtQuick
import Spear.Theme
import Spear.Widgets
import Spear.Input

Item {
    id: page
    required property var app
    required property var ui

    readonly property var sel: app.selected
    readonly property var chans: app.channels

    // 帯域中心・LO オフセット・プリセットは site.conf(jr_train.preset / jr_train.lo_offset_hz)。画面からは変えない
    readonly property var softKeys: [
        { label: app.recording ? "STOP REC" : "REC", action: "rec", active: app.recording },
        null, null, null, null, null,
        { label: "DIAG", action: "diag" } ]
    function softKey(action) {
        switch (action) {
        case "rec": app.toggleRecording(); break
        case "diag": ui.showDiagnostics(); break
        }
    }
    function tileColor(c) {
        if (c.active) return Theme.green
        if (c.locked) return Theme.amber
        if (c.carrier) return Theme.cyan
        return Theme.gray
    }
    function stateText(c) {
        if (c.active) return "SYNC"
        if (c.locked) return "LOCK"
        return c.carrier ? "carrier" : "--"
    }
    readonly property var markers: {
        var m = []
        for (var i = 0; i < page.chans.length; ++i) {
            var c = page.chans[i]
            m.push({ hz: c.offset, label: "", color: i === app.selectedChannel ? Theme.amber : (c.active ? Theme.green : Theme.gridMajor) })
        }
        return m
    }

    // ---- 上: 帯域スペクトラム ----
    SpectrumView {
        id: band
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        height: Math.round(parent.height * 0.34)
        source: app.bandView; centerFreq: app.bandCenterHz; centerEditable: false
        markers: page.markers
        markerHz: app.channelOffsetHz(app.selectedChannel); markerLabel: page.sel.label !== undefined ? page.sel.label : ""
        onAskRef: ui.askRef(app.bandView.dbMax, (v) => { app.bandView.dbMin = v - 90; app.bandView.dbMax = v })
    }

    // ---- 中: チャネルのタイル(2 段)----
    Item {
        id: grid
        anchors.top: band.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: Theme.pad; anchors.rightMargin: Theme.pad
        readonly property int cols: Math.max(1, Math.ceil(page.chans.length / 2))
        readonly property real cw: (width - (cols - 1) * 4) / cols
        height: 2 * 82 + 6
        Repeater {
            model: page.chans
            delegate: Rectangle {
                required property var modelData
                required property int index
                readonly property bool selectedCh: index === app.selectedChannel
                x: (index % grid.cols) * (grid.cw + 4); y: Math.floor(index / grid.cols) * 82 + 6
                width: grid.cw; height: 76
                color: selectedCh ? "#141414" : Theme.panel
                border.color: selectedCh ? Theme.amber : Theme.line; border.width: selectedCh ? 2 : 1
                Text { x: 8; y: 3; text: modelData.label; color: selectedCh ? Theme.amber : Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
                Text { anchors.right: parent.right; anchors.rightMargin: 8; y: 5; text: page.stateText(modelData); color: page.tileColor(modelData)
                       font.family: Theme.mono; font.pixelSize: Theme.fsSmall; font.bold: modelData.active }
                Text { x: 8; y: 26; text: (modelData.freq / 1e6).toFixed(5); color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                Text { x: 8; y: 48
                       text: modelData.carrier ? (modelData.evm.toFixed(0) + "%  " + (modelData.freqErr >= 0 ? "+" : "") + modelData.freqErr.toFixed(0) + "Hz") : ""
                       color: modelData.active ? Theme.text : Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                Rectangle { x: 0; y: parent.height - 4; width: parent.width; height: 4; color: page.tileColor(modelData); visible: modelData.active || modelData.locked }
                MouseArea { anchors.fill: parent; onClicked: if (index !== app.selectedChannel) { Feedback.tap(); app.selectedChannel = index } }
            }
        }
    }

    // ---- 下: 選択チャネル ----
    Item {
        id: lower
        anchors.top: grid.bottom; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: Theme.pad; anchors.topMargin: 6

        ConstellationView {
            id: cons
            x: 0; y: 0; height: parent.height; width: Math.min(parent.height + 40, Math.round(parent.width * 0.36))
            points: app.constellation
            references: [0.7071, 0.7071, -0.7071, 0.7071, -0.7071, -0.7071, 0.7071, -0.7071]
            title: "CONSTELLATION  " + (page.sel.label !== undefined ? page.sel.label : "") + "  π/4-DQPSK 4800 Bd (differential)"
            info: page.sel.evm !== undefined && page.sel.carrier ? "EVM " + page.sel.evm.toFixed(1) + "%" : ""
        }
        Rectangle {
            x: cons.width + Theme.pad; y: 0; width: parent.width - x; height: parent.height
            color: Theme.panel; border.color: Theme.line; border.width: 1
            Column {
                x: Theme.pad; y: Theme.pad; spacing: 6
                Row {
                    spacing: 6
                    Rectangle {
                        width: 150; height: 52; color: page.sel.active ? Theme.green : (page.sel.locked ? Theme.amber : (page.sel.carrier ? Theme.cyan : Theme.gray))
                        Text { anchors.centerIn: parent; text: page.sel.active ? "FRAME SYNC" : (page.sel.locked ? "LOCKED" : (page.sel.carrier ? "CARRIER" : "IDLE")); color: "#000000"
                               font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
                    }
                    Readout { label: "CHANNEL"; value: page.sel.freq !== undefined ? Theme.fmtFreq(page.sel.freq) : "--"; width: 220; height: 52 }
                    Readout { label: "TYPE"; value: page.sel.service !== undefined ? page.sel.service.toUpperCase() : "--"; width: 120; height: 52 }
                    Readout { label: "FREQ ERR"; value: page.sel.carrier ? (page.sel.freqErr >= 0 ? "+" : "") + page.sel.freqErr.toFixed(1) + " Hz" : "--"; width: 150; height: 52 }
                    Readout { label: "EVM"; value: page.sel.carrier ? page.sel.evm.toFixed(1) + " %" : "--"; width: 120; height: 52 }
                }
                Row {
                    spacing: 6
                    Readout { label: "FRAMES / SYNCED"; value: page.sel.frames !== undefined ? page.sel.frames.toFixed(0) + " / " + page.sel.synced.toFixed(0) : "--"; width: 220; height: 52; valueColor: Theme.green }
                    Readout { label: "SYMBOLS (LOCKED)"; value: page.sel.symbols !== undefined ? page.sel.symbols.toFixed(0) + " (" + page.sel.lockedSymbols.toFixed(0) + ")" : "--"; width: 260; height: 52 }
                    Readout { label: "CARRIER ERR"; value: page.sel.carrierError !== undefined ? page.sel.carrierError.toFixed(3) : "--"; width: 150; height: 52 }
                    Readout { label: "EPOCH / RESETS"; value: page.sel.epoch !== undefined ? page.sel.epoch.toFixed(0) + " / " + page.sel.resets.toFixed(0) : "--"; width: 190; height: 52 }
                }
                Row {
                    spacing: 6
                    Readout { label: "PRESET"; value: app.presetId.toUpperCase() + "  " + app.numChannels + " ch"; width: 220; height: 52 }
                    Readout { label: "SYNC CH"; value: app.activeChannels + " / " + app.numChannels; width: 150; height: 52; valueColor: app.activeChannels > 0 ? Theme.green : Theme.text }
                    Readout { label: "DROPS / GAPS"; value: app.losslessDrops.toFixed(0) + " / " + app.discontinuities.toFixed(0); width: 170; height: 52
                              valueColor: app.losslessDrops > 0 ? Theme.red : Theme.text }
                    Readout { label: "DSP (" + app.dspThreads + " thr)"; value: (app.dspLoad * 100).toFixed(0) + " %"; width: 150; height: 52
                              valueColor: app.dspLoad > 0.8 ? Theme.red : Theme.text }
                }
                Row {
                    spacing: 6
                    Readout { label: "REC (radio.rx SigMF)"; width: 640; height: 52
                              value: app.recording ? ("● " + app.recordSeconds.toFixed(1) + " s  " + app.recordPath)
                                                   : (app.recordPath.length ? "last " + app.recordSeconds.toFixed(1) + " s  " + app.recordPath : "--")
                              valueSize: Theme.fsBase
                              valueColor: app.recording ? Theme.red : Theme.text }
                }
                Text { visible: app.recordError.length > 0; text: "REC ERROR  " + app.recordError; color: Theme.red; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                Text { visible: page.sel.note !== undefined && page.sel.note.length > 0; text: page.sel.note !== undefined ? page.sel.note : ""
                       color: Theme.textDim; font.pixelSize: Theme.fsSmall }
            }
        }
    }
}

// SLPR 20CH RX のページ (1920×1200)
//   上 : 20ch 俯瞰スペクトラム(slpr.band 400 kHz、チャネルマーカー付き。ICOM の 01–20)
//   中 : チャネル格子(2 段 × 10: 上段 01–10、下段 11–20。電力バーと閾値、開いていれば電力値。タップで選択)
//   下 : 選択チャネル — チャネル IQ スペクトラム / 読み出し(状態・電力・床・音声)
// STD-T98 MONITOR の縮小版: デジタルでも秘話でもないので、アイパターン・フレーム・CSM・鍵の表示が無い。
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

    // チャネル選択は格子のタップで行う。帯域中心・LO オフセットはチャネル表と App が決める値なので操作キーは置かない。
    readonly property var softKeys: [
        { label: "ALL CH AUDIO", action: "all", active: app.allChannelAudio },
        { label: "SQUELCH", action: "sq" }, { label: "MUTE", action: "mute", active: app.mute },
        null, null, null, { label: "DIAG", action: "diag" } ]
    function softKey(action) {
        switch (action) {
        case "all": app.allChannelAudio = !app.allChannelAudio; break
        case "sq": ui.askLevel("SQUELCH  MARGIN ABOVE NOISE FLOOR", "dB", 0, 40, app.squelchMarginDb, (v) => app.squelchMarginDb = v); break
        case "diag": ui.showDiagnostics(); break
        case "mute": app.mute = !app.mute; break
        }
    }
    function chColor(c) { return c.open ? Theme.green : Theme.gray }
    // チャネルマーカー(帯域中心からのオフセット)。選択 ch は琥珀。ch11 と ch12 の間は 25 kHz 空く
    readonly property var markers: {
        var m = []
        for (var i = 0; i < app.numChannels; ++i)
            m.push({ hz: app.channelOffsetHz(i), label: app.channelName(i),
                     color: i === app.selectedChannel ? Theme.amber : Theme.gridMajor })
        return m
    }

    // ---- 上: 俯瞰スペクトラム ----
    SpectrumView {
        id: band
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        height: Math.round(parent.height * 0.42)
        source: app.bandView; centerFreq: app.bandCenterHz; centerEditable: false
        markers: page.markers
        markerHz: app.channelOffsetHz(app.selectedChannel); markerLabel: "CH " + app.channelName(app.selectedChannel)
        onAskRef: ui.askRef(app.bandView.dbMax, (v) => { app.bandView.dbMin = v - 90; app.bandView.dbMax = v })
    }

    // ---- 中: チャネル格子(上段 単信 11ch の 01–10、下段 11 と 単信 9ch の 12–20)----
    Item {
        id: grid
        anchors.top: band.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: Theme.pad; anchors.rightMargin: Theme.pad
        height: 2 * 100 + 6
        readonly property int cols: 10
        readonly property real cw: (width - (cols - 1) * 4) / cols
        Repeater {
            model: page.chans
            delegate: Rectangle {
                required property var modelData
                required property int index
                readonly property bool selectedCh: index === app.selectedChannel
                x: (index % grid.cols) * (grid.cw + 4); y: Math.floor(index / grid.cols) * 100 + 6
                width: grid.cw; height: 94
                color: selectedCh ? "#141414" : Theme.panel
                border.color: selectedCh ? Theme.amber : Theme.line; border.width: selectedCh ? 2 : 1
                Text { x: 10; y: 6; text: "CH " + modelData.name; color: selectedCh ? Theme.amber : Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsLarge; font.bold: true }
                // 開いているときだけ緑の電力値(dBFS)
                Text { anchors.right: parent.right; anchors.rightMargin: 10; y: 10; text: modelData.open ? modelData.power.toFixed(0) : ""; color: page.chColor(modelData); font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
                Text { x: 10; y: 40; text: (modelData.freq / 1e6).toFixed(4) + " MHz"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                // 電力バー(-100..-20 dB)。琥珀の目印は開く閾値 = 床(全 ch 中央値)+ 余裕。床が動けば目印も動く
                Rectangle {
                    x: 10; y: 66; width: parent.width - 20; height: 14; color: Theme.bg; border.color: Theme.lineDim; border.width: 1
                    Rectangle { x: 1; y: 1; height: 12; width: Math.round((parent.width - 2) * Math.max(0, Math.min(1, (modelData.power + 100) / 80))); color: page.chColor(modelData) }
                    Rectangle { x: Math.round((parent.width - 2) * Math.max(0, Math.min(1, (modelData.threshold + 100) / 80))); y: -3; width: 2; height: 20; color: Theme.amber; opacity: 0.8; visible: modelData.threshold > -900 }
                }
                MouseArea { anchors.fill: parent; onClicked: if (index !== app.selectedChannel) { Feedback.tap(); app.selectedChannel = index } }
            }
        }
    }

    // ---- 下: 選択チャネル ----
    Item {
        id: lower
        anchors.top: grid.bottom; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: Theme.pad; anchors.topMargin: 6

        // チャネル IQ スペクトラム(観測点: slpr.channel 12.5 kHz)
        Item {
            id: chanSpec
            x: 0; y: 0; width: Math.round(parent.width * 0.30); height: parent.height
            Text { x: Theme.pad; y: 4; text: "CHANNEL IQ  slpr.channel  12.5 kHz"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            SpectrumView {
                anchors.fill: parent; anchors.topMargin: 20
                compact: true
                source: app.channelView; centerFreq: app.channelFreqHz(app.selectedChannel); centerEditable: false
                // dB レンジは帯域表示に追従(App 側で同期)。REF は帯域側を編集する
                onAskRef: ui.askRef(app.bandView.dbMax, (v) => { app.bandView.dbMin = v - 90; app.bandView.dbMax = v })
            }
        }
        // 読み出し
        Rectangle {
            x: chanSpec.x + chanSpec.width + Theme.pad; y: 0; width: parent.width - x; height: parent.height
            color: Theme.panel; border.color: Theme.line; border.width: 1
            Column {
                x: Theme.pad; y: Theme.pad; spacing: 8
                Row {
                    spacing: 6
                    Rectangle {
                        width: 150; height: 56; color: page.sel.open ? Theme.green : Theme.gray
                        Text { anchors.centerIn: parent; text: page.sel.open ? "CARRIER" : "IDLE"; color: "#000000"
                               font.family: Theme.mono; font.pixelSize: Theme.fsLarge; font.bold: true }
                    }
                    Readout { label: "CHANNEL"; value: "CH " + page.sel.name; width: 150; height: 56; valueColor: Theme.amber }
                    Readout { label: "FREQUENCY"; value: Theme.fmtFreq(page.sel.freq); width: 250; height: 56; valueColor: Theme.textBright }
                    Readout { label: "GROUP"; value: page.sel.group; width: 210; height: 56 }
                    Readout { label: "POWER"; value: page.sel.power > -900 ? page.sel.power.toFixed(1) + " dB" : "--"; width: 150; height: 56 }
                    Readout { label: "FLOOR / SQL"; value: page.sel.floor > -900 ? page.sel.floor.toFixed(0) + " / +" + app.squelchMarginDb.toFixed(0) + " dB" : "--"; width: 190; height: 56 }
                }
                Row {
                    spacing: 6
                    Readout { label: "OPENS / TIME"; value: page.sel.opens.toFixed(0) + " / " + page.sel.openS.toFixed(0) + " s"; width: 200; height: 56 }
                    Readout { label: "OPEN CH"; value: String(app.openChannels) + " / " + app.numChannels; width: 150; height: 56; valueColor: app.openChannels > 0 ? Theme.green : Theme.text }
                    Readout { label: "AUDIO"; value: app.audioError.length ? "ERROR" : (app.mute ? "MUTE" : (app.allChannelAudio ? "ALL CH" : "CH " + page.sel.name)); width: 150; height: 56
                              valueColor: app.audioError.length ? Theme.red : Theme.text }
                    Readout { label: "UNDER / OVER"; value: app.audioUnderruns.toFixed(0) + " / " + app.audioOverruns.toFixed(0); width: 170; height: 56
                              valueColor: (app.audioUnderruns > 0 || app.audioOverruns > 0) ? Theme.amber : Theme.text }
                    Readout { label: "LATENCY"; value: app.audioLatencyMs.toFixed(0) + " ms"; width: 140; height: 56 }
                    Readout { label: "DROPS / DSP"; value: app.losslessDrops.toFixed(0) + " / " + (app.dspLoad * 100).toFixed(0) + "%"; width: 170; height: 56
                              valueColor: app.losslessDrops > 0 ? Theme.red : Theme.text }
                }
                Row {
                    spacing: Theme.pad * 3
                    LevelMeter { label: "CH " + page.sel.name + " POWER (dBFS)"; value: page.sel.power; minDb: -100; maxDb: 0; threshold: page.sel.threshold > -900 ? page.sel.threshold : NaN; width: 560 }
                    LevelMeter { label: "AUDIO OUT (dBFS)"; value: app.mixDb; minDb: -60; maxDb: 0; barColor: Theme.cyan; width: 480 }
                }
                Text { visible: app.audioError.length > 0; text: "AUDIO ERROR  " + app.audioError; color: Theme.red; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            }
        }
    }
}

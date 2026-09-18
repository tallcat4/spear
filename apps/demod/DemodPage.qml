// FM / AM RX のページ。上: radio.rx の spectrum、中: channel IQ の spectrum(後付けの観測点)、下: 復調パネル
import QtQuick
import Spear.Theme
import Spear.Widgets
import Spear.Input

Item {
    id: page
    required property var app
    required property var ui

    // RX 周波数は導出値(Core の center + App の channel offset)。ここで計算し、コピーは持たない
    readonly property double rxFreq: sys.centerFreq + app.channelOffsetHz

    readonly property var softKeys: [
        { label: "RX FREQ", action: "freq" }, { label: "MODE " + app.mode, action: "mode" },
        { label: "SQUELCH", action: "sq" }, { label: "LO OFS", action: "lo" },
        { label: "MUTE", action: "mute", active: app.mute } ]
    function softKey(action) {
        switch (action) {
        case "freq": ui.askFreq(page.rxFreq, (v) => app.tuneRx(v)); break
        case "lo": ui.askOffset(app.loOffsetHz, (v) => app.loOffsetHz = v); break
        case "mode": app.cycleMode(); break
        case "sq": ui.askRef(app.squelchDb, (v) => app.squelchDb = v); break
        case "mute": app.mute = !app.mute; break
        }
    }

    SpectrumView {
        id: wide
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        height: Math.round(parent.height * 0.40)
        source: app.view; centerFreq: sys.centerFreq; centerEditable: false   // これは LO。選局は RX FREQ で
        markerHz: app.channelOffsetHz; markerLabel: "RX"
        onAskRef: ui.askRef(app.view.dbMax, (v) => { app.view.dbMin = v - 90; app.view.dbMax = v })
    }
    // 観測点 2: App 内部の channel IQ(240 kHz)。DSP コードに触れずに Stream Bus から取っている (§0)
    Item {
        id: chanBox
        anchors.top: wide.bottom; anchors.left: parent.left; anchors.right: parent.right
        height: Math.round(parent.height * 0.33)
        Text { x: Theme.pad * 2; y: 2; text: "CHANNEL IQ  demod.channel  240 kHz  (observation point on the app's internal stream)"
               color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
        SpectrumView {
            anchors.fill: parent; anchors.topMargin: 20
            source: app.channelView; centerFreq: page.rxFreq; centerEditable: false
            onAskRef: ui.askRef(app.channelView.dbMax, (v) => { app.channelView.dbMin = v - 90; app.channelView.dbMax = v })
        }
    }
    // ---- 復調パネル ----
    Rectangle {
        anchors.top: chanBox.bottom; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: Theme.pad
        color: Theme.panel; border.color: Theme.line; border.width: 1
        Row {
            x: Theme.pad; y: Theme.pad; spacing: Theme.pad
            Rectangle {
                width: 200; height: 56; color: app.squelchOpen ? Theme.green : Theme.gray
                Text { anchors.centerIn: parent; text: app.squelchOpen ? "CARRIER" : "SQUELCHED"; color: "#000000"
                       font.family: Theme.mono; font.pixelSize: Theme.fsLarge; font.bold: true }
            }
            ValueField { label: "RX FREQ"; value: Theme.fmtFreq(page.rxFreq); width: 260; height: 56; onActivated: ui.askFreq(page.rxFreq, (v) => app.tuneRx(v)) }
            Readout { label: "MODE"; value: app.mode; width: 110; valueColor: Theme.textBright }
            Readout { label: "CH BW"; value: (app.channelBandwidth / 1e3).toFixed(1) + " kHz"; width: 140 }
            Readout { label: "LO / OFFSET"; value: Theme.fmtFreq(sys.centerFreq) + "  " + Theme.fmtHz(app.channelOffsetHz); width: 420 }
            Readout { label: "AUDIO"; value: app.audioError.length ? "ERROR" : (app.mute ? "MUTE" : "ON"); width: 110
                      valueColor: app.audioError.length ? Theme.red : Theme.text }
            Readout { label: "UNDER/OVER"; value: app.audioUnderruns.toFixed(0) + " / " + app.audioOverruns.toFixed(0); width: 160
                      valueColor: (app.audioUnderruns > 0 || app.audioOverruns > 0) ? Theme.amber : Theme.text }
            Readout { label: "LATENCY"; value: app.audioLatencyMs.toFixed(0) + " ms"; width: 130 }
            Readout { label: "DROPS"; value: app.lossless_drops.toFixed(0); width: 110; valueColor: app.lossless_drops > 0 ? Theme.red : Theme.green }
        }
        Row {
            x: Theme.pad; y: Theme.pad + 80; spacing: Theme.pad * 3
            LevelMeter { label: "CHANNEL POWER (dBFS)"; value: app.signalDb; minDb: -100; maxDb: 0; threshold: app.squelchDb; width: 800 }
            LevelMeter { label: "AUDIO (dBFS)"; value: app.audioDb; minDb: -60; maxDb: 0; barColor: Theme.cyan; width: 600 }
        }
        Text { x: Theme.pad; y: Theme.pad + 150; text: app.audioError; color: Theme.red; visible: app.audioError.length > 0
               font.family: Theme.mono; font.pixelSize: Theme.fsBase }
    }
}

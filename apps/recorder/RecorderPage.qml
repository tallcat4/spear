// IQ RECORDER のページ: 上に SpectrumView、下に録音パネル。録音の状態は app が所有する。
import QtQuick
import Spear.Theme
import Spear.Widgets

Item {
    id: page
    required property var app      // この App の QObject(シェルが注入)
    required property var ui       // シェルのサービス(askFreq / askRef / showDiagnostics)

    readonly property var softKeys: [
        { label: app.recording ? "STOP" : "RECORD", action: "rec", active: app.recording },
        { label: "FREQ", action: "freq", enabled: !app.recording },   // 録音中の retune は sidecar に残るが、意図的操作に限る
        { label: "-1 MHz", action: "f-", enabled: !app.recording }, { label: "+1 MHz", action: "f+", enabled: !app.recording },
        { label: "RATE", action: "rate", enabled: !app.recording }, { label: "REF LVL", action: "ref" }, { label: "DIAG", action: "diag" } ]
    function softKey(action) {
        switch (action) {
        case "rec": if (app.recording) app.stopRecording(); else app.record(); break
        case "freq": ui.askFreq(sys.centerFreq, (v) => app.tune(v)); break
        case "rate": if (!app.recording) ui.askRate(sys.sampleRate, (v) => ui.restartWithRate(v)); break
        case "f-": app.stepFreq(-1e6); break
        case "f+": app.stepFreq(1e6); break
        case "ref": ui.askRef(app.view.dbMax, (v) => { app.view.dbMin = v - 90; app.view.dbMax = v }); break
        case "diag": ui.showDiagnostics(); break
        }
    }

    SpectrumView {
        id: view
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        height: Math.round(parent.height * 0.62)
        source: app.view
        centerFreq: sys.centerFreq
        centerEditable: !app.recording
        onAskFreq: ui.askFreq(sys.centerFreq, (v) => app.tune(v))
        onAskRef: ui.askRef(app.view.dbMax, (v) => { app.view.dbMin = v - 90; app.view.dbMax = v })
    }

    // ---- 録音パネル ----
    Rectangle {
        anchors.top: view.bottom; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: Theme.pad
        color: Theme.panel; border.color: Theme.line; border.width: 1
        Rectangle {
            x: Theme.pad; y: Theme.pad; width: 260; height: 64
            color: app.recording ? Theme.red : Theme.gray
            Text { anchors.centerIn: parent; text: app.recording ? "RECORDING" : "STANDBY"; color: "#000000"
                   font.family: Theme.mono; font.pixelSize: Theme.fsHuge; font.bold: true }
        }
        Text { x: Theme.pad; y: Theme.pad + 150; width: parent.width - 2 * Theme.pad; elide: Text.ElideMiddle
               text: app.path.length ? app.path + ".sigmf-data" : "(no file — press RECORD)"; color: Theme.textDim
               font.family: Theme.mono; font.pixelSize: Theme.fsBase }
        Text { x: Theme.pad; y: Theme.pad + 180; text: app.lastError; color: Theme.red; visible: app.lastError.length > 0
               font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
        Row {
            x: 300; y: Theme.pad; spacing: Theme.pad
            Readout { label: "DURATION"; value: app.seconds.toFixed(1) + " s"; width: 200; valueColor: Theme.textBright }
            Readout { label: "SAMPLES"; value: app.samplesWritten.toLocaleString(Qt.locale("C"), "f", 0); width: 260 }
            Readout { label: "WRITTEN"; value: (app.bytesWritten / 1e6).toFixed(1) + " MB"; width: 200 }
            Readout { label: "RATE"; value: Theme.fmtRate(sys.sampleRate) + "  (" + (sys.sampleRate * 4 / 1e6).toFixed(1) + " MB/s)"; width: 330 }
            Readout { label: "DISK FREE"; value: sys.diskFreeGb.toFixed(1) + " GB"; width: 200; valueColor: sys.diskFreeGb < 2 ? Theme.red : Theme.text }
        }
        // Lossless 不変条件 (§10): drop は 0 でなければならない。0 以外は赤で即座に分かる
        Rectangle {
            x: 300; y: Theme.pad + 80; width: 700; height: 52
            color: app.droppedBlocks > 0 ? Theme.red : "transparent"; border.color: Theme.lineDim; border.width: 1
            Text { anchors.verticalCenter: parent.verticalCenter; x: Theme.pad
                   text: "LOSSLESS DROPS  " + app.droppedBlocks + (app.droppedBlocks > 0 ? "   — recording has DISCONTINUITY (see sidecar)" : "   — no silent loss")
                   color: app.droppedBlocks > 0 ? "#000000" : Theme.green; font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
        }
    }
}

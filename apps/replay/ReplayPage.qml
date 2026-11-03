// IQ REPLAY TX のページ: 上に送っている IQ の SpectrumView、下に送信パネル。
// 送信中かどうか・送信ゲイン・端子は Core(sys.transmitting / sys.txGain / sys.txPort)から読む。TX MASTER も Core(sys.txArmed)。
import QtQuick
import Spear.Theme
import Spear.Widgets

Item {
    id: page
    required property var app
    required property var ui

    readonly property bool canSend: app.fileValid && app.configCurrent && sys.txArmed
    readonly property var softKeys: [
        { label: app.sending ? "STOP" : "TRANSMIT", action: "tx", active: app.sending, enabled: app.sending || page.canSend },
        { label: "FILE", action: "file", enabled: !app.sending && !shell.busy },   // 録音を変えると装置を録音の center / rate で構成し直す(App は止めない)
        { label: "TX GAIN", action: "gain" },
        { label: app.loop ? "LOOP ON" : "LOOP OFF", action: "loop", active: app.loop },
        { label: "TX PORT", action: "port", enabled: !app.sending && !shell.busy },
        { label: "REF LVL", action: "ref" }, { label: "DIAG", action: "diag" } ]
    function softKey(action) {
        switch (action) {
        case "tx": if (app.sending) app.stopTransmit(); else app.transmit(); break
        case "file":
            app.refreshRecordings()
            ui.askList("RECORDING TO TRANSMIT", "Transmitted as recorded: same center frequency, sample rate and IQ.  The radio is reconfigured for the selection.",
                       app.recordings, app.file, (v) => { if (v !== app.file || !app.configCurrent) { app.file = v; ui.applyRf() } })
            break
        case "gain": ui.askLevel("TX GAIN  (0 dB = minimum output)", "dB", 0, app.txGainLimit(), sys.txGain, (v) => app.setTxGain(v)); break
        case "loop": app.loop = !app.loop; break
        case "port":
            ui.askList("TX PORT", "Only the TX/RX ports can transmit.",
                       [ { label: "TRXA", sublabel: "A:A  TX/RX", value: "TRXA" }, { label: "TRXB", sublabel: "A:B  TX/RX", value: "TRXB" } ],
                       app.txPort, (v) => { if (v !== app.txPort) { app.txPort = v; ui.applyRf() } })
            break
        case "ref": ui.askRef(app.view.dbMax, (v) => { app.view.dbMin = v - 90; app.view.dbMax = v }); break
        case "diag": ui.showDiagnostics(); break
        }
    }

    SpectrumView {
        id: view
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        height: Math.round(parent.height * 0.55)
        source: app.view
        centerFreq: sys.centerFreq
        centerEditable: false   // 送信周波数は録音の値。UI から変えない
        onAskRef: ui.askRef(app.view.dbMax, (v) => { app.view.dbMin = v - 90; app.view.dbMax = v })
    }

    // ---- 送信パネル ----
    Rectangle {
        anchors.top: view.bottom; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: Theme.pad
        color: Theme.panel; border.color: Theme.line; border.width: 1

        // 状態灯: TRANSMITTING(装置が burst の中 = Core)/ SAFE(TX MASTER)/ STANDBY
        Rectangle {
            id: lamp
            x: Theme.pad; y: Theme.pad; width: 340; height: 72
            color: sys.transmitting ? Theme.red : (sys.txArmed ? "transparent" : Theme.gray)
            border.color: sys.transmitting || sys.txArmed ? Theme.red : Theme.gray; border.width: 2
            Text { anchors.centerIn: parent
                   text: sys.transmitting ? "TRANSMITTING" : (sys.txArmed ? "STANDBY" : "TX SAFE")
                   color: sys.transmitting ? "#000000" : (sys.txArmed ? Theme.red : "#000000")
                   font.family: Theme.mono; font.pixelSize: Theme.fsHuge; font.bold: true }
        }
        Text { x: Theme.pad; y: lamp.y + lamp.height + 8; width: lamp.width; wrapMode: Text.Wrap
               text: sys.txArmed ? "TX MASTER ARMED" : "TX MASTER SAFE — arm in the main menu"
               color: sys.txArmed ? Theme.red : Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall; font.bold: sys.txArmed }

        Grid {
            x: lamp.x + lamp.width + Theme.pad * 2; y: Theme.pad
            columns: 4; columnSpacing: Theme.pad; rowSpacing: Theme.pad
            Readout { label: "TX FREQ"; value: app.fileValid ? Theme.fmtFreq(app.fileFreq) : "--"; width: 320; valueColor: Theme.textBright }
            Readout { label: "RATE"; value: app.fileValid ? Theme.fmtRate(app.fileRate) : "--"; width: 220 }
            Readout { label: "TX GAIN"; value: sys.txGain.toFixed(1) + " dB"; width: 220; valueColor: sys.txGain > 0 ? Theme.amber : Theme.textBright }
            Readout { label: "TX PORT"; value: sys.txPort; width: 180 }
            Readout { label: "POSITION"; value: app.positionSeconds.toFixed(1) + " / " + app.fileSeconds.toFixed(1) + " s"; width: 320 }
            Readout { label: "PASSES"; value: app.loop ? (app.passes + 1).toString() : "ONCE"; width: 220 }
            Readout { label: "UNDERFLOW"; value: sys.txUnderflows.toString(); width: 220; valueColor: sys.txUnderflows > 0 ? Theme.red : Theme.green }
            Readout { label: "LO CORR"; value: sys.loCorrectionPpm.toFixed(2) + " ppm"; width: 180 }
        }
        Column {
            x: Theme.pad; y: 200; width: parent.width - 2 * Theme.pad; spacing: 6
            Text { width: parent.width; elide: Text.ElideMiddle
                   text: app.file.length ? app.file + ".sigmf-data" : "(no recording — press FILE)"
                   color: app.fileValid ? Theme.text : Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
            Text { visible: !app.fileValid && app.file.length > 0; text: "CANNOT USE RECORDING: " + app.fileError
                   color: Theme.red; font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
            Text { visible: app.fileValid && !app.configCurrent && !sys.transmitting
                   text: shell.busy ? "APPLYING RF CONFIGURATION ..." : "RECORDING / PORT NOT APPLIED — select it again"; color: Theme.amber
                   font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
            // 録音に欠落・retune があっても連続した IQ として最初の周波数で送る(送信の途中で周波数は変えない)
            Text { visible: app.fileValid && (app.fileDiscontinuities > 0 || app.fileRetunes > 0)
                   text: "NOTE: recording has " + app.fileDiscontinuities + " discontinuities and " + app.fileRetunes
                         + " retunes; sent back-to-back at the first capture frequency"
                   color: Theme.amber; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
            Text { visible: app.lastError.length > 0; text: app.lastError; color: Theme.red
                   font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
        }
    }
}

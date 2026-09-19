// 装置上の可観測性 (要件 §12.1): 装置 / Stream Bus / host / イベント履歴を 1 画面で
import QtQuick
import Spear.Theme
import Spear.Widgets

Item {
    id: page
    readonly property int colW: Math.floor((width - Theme.pad * 4) / 3)

    component Header: Text { color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase; bottomPadding: 6 }
    component KV: Row {
        property string k: ""; property string v: ""; property color vc: Theme.text
        spacing: 0
        Text { width: 150; text: k; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
        Text { text: v; color: vc; font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
    }

    Row {
        x: Theme.pad; y: Theme.pad; spacing: Theme.pad
        // ---- 装置 ----
        Column {
            width: page.colW; spacing: 4
            Header { text: "DEVICE" }
            Rectangle {
                width: parent.width; height: 56; color: Theme.stateFill(sys.deviceStateCode)
                Text { anchors.centerIn: parent; text: sys.deviceState + "  " + sys.stateSeconds.toFixed(0) + " s"; color: "#000000"
                       font.family: Theme.mono; font.pixelSize: Theme.fsLarge; font.bold: true }
            }
            Text { width: parent.width; wrapMode: Text.Wrap; text: sys.deviceEvidence; color: Theme.text
                   font.family: Theme.mono; font.pixelSize: Theme.fsSmall; bottomPadding: 6 }
            KV { k: "SERIAL"; v: sys.serial }
            KV { k: "FX3"; v: sys.fx3State }
            KV { k: "STAGE"; v: sys.stage + " / 6" }
            KV { k: "GENERATION"; v: sys.generation.toString() }
            KV { k: "CENTER"; v: Theme.fmtFreq(sys.centerFreq) }
            KV { k: "RATE"; v: Theme.fmtRate(sys.sampleRate) }
            KV { k: "LO CORR"; v: sys.loCorrectionPpm !== 0 ? (sys.loCorrectionPpm > 0 ? "+" : "") + sys.loCorrectionPpm.toFixed(2) + " ppm" : "none" }
            KV { k: "GAIN / PORT"; v: sys.gain.toFixed(1) + " dB / " + sys.port + " (" + sys.portUhd + ")" }
            Item { width: 1; height: 8 }
            KV { k: "LO"; v: sys.sensorsValid ? (sys.loLocked ? "LOCKED" : "UNLOCKED") : "n/a"; vc: !sys.sensorsValid ? Theme.textDim : (sys.loLocked ? Theme.green : Theme.red) }
            KV { k: "AD9361 TEMP"; v: sys.sensorsValid ? sys.rxTemp.toFixed(1) + " °C" : "n/a" }
            KV { k: "RSSI"; v: sys.sensorsValid ? sys.rssi.toFixed(1) + " dB" : "n/a" }
            KV { k: "CLOCK DRIFT"; v: sys.sensorsValid ? sys.driftPpm.toFixed(2) + " ± " + sys.driftUncertaintyPpm.toFixed(2) + " ppm" : "n/a" }
            Item { width: 1; height: 8 }
            KV { k: "OVERFLOW"; v: sys.overflowCount.toString(); vc: sys.overflowCount ? Theme.red : Theme.text }
            KV { k: "OUT OF SEQ"; v: sys.oosCount.toString(); vc: sys.oosCount ? Theme.red : Theme.text }
            KV { k: "TIMEOUT"; v: sys.timeoutCount.toString(); vc: sys.timeoutCount ? Theme.amber : Theme.text }
            KV { k: "DISCONTINUITY"; v: sys.discontinuityCount.toString(); vc: sys.discontinuityCount ? Theme.amber : Theme.text }
        }
        // ---- Stream Bus ----
        Column {
            width: page.colW; spacing: 4
            Header { text: "STREAM BUS  radio.rx" }
            Row {
                spacing: 0
                Text { width: 150; text: "CONSUMER"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                Text { width: 100; text: "POLICY"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                Text { width: 110; text: "DELIVERED"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall; horizontalAlignment: Text.AlignRight }
                Text { width: 100; text: "DROPPED"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall; horizontalAlignment: Text.AlignRight }
                Text { width: 110; text: "QUEUE"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall; horizontalAlignment: Text.AlignRight }
            }
            Rectangle { width: parent.width; height: 1; color: Theme.line }
            Repeater {
                model: sys.consumers
                delegate: Row {
                    required property var modelData
                    readonly property bool lossless: modelData.policy === "LOSSLESS"
                    readonly property bool bad: lossless && modelData.dropped > 0
                    spacing: 0
                    Text { width: 150; text: modelData.name; color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
                    Text { width: 100; text: modelData.policy; color: lossless ? Theme.cyan : Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
                    Text { width: 110; text: modelData.delivered; color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase; horizontalAlignment: Text.AlignRight }
                    Text { width: 100; text: modelData.dropped; color: bad ? Theme.red : (lossless ? Theme.green : Theme.textDim)
                           font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: bad; horizontalAlignment: Text.AlignRight }
                    Text { width: 110; text: modelData.depth + "/" + modelData.capacity + " ^" + modelData.maxDepth; color: Theme.textDim
                           font.family: Theme.mono; font.pixelSize: Theme.fsBase; horizontalAlignment: Text.AlignRight }
                }
            }
            Text { visible: sys.consumers.length === 0; text: "(no consumers — no app running)"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
            Item { width: 1; height: 12 }
            Header { text: "INVARIANT" }
            Text { width: parent.width; wrapMode: Text.Wrap; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall
                   text: "LOSSLESS consumers deliver every sample or raise DISCONTINUITY. LATEST consumers drop old blocks by design; their drops are counted, never silent." }
            KV { k: "CONSUMER OVF"; v: sys.consumerOverflowCount.toString(); vc: sys.consumerOverflowCount ? Theme.red : Theme.text }
        }
        // ---- host ----
        Column {
            width: page.colW; spacing: 4
            Header { text: "HOST" }
            KV { k: "CPU"; v: sys.cpuMhz.toFixed(0) + " MHz  " + sys.governor }
            KV { k: "PKG TEMP"; v: sys.pkgTemp.toFixed(0) + " °C"; vc: sys.pkgTemp >= 90 ? Theme.red : Theme.text }
            KV { k: "THROTTLE"; v: sys.throttleCount.toString() }
            KV { k: "POWER"; v: sys.onAc ? "AC" : "BATTERY"; vc: sys.onAc ? Theme.text : Theme.amber }
            KV { k: "DISK FREE"; v: sys.diskFreeGb.toFixed(1) + " GB"; vc: sys.diskFreeGb < 2 ? Theme.red : Theme.text }
            KV { k: "RSS"; v: sys.rssMb.toFixed(0) + " MB" }
            KV { k: "MEM AVAIL"; v: sys.memAvailMb.toFixed(0) + " MB" }
            KV { k: "RTPRIO"; v: sys.rtprioLimit.toString(); vc: sys.rtprioLimit > 0 ? Theme.text : Theme.amber }
            KV { k: "LOAD"; v: sys.load1.toFixed(2) }
            KV { k: "SOURCE"; v: sys.sourceName }
        }
    }

    // ---- イベント履歴 ----
    Item {
        x: Theme.pad; y: parent.height * 0.52
        width: parent.width - Theme.pad * 2; height: parent.height - y - Theme.pad
        Header { id: evh; text: "EVENT LOG  (newest first)" }
        Rectangle { y: evh.height; width: parent.width; height: 1; color: Theme.line }
        ListView {
            y: evh.height + 4; width: parent.width; height: parent.height - y
            model: sys.events
            clip: true
            delegate: Row {
                required property string time; required property string kind; required property string source
                required property string detail; required property int severity; required property string range
                spacing: 0; height: 22; clip: true
                Text { width: 120; text: time; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                Text { width: 190; text: kind; color: Theme.severityColor(severity); font.family: Theme.mono; font.pixelSize: Theme.fsSmall; font.bold: severity >= 2 }
                Text { width: 160; text: source; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall; elide: Text.ElideRight }
                // 複数行の detail(UHD のエラー文など)は 1 行に畳む。elide は改行を跨げないため
                Text { width: 1300; text: (detail + (range.length ? "   [" + range + "]" : "")).replace(/\s*\n\s*/g, " | ")
                       color: severity >= 3 ? Theme.red : Theme.text
                       font.family: Theme.mono; font.pixelSize: Theme.fsSmall; elide: Text.ElideRight; maximumLineCount: 1 }
            }
        }
    }
}

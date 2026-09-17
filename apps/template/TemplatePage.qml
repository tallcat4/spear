// テンプレートのページ。見えるもの: app(この App)、sys(SystemModel)、ui(シェルのサービス)。
import QtQuick
import Spear.Theme
import Spear.Widgets

Item {
    id: page
    required property var app
    required property var ui

    // 7 個までのソフトキー(8 個目 BACK はシェル)。null は空き。
    readonly property var softKeys: [
        { label: "THRESH", action: "thresh" }, null, null, null, null, null, { label: "DIAG", action: "diag" } ]
    function softKey(action) {
        switch (action) {
        case "thresh": ui.askRef(app.threshold, (v) => app.threshold = v); break
        case "diag": ui.showDiagnostics(); break
        }
    }

    Column {
        x: Theme.pad; y: Theme.pad; spacing: Theme.pad
        Text { text: app.appName; color: Theme.textBright; font.family: Theme.mono; font.pixelSize: Theme.fsHuge; font.bold: true }
        Row {
            spacing: Theme.pad
            Readout { label: "THRESHOLD"; value: app.threshold.toFixed(0) + " dBFS"; width: 240 }
            Readout { label: "BLOCKS"; value: app.blocksSeen.toFixed(0); width: 200 }
            Readout { label: "CENTER"; value: Theme.fmtFreq(sys.centerFreq); width: 260 }
        }
    }
}

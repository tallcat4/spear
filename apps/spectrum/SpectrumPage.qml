// SPECTRUM のページ。見えるもの: app(SpectrumApp)、sys(SystemModel)、ui(シェルのサービス: askFreq 等)
import QtQuick
import Spear.Theme
import Spear.Widgets

Item {
    id: page
    required property var app      // この App の QObject(シェルが注入)
    required property var ui       // シェルのサービス(askFreq / askRef / showDiagnostics)
    property bool maxHold: true

    // ソフトキー(7 個まで。8 個目の BACK はシェルが付ける)
    readonly property var softKeys: [
        { label: "FREQ", action: "freq" }, { label: "-1 MHz", action: "f-" }, { label: "+1 MHz", action: "f+" },
        { label: "RATE", action: "rate" }, { label: "REF LVL", action: "ref" }, { label: "AVG " + app.view.averaging, action: "avg" },
        { label: "HOLD", action: "hold", active: page.maxHold } ]
    function softKey(action) {
        switch (action) {
        case "freq": ui.askFreq(sys.centerFreq, (v) => app.tune(v)); break
        case "rate": ui.askRate(sys.sampleRate, (v) => ui.restartWithRate(v)); break
        case "f-": app.stepFreq(-1e6); break
        case "f+": app.stepFreq(1e6); break
        case "ref": ui.askRef(app.view.dbMax, (v) => { app.view.dbMin = v - 90; app.view.dbMax = v }); break
        case "avg": app.view.averaging = app.view.averaging >= 16 ? 1 : app.view.averaging * 2; break
        case "hold": page.maxHold = !page.maxHold; app.view.resetMaxHold(); break
        case "diag": ui.showDiagnostics(); break
        }
    }

    SpectrumView {
        anchors.fill: parent
        source: app.view
        centerFreq: sys.centerFreq
        maxHold: page.maxHold
        onAskFreq: ui.askFreq(sys.centerFreq, (v) => app.tune(v))
        onAskRef: ui.askRef(app.view.dbMax, (v) => { app.view.dbMin = v - 90; app.view.dbMax = v })
    }
}

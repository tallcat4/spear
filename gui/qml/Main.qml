import QtQuick
import QtQuick.Window
import Spear.Theme
import Spear.Input
import Spear

// シェル: StatusBar / ページ(メニュー | App ページ | Diagnostics)/ SoftKeyBar。
// 操作はタッチのみ(FZ-G2 タブレットモード、キーボード・マウスなし)。
Window {
    id: win
    width: startWidth; height: startHeight
    visible: true
    visibility: startFullscreen ? Window.FullScreen : Window.Windowed
    title: "S.P.E.A.R."
    color: Theme.bg

    readonly property bool appOpen: shell.activeApp !== null
    readonly property bool diagOpen: shell.diagnosticsOpen

    function back() {
        if (shell.diagnosticsOpen) { shell.showDiagnostics(false); return }
        if (shell.activeApp !== null) shell.stopApp()
    }

    // ---- App ページから呼べるシェルのサービス ----
    QtObject {
        id: uiServices
        function askFreq(current, cb) { win.askFreq(current, cb) }
        function askRate(current, cb) { win.askRate(current, cb) }
        function askGain(current, cb) { win.askGain(current, cb) }
        function askRef(current, cb) { win.askRef(current, cb) }
        function askOffset(current, cb) { win.askOffset(current, cb) }
        function showDiagnostics() { shell.showDiagnostics(true) }
        function restartWithRate(v) { shell.restartActiveWithRate(v) }   // 汎用 App のレート変更(App を再起動する)
    }

    // 固定パネル 1920x1200 前提の固定キャンバス (§1)。窓がそれより小さい/大きい場合は等倍縮尺で全体を収める。
    Item {
        id: canvas
        width: 1920; height: 1200
        readonly property real fit: Math.min(win.width / 1920, win.height / 1200)
        scale: fit
        transformOrigin: Item.TopLeft
        x: Math.round((win.width - 1920 * fit) / 2); y: Math.round((win.height - 1200 * fit) / 2)

        StatusBar { id: status; width: parent.width; anchors.top: parent.top }

        Item {
            id: content
            anchors.top: status.bottom; anchors.bottom: softkeys.top; width: parent.width

            MenuPage {
                anchors.fill: parent; visible: !win.appOpen && !win.diagOpen
                onSelected: (i) => { if (!shell.busy) shell.startApp(i) }
                onAskGain: win.askGain(shell.draftGain, (v) => { if (v < 0) shell.draftAgc = true; else shell.draftGain = v })
            }
            // App のページ: レジストリの page_url を Loader で読む。ページには app / sys / ui が見える。
            Loader {
                id: appPage
                anchors.fill: parent
                visible: win.appOpen && !win.diagOpen
                // ページの required property (app / ui) は setSource で注入する
                Connections {
                    target: shell
                    function onActiveChanged() {
                        if (shell.activeApp !== null) {
                            if (appPage.source != shell.activePage) appPage.setSource(shell.activePage, { "app": shell.activeApp, "ui": uiServices })
                        } else {
                            appPage.setSource("")
                        }
                    }
                }
            }
            DiagnosticsPage { anchors.fill: parent; visible: win.diagOpen }
        }

        SoftKeyBar {
            id: softkeys
            width: parent.width; anchors.bottom: parent.bottom
            keys: {
                var k = []
                if (win.diagOpen) {
                    for (var i = 0; i < 7; ++i) k.push(null)
                } else if (win.appOpen && appPage.item && appPage.item.softKeys) {
                    k = appPage.item.softKeys.slice(0, 7)
                    while (k.length < 7) k.push(null)
                } else {
                    // CENTER / RATE は各 App が決める(メニューには置かない)。GAIN だけは装置の共通設定
                    k = [ { label: "GAIN", action: "mgain" }, null, null, null, null, null, { label: "DIAG", action: "diag" } ]
                }
                k.push((win.appOpen || win.diagOpen) ? { label: "BACK", action: "back" } : null)
                return k
            }
            onPressed: (i) => win.softkey(i)
        }

        // ---- 自前の数値入力(モーダル)。入力中もステータスバーは見せる ----
        NumericEntry {
            id: entry
            anchors.topMargin: Theme.statusBarH
            property var callback: null
            onAccepted: (v) => { if (callback) callback(v) }
        }
    }

    function softkey(i) {
        var k = softkeys.keys[i]
        if (!k) return
        switch (k.action) {
        case "back": back(); return
        case "mgain": askGain(shell.draftGain, (v) => { if (v < 0) shell.draftAgc = true; else shell.draftGain = v }); return
        case "diag": shell.showDiagnostics(true); return
        }
        if (win.appOpen && appPage.item && appPage.item.softKey) appPage.item.softKey(k.action)
    }

    // ---- 数値入力の定型(App からは ui.ask*() で呼ぶ) ----
    function askFreq(current, cb) {
        entry.title = "CENTER FREQUENCY"; entry.minimum = 70e6; entry.maximum = 6e9
        entry.units = [ { label: "GHz", factor: 1e9 }, { label: "MHz", factor: 1e6 }, { label: "kHz", factor: 1e3 }, { label: "Hz", factor: 1 } ]
        entry.displayFactor = 1e6; entry.displayUnit = "MHz"; entry.displayDecimals = 6; entry.allowNegative = false
        entry.callback = cb; entry.open(current)
    }
    function askRate(current, cb) {
        entry.title = "SAMPLE RATE"; entry.minimum = 200e3; entry.maximum = 10e6
        entry.units = [ { label: "Msps", factor: 1e6 }, { label: "ksps", factor: 1e3 } ]
        entry.displayFactor = 1e6; entry.displayUnit = "Msps"; entry.displayDecimals = 3; entry.allowNegative = false
        entry.callback = cb; entry.open(current)
    }
    // 戻り値 < 0 は AGC(AGC キーは -1 を返す)。数値なら手動 gain
    function askGain(current, cb) {
        entry.title = "RX GAIN  (or AGC)"; entry.minimum = 0; entry.maximum = 76
        entry.units = [ { label: "dB", factor: 1 }, { label: "AGC", value: -1 } ]
        entry.displayFactor = 1; entry.displayUnit = "dB"; entry.displayDecimals = 1; entry.allowNegative = false
        entry.callback = cb; entry.open(current)
    }
    function askOffset(current, cb) {
        entry.title = "LO OFFSET (channel position relative to LO)"; entry.minimum = -900e3; entry.maximum = 900e3
        entry.units = [ { label: "kHz", factor: 1e3 }, { label: "Hz", factor: 1 } ]
        entry.displayFactor = 1e3; entry.displayUnit = "kHz"; entry.displayDecimals = 1; entry.allowNegative = true
        entry.callback = cb; entry.open(current)
    }
    function askRef(current, cb) {
        entry.title = "REFERENCE LEVEL (TOP OF SCALE)"; entry.minimum = -150; entry.maximum = 20
        entry.units = [ { label: "dBFS", factor: 1 } ]
        entry.displayFactor = 1; entry.displayUnit = "dBFS"; entry.displayDecimals = 0; entry.allowNegative = true
        entry.callback = cb; entry.open(current)
    }

    // ---- 検証用(--screenshot 系 CLI から呼ぶ) ----
    function openFreqEntry() { askFreq(sys.centerFreq, (v) => { if (shell.activeApp && shell.activeApp.tune) shell.activeApp.tune(v) }) }
    function demoEntryError() { openFreqEntry(); entry.type("9"); entry.type("."); entry.type("5"); entry.commit(1e9, "GHz") }
    function demoRapidTune() { var a = shell.activeApp; if (a && a.stepFreq) { a.stepFreq(1e6); a.stepFreq(1e6); a.stepFreq(1e6) } }
    function demoEntryTune(mhzText) { openFreqEntry(); for (var i = 0; i < mhzText.length; ++i) entry.type(mhzText[i]); entry.commit(1e6, "MHz") }
    function demoAppAction(action) { if (win.appOpen && appPage.item && appPage.item.softKey) appPage.item.softKey(action) }
}

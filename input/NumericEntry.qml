// Spear.Input — NumericEntry: 自前の数値入力パネル(モーダル)。
//
// 計測器の文法: 数字を打ってから単位キーで確定する(例: 8 2 . 5 [MHz])。ENTER は表示単位で確定。
// 範囲外・空・不正はパネルを閉じずに赤で理由を出す。タッチ専用(FZ-G2 タブレットモード)。
// OS/デスクトップの仮想キーボードには依存しない。
//
// 使い方:
//   NumericEntry { id: freqEntry; title: "CENTER FREQUENCY"; minimum: 70e6; maximum: 6e9
//                  units: [{label:"GHz",factor:1e9},{label:"MHz",factor:1e6},{label:"kHz",factor:1e3}]
//                  displayFactor: 1e6; displayUnit: "MHz"; displayDecimals: 6
//                  onAccepted: (v) => shell.tune(v) }
//   freqEntry.open(currentValue)
import QtQuick
import Spear.Theme

Item {
    id: entry
    anchors.fill: parent
    visible: false
    z: 1000

    property string title: "VALUE"
    property real minimum: -Infinity
    property real maximum: Infinity
    property var units: []                 // [{label, factor}] 空なら ENTER のみ(factor 1)。{label, value} なら入力なしでその値を確定(AGC 等)
    property real displayFactor: 1
    property string displayUnit: ""
    property int displayDecimals: 3
    property bool allowNegative: minimum < 0
    property bool allowDecimal: true
    property real current: 0
    property string text: ""
    property string error: ""
    readonly property bool opened: visible

    signal accepted(real value)
    signal cancelled()

    function open(value) {
        current = value
        text = ""
        error = ""
        visible = true
    }
    function close() { visible = false; error = "" }

    function fmt(v) {
        var s = (v / displayFactor).toFixed(displayDecimals)
        return s + (displayUnit.length ? " " + displayUnit : "")
    }
    function commit(factor, unitLabel) {
        if (text.length === 0 || text === "-" || text === "." || text === "-.") { error = "NO VALUE ENTERED"; return }
        var v = parseFloat(text) * factor
        if (!isFinite(v)) { error = "INVALID NUMBER"; return }
        if (v < minimum || v > maximum) {
            error = "OUT OF RANGE  " + fmt(minimum) + " .. " + fmt(maximum)
            return
        }
        error = ""
        var out = v
        visible = false
        accepted(out)
    }
    function type(ch) {
        error = ""
        if (ch === ".") { if (!allowDecimal || text.indexOf(".") >= 0) return; text += (text.length === 0 || text === "-") ? "0." : "." ; return }
        if (ch === "-") { if (!allowNegative) return; text = text.startsWith("-") ? text.substring(1) : "-" + text; return }
        if (text.length >= 16) return
        text += ch
    }
    function backspace() { error = ""; text = text.substring(0, text.length - 1) }
    function clear() { error = ""; text = "" }

    // ---- 背面: 黒で完全に覆う(透過にしない) ----
    Rectangle { anchors.fill: parent; color: Theme.bg; MouseArea { anchors.fill: parent } }

    Rectangle {
        id: panel
        width: 1260; height: 750
        anchors.centerIn: parent
        color: Theme.panel; border.color: Theme.line; border.width: 1

        // ---- 表示部 ----
        Text { x: 24; y: 18; text: entry.title; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
        Text { x: 24; y: 48; text: "CURRENT  " + entry.fmt(entry.current); color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
        Rectangle {
            x: 24; y: 84; width: panel.width - 48; height: 96
            color: Theme.bg; border.color: entry.error.length ? Theme.red : Theme.text; border.width: 2
            Text {
                anchors.right: cursor.left; anchors.rightMargin: 4; anchors.verticalCenter: parent.verticalCenter
                text: entry.text.length ? entry.text : "0"
                color: entry.text.length ? Theme.textBright : Theme.textDim
                font.family: Theme.mono; font.pixelSize: 56; font.bold: true
            }
            Rectangle { id: cursor; anchors.right: parent.right; anchors.rightMargin: 24; anchors.verticalCenter: parent.verticalCenter
                        width: 4; height: 56; color: Theme.text; visible: blink.on
                        Timer { id: blink; property bool on: true; interval: 500; repeat: true; running: entry.visible; onTriggered: on = !on } }
        }
        Text { x: 24; y: 190; text: "RANGE  " + entry.fmt(entry.minimum) + "  ..  " + entry.fmt(entry.maximum)
               color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase; visible: isFinite(entry.minimum) && isFinite(entry.maximum) }
        Rectangle {
            x: 24; y: 220; width: panel.width - 48; height: 34; color: entry.error.length ? Theme.red : "transparent"
            Text { anchors.verticalCenter: parent.verticalCenter; x: 10; text: entry.error; color: "#000000"
                   font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
        }

        // ---- テンキー ----
        Keypad {
            id: digits
            x: 24; y: 276
            columns: 3; keyWidth: 150; keyHeight: 100; gap: 8; labelSize: 34
            keys: [
                { label: "7", action: "7" }, { label: "8", action: "8" }, { label: "9", action: "9" },
                { label: "4", action: "4" }, { label: "5", action: "5" }, { label: "6", action: "6" },
                { label: "1", action: "1" }, { label: "2", action: "2" }, { label: "3", action: "3" },
                { label: "±", action: "-", enabled: entry.allowNegative }, { label: "0", action: "0" }, { label: ".", action: ".", enabled: entry.allowDecimal }
            ]
            onPressed: (a) => { if (a === "-" || a === ".") entry.type(a); else entry.type(a) }
        }
        Keypad {
            id: edit
            x: 24 + digits.width + 16; y: 276
            columns: 1; keyWidth: 170; keyHeight: 100; gap: 8; labelSize: Theme.fsLarge
            keys: [ { label: "⌫ BKSP", action: "bksp" }, { label: "CLEAR", action: "clr" } ]
            onPressed: (a) => { if (a === "bksp") entry.backspace(); else entry.clear() }
        }
        // ---- 単位 / 確定 ----
        Keypad {
            id: unitsPad
            x: 24 + digits.width + 16 + edit.width + 40; y: 276
            columns: 1; keyWidth: 260; keyHeight: 100; gap: 8; labelSize: Theme.fsLarge
            keys: {
                var k = []
                for (var i = 0; i < entry.units.length && i < 4; ++i)
                    k.push({ label: entry.units[i].label, action: "u" + i })
                return k
            }
            onPressed: (a) => { var i = parseInt(a.substring(1)); var u = entry.units[i]
                                if (u.value !== undefined) { entry.error = ""; entry.visible = false; entry.accepted(u.value) }
                                else entry.commit(u.factor, u.label) }
        }
        Keypad {
            x: unitsPad.x + unitsPad.width + 16; y: 276
            columns: 1; keyWidth: 220; keyHeight: 100; gap: 8; labelSize: Theme.fsLarge
            keys: [ { label: "ENTER", action: "enter", sublabel: entry.displayUnit, accent: Theme.green },   // 確定 = 緑(他のキーと一目で区別)
                    null, null,
                    { label: "CANCEL", action: "cancel" } ]
            onPressed: (a) => { if (a === "enter") entry.commit(entry.displayFactor, entry.displayUnit); else { entry.close(); entry.cancelled() } }
        }
    }

}

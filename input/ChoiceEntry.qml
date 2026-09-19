// Spear.Input — ChoiceEntry: 少数の選択肢から 1 つ選ぶ自前のモーダル(受信端子、モード等)。
//
// 計測器の文法: 選択肢を大きなキーで並べ、現在値は反転で示す。キーを押した瞬間に確定して閉じる(ENTER は要らない)。
// 拒否が起きない部品なので拒否音は鳴らない(タップ音は KeyButton.fire)。タッチ専用(FZ-G2 タブレットモード)。
// OS/デスクトップの仮想キーボードには依存しない。NumericEntry と同じ骨格(黒で覆う、ステータスバーは見せる)。
//
// 使い方:
//   ChoiceEntry { id: portEntry; title: "RX PORT"
//                 choices: [{label:"TRXA", sublabel:"A: TX/RX", value:"TRXA"}, ...]
//                 onAccepted: (v) => shell.draftPort = v }
//   portEntry.open(currentValue)
import QtQuick
import Spear.Theme

Item {
    id: entry
    anchors.fill: parent
    visible: false
    z: 1000

    property string title: "SELECT"
    property string note: ""               // 題名の下の補足(1 行。空なら出さない)
    property var choices: []               // [{label, sublabel?, value, enabled?}] 最大 8
    property var current: undefined
    property int columns: 1
    readonly property bool opened: visible

    signal accepted(var value)
    signal cancelled()

    function open(value) { current = value; visible = true }
    function close() { visible = false }

    // ---- 背面: 黒で完全に覆う(透過にしない) ----
    Rectangle { anchors.fill: parent; color: Theme.bg; MouseArea { anchors.fill: parent } }

    Rectangle {
        id: panel
        width: 1260; height: pad.y + pad.implicitHeight + 36   // 選択肢の行数に合わせる(数値入力と同じ幅)
        anchors.centerIn: parent
        color: Theme.panel; border.color: Theme.line; border.width: 1

        Text { x: 24; y: 18; text: entry.title; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
        Text { x: 24; y: 48; text: "CURRENT  " + (entry.current !== undefined ? String(entry.current) : "----")
               color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
        Text { x: 24; y: 78; width: panel.width - 48; wrapMode: Text.Wrap; text: entry.note; visible: entry.note.length > 0
               color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }

        // ---- 選択肢: 現在値は反転(active)。押した瞬間に確定 ----
        Keypad {
            id: pad
            x: 24; y: 140
            columns: entry.columns; keyWidth: 420; keyHeight: 110; gap: 12; labelSize: Theme.fsHuge
            keys: {
                var k = []
                for (var i = 0; i < entry.choices.length && i < 8; ++i) {
                    var c = entry.choices[i]
                    k.push({ label: c.label, sublabel: c.sublabel ? c.sublabel : "", action: "c" + i,
                             active: c.value === entry.current, enabled: c.enabled !== false })
                }
                return k
            }
            onPressed: (a) => { var c = entry.choices[parseInt(a.substring(1))]; entry.visible = false; entry.accepted(c.value) }
        }
        Keypad {
            x: panel.width - 24 - 220; y: 140
            columns: 1; keyWidth: 220; keyHeight: 100; gap: 8; labelSize: Theme.fsLarge
            keys: [ { label: "CANCEL", action: "cancel" } ]
            onPressed: { entry.close(); entry.cancelled() }
        }
    }
}

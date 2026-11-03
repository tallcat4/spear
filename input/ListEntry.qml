// Spear.Input — ListEntry: 多数の項目(録音ファイル等)から 1 つ選ぶ自前のモーダル。ChoiceEntry(最大 8)の多数版。
//
// 計測器の文法: 1 行 1 項目の大きな行をページ単位で並べ、PREV / NEXT でページを送る(スクロール・フリックは使わない)。
// 現在値の行は反転。行を押した瞬間に確定して閉じる。タッチ専用。ChoiceEntry と同じ骨格(黒で覆う、ステータスバーは見せる)。
//
// 使い方:
//   ListEntry { id: fileEntry; title: "RECORDING"
//               items: [{label:"rec_20261103", sublabel:"146.734 MHz  4 Msps  118 s", value:"/path/rec_20261103"}, ...]
//               onAccepted: (v) => app.setFile(v) }
//   fileEntry.open(currentValue)
import QtQuick
import Spear.Theme

Item {
    id: entry
    anchors.fill: parent
    visible: false
    z: 1000

    property string title: "SELECT"
    property string note: ""
    property var items: []                 // [{label, sublabel?, value}]
    property var current: undefined
    property int page: 0
    readonly property int perPage: 7
    readonly property int pages: Math.max(1, Math.ceil(items.length / perPage))
    readonly property bool opened: visible

    signal accepted(var value)
    signal cancelled()

    // 現在値のあるページから開く
    function open(value) {
        current = value
        var idx = 0
        for (var i = 0; i < items.length; ++i) if (items[i].value === value) { idx = i; break }
        page = Math.floor(idx / perPage)
        visible = true
    }
    function close() { visible = false }

    Rectangle { anchors.fill: parent; color: Theme.bg; MouseArea { anchors.fill: parent } }

    Rectangle {
        id: panel
        width: 1600; height: parent.height - 2 * Theme.pad
        anchors.centerIn: parent
        color: Theme.panel; border.color: Theme.line; border.width: 1

        Text { x: 24; y: 18; text: entry.title + "   " + (entry.page + 1) + " / " + entry.pages + "   (" + entry.items.length + ")"
               color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
        Text { x: 24; y: 48; width: panel.width - 300; wrapMode: Text.Wrap; text: entry.note; visible: entry.note.length > 0
               color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
        Text { x: 24; y: 120; visible: entry.items.length === 0; text: "(none)"
               color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsLarge }

        // ---- 行: 現在値は反転。押した瞬間に確定 ----
        Column {
            x: 24; y: 96; spacing: 8
            Repeater {
                model: Math.max(0, Math.min(entry.perPage, entry.items.length - entry.page * entry.perPage))
                delegate: KeyButton {
                    required property int index
                    readonly property var item: entry.items[entry.page * entry.perPage + index]
                    width: panel.width - 48 - 260; height: 112
                    label: item ? item.label : ""
                    sublabel: item && item.sublabel ? item.sublabel : ""
                    labelSize: Theme.fsLarge
                    active: item !== undefined && item.value === entry.current
                    onPressed: { var v = item.value; entry.visible = false; entry.accepted(v) }
                }
            }
        }
        Keypad {
            x: panel.width - 24 - 220; y: 96
            columns: 1; keyWidth: 220; keyHeight: 112; gap: 8; labelSize: Theme.fsLarge
            keys: [ { label: "PREV", action: "prev", enabled: entry.page > 0 },
                    { label: "NEXT", action: "next", enabled: entry.page + 1 < entry.pages },
                    { label: "CANCEL", action: "cancel" } ]
            onPressed: (a) => {
                if (a === "prev") entry.page = Math.max(0, entry.page - 1)
                else if (a === "next") entry.page = Math.min(entry.pages - 1, entry.page + 1)
                else { entry.close(); entry.cancelled() }
            }
        }
    }
}

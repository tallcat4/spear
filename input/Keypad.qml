// Spear.Input — Keypad: キー配列を model で与える汎用グリッド。
// keys: [{label, action, span?, sublabel?, enabled?, active?, repeat?, accent?}] を columns 列で並べる。null = 空き。accent = 強調色(確定キー等)。
// 数字テンキー、単位キー、将来の英数キーボード、機能キー列など、あらゆる自前入力の土台。
import QtQuick
import Spear.Theme

Item {
    id: pad
    property var keys: []
    property int columns: 3
    property int keyWidth: 120
    property int keyHeight: 84
    property int gap: 6
    property int labelSize: Theme.fsLarge
    signal pressed(string action, var key)

    implicitWidth: columns * keyWidth + (columns - 1) * gap
    implicitHeight: Math.ceil(keys.length / columns) * (keyHeight + gap) - gap

    Repeater {
        id: rep
        model: pad.keys
        delegate: KeyButton {
            required property int index
            required property var modelData
            property var keyData: modelData
            visible: modelData !== null
            x: (index % pad.columns) * (pad.keyWidth + pad.gap)
            y: Math.floor(index / pad.columns) * (pad.keyHeight + pad.gap)
            width: modelData && modelData.span ? modelData.span * pad.keyWidth + (modelData.span - 1) * pad.gap : pad.keyWidth
            height: pad.keyHeight
            label: modelData ? modelData.label : ""
            sublabel: modelData && modelData.sublabel ? modelData.sublabel : ""
            enabled: modelData ? (modelData.enabled !== false) : false
            active: modelData ? (modelData.active === true) : false
            repeat: modelData ? (modelData.repeat === true) : false
            accent: modelData && modelData.accent ? modelData.accent : "transparent"
            labelSize: pad.labelSize
            onPressed: pad.pressed(modelData.action, modelData)
        }
    }
}

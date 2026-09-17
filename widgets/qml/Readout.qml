// 計器の数値表示: ラベル(小・淡)+ 値(等幅・明)。枠は 1 px、装飾なし。
import QtQuick
import Spear.Theme

Item {
    id: root
    property string label: ""
    property string value: ""
    property color valueColor: Theme.text
    property int valueSize: Theme.fsLarge
    property bool framed: true
    implicitWidth: Math.max(lbl.implicitWidth, val.implicitWidth) + 2 * Theme.pad
    implicitHeight: lbl.implicitHeight + val.implicitHeight + 10
    Rectangle { anchors.fill: parent; color: "transparent"; border.color: Theme.lineDim; border.width: root.framed ? 1 : 0 }
    Text { id: lbl; x: Theme.pad; y: 3; text: root.label; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
    Text { id: val; x: Theme.pad; anchors.top: lbl.bottom; text: root.value; color: root.valueColor
           font.family: Theme.mono; font.pixelSize: root.valueSize; font.bold: true }
}

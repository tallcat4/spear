// Spear.Input — ValueField: タップ(または選択 + Enter)で入力パネルを開く読み出し欄。
import QtQuick
import Spear.Theme

Rectangle {
    id: field
    property string label: ""
    property string value: ""
    property bool selected: false
    property bool editable: true
    property color valueColor: Theme.textBright
    signal activated()

    implicitWidth: 300; implicitHeight: 62
    color: selected ? "#141414" : "transparent"
    border.color: selected ? Theme.text : (editable ? Theme.line : Theme.lineDim); border.width: selected ? 2 : 1

    Text { x: Theme.pad; y: 4; text: field.label; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
    Text { x: Theme.pad; y: 24; text: field.value; color: field.valueColor; font.family: Theme.mono; font.pixelSize: Theme.fsLarge; font.bold: true }
    // 編集可能の目印: 右端の小さな記号(装飾ではなく操作可能性の表示)
    Text { anchors.right: parent.right; anchors.rightMargin: 10; anchors.bottom: parent.bottom; anchors.bottomMargin: 6
           text: "≡"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall; visible: field.editable }
    MouseArea { anchors.fill: parent; enabled: field.editable; onClicked: { Feedback.tap(); field.activated() } }
}

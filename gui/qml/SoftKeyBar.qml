// 画面下のソフトキー(オシロ / MFD の文法)。タッチ専用。keys: [{label, action, enabled, active}]
import QtQuick
import Spear.Theme

Rectangle {
    id: bar
    property var keys: []
    signal pressed(int index)
    height: Theme.softKeyH
    color: Theme.panel
    Rectangle { width: parent.width; height: 1; color: Theme.line }
    Row {
        anchors.fill: parent; anchors.topMargin: 1
        Repeater {
            model: 8
            delegate: Item {
                required property int index
                width: bar.width / 8; height: bar.height - 1
                property var k: index < bar.keys.length ? bar.keys[index] : null
                property bool on: k !== null && k.enabled !== false
                Rectangle {
                    anchors.fill: parent; anchors.margins: 4
                    color: (k && k.active) ? Theme.invertBg : (ma.pressed && on ? "#303030" : Theme.bg)
                    border.color: on ? Theme.line : Theme.lineDim; border.width: 1
                    Text { anchors.centerIn: parent
                           text: k ? k.label : ""; color: (k && k.active) ? Theme.invertText : (on ? Theme.text : Theme.lineDim)
                           font.family: Theme.mono; font.pixelSize: Theme.fsLarge; font.bold: true }
                    MouseArea { id: ma; anchors.fill: parent; enabled: on; onClicked: bar.pressed(index) }
                }
            }
        }
    }
}

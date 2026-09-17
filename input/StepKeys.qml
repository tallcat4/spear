// Spear.Input — StepKeys: −/+ の 2 キー(長押しリピート)。ラベル付き。
import QtQuick
import Spear.Theme

Row {
    id: sk
    property string label: ""
    property real step: 1
    property int keyWidth: 100
    property int keyHeight: 64
    signal stepped(real delta)
    spacing: 6
    KeyButton { width: sk.keyWidth; height: sk.keyHeight; label: "−"; repeat: true; onPressed: sk.stepped(-sk.step) }
    Text { anchors.verticalCenter: parent.verticalCenter; text: sk.label; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall
           width: 90; horizontalAlignment: Text.AlignHCenter }
    KeyButton { width: sk.keyWidth; height: sk.keyHeight; label: "+"; repeat: true; onPressed: sk.stepped(sk.step) }
}

// 音量の表示と操作(OS の出力の音量。SystemVolume = volume)。ステータスバーの VOL のタップ、または音量キーで出る。
// 画面を覆わない(受信の表示は見えたまま)。操作が無ければ消える。表示の外をタップしても消える。
import QtQuick
import Spear.Theme
import Spear.Input

Item {
    id: pop
    anchors.fill: parent
    visible: false
    z: 900

    readonly property int segments: 20
    function show() { visible = true; hide.restart() }
    function close() { visible = false; hide.stop() }

    Timer { id: hide; interval: 3000; onTriggered: pop.visible = false }
    Connections {
        target: volume
        function onKeyUsed() { pop.show() }
        function onChanged() { if (pop.visible) hide.restart() }
    }

    MouseArea { anchors.fill: parent; onPressed: pop.close() }   // 外のタップで閉じる(下へは通さない)

    Rectangle {
        id: panel
        anchors.right: parent.right; anchors.top: parent.top
        anchors.rightMargin: Theme.pad; anchors.topMargin: Theme.statusBarH + Theme.pad
        width: 620; height: col.implicitHeight + 2 * Theme.pad
        color: Theme.panel; border.color: Theme.line; border.width: 1
        MouseArea { anchors.fill: parent; onPressed: hide.restart() }

        Column {
            id: col
            x: Theme.pad; y: Theme.pad; width: parent.width - 2 * Theme.pad
            spacing: Theme.pad
            Item {
                width: parent.width; height: 44
                Text { anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                       text: "VOLUME"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
                Text { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                       text: !volume.available ? "--" : (volume.muted ? "MUTE" : Math.round(volume.volume * 100) + " %")
                       color: volume.muted ? Theme.amber : Theme.textBright; font.family: Theme.mono; font.pixelSize: Theme.fsHuge; font.bold: true }
            }
            Row {
                spacing: 4
                Repeater {
                    model: pop.segments
                    Rectangle {
                        required property int index
                        width: (col.width - 4 * (pop.segments - 1)) / pop.segments; height: 24
                        color: volume.available && index < Math.round(volume.volume * pop.segments)
                               ? (volume.muted ? Theme.gray : Theme.green) : Theme.lineDim
                    }
                }
            }
            Text { visible: volume.error.length > 0; width: parent.width; elide: Text.ElideRight
                   text: volume.error; color: Theme.red; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            Row {
                spacing: Theme.pad
                KeyButton { width: 190; label: "−"; repeat: true; onPressed: { volume.step(-1); hide.restart() } }
                KeyButton { width: 190; label: "+"; repeat: true; onPressed: { volume.step(+1); hide.restart() } }
                KeyButton { width: 190; label: "MUTE"; active: volume.muted; onPressed: { volume.toggleMute(); hide.restart() } }
            }
        }
    }
}

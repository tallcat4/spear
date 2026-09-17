// 確認のモーダル(黒で全面を覆い、中央に問いと 2 つの大きなキー)。タッチ専用。
// 今は EXIT の確認にだけ使う。数値入力(Spear.Input の NumericEntry)と同じ文法: 背面は透過にしない、ステータスバーは見せる。
import QtQuick
import Spear.Theme
import Spear.Input

Item {
    id: dlg
    anchors.fill: parent
    visible: false
    z: 1000

    property string title: "CONFIRM"
    property string message: ""
    property string acceptLabel: "OK"
    property string cancelLabel: "CANCEL"
    signal accepted()
    signal cancelled()

    function open() { visible = true }
    function close() { visible = false }

    Rectangle { anchors.fill: parent; color: Theme.bg; MouseArea { anchors.fill: parent } }
    Rectangle {
        width: 900; height: 360
        anchors.centerIn: parent
        color: Theme.panel; border.color: Theme.line; border.width: 1
        Text { x: 24; y: 18; text: dlg.title; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
        Text { x: 24; y: 70; width: parent.width - 48; wrapMode: Text.WordWrap; text: dlg.message
               color: Theme.textBright; font.family: Theme.mono; font.pixelSize: Theme.fsLarge }
        Row {
            anchors.bottom: parent.bottom; anchors.bottomMargin: 24; anchors.horizontalCenter: parent.horizontalCenter
            spacing: 24
            KeyButton { width: 360; height: 110; label: dlg.cancelLabel; onPressed: { dlg.close(); dlg.cancelled() } }
            KeyButton { width: 360; height: 110; label: dlg.acceptLabel; accent: Theme.red; onPressed: { dlg.close(); dlg.accepted() } }
        }
    }
}

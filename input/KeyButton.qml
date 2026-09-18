// Spear.Input — KeyButton: 自前のフラットなキー。装飾なし、押下で反転、長押しリピート対応。
// Qt Quick Controls / Virtual Keyboard には依存しない。
import QtQuick
import Spear.Theme

Rectangle {
    id: key
    property string label: ""
    property string sublabel: ""          // 左上の小さな補助表示(単位など)
    property bool active: false           // トグル状態の表示(反転)
    property bool repeat: false           // 長押しで pressed を繰り返す
    property int repeatDelayMs: 400
    property int repeatIntervalMs: 80
    property int labelSize: Theme.fsLarge
    property color labelColor: Theme.text
    property color accent: "transparent"  // 確定キー(ENTER)などの強調色: 枠と文字がその色、押下でその色に反転(意味のある色だけ使う)
    readonly property bool accented: accent.a > 0
    signal pressed()
    // 受け付けた入力は必ずここを通る(タップ音 → pressed())。音を先に書くので、pressed() の先で拒否音が続いても順に鳴る
    function fire() { Feedback.tap(); key.pressed() }

    implicitWidth: 120; implicitHeight: 84
    color: (active || ma.pressed) && enabled ? (accented ? accent : Theme.invertBg) : Theme.bg
    border.color: enabled ? (accented ? accent : Theme.line) : Theme.lineDim; border.width: accented && enabled ? 2 : 1

    Text { x: 8; y: 5; text: key.sublabel; color: key.enabled ? Theme.textDim : Theme.lineDim
           font.family: Theme.mono; font.pixelSize: Theme.fsSmall; visible: key.sublabel.length > 0 }
    Text { anchors.centerIn: parent; anchors.verticalCenterOffset: key.sublabel.length ? 4 : 0
           text: key.label; font.family: Theme.mono; font.pixelSize: key.labelSize; font.bold: true
           color: (key.active || ma.pressed) && key.enabled ? Theme.invertText : (key.enabled ? (key.accented ? key.accent : key.labelColor) : Theme.lineDim) }

    MouseArea {
        id: ma
        anchors.fill: parent
        enabled: key.enabled
        onClicked: if (!key.repeat) key.fire()
        onPressed: if (key.repeat) { key.fire(); rep.start() }
        onReleased: rep.stop()
        onCanceled: rep.stop()
    }
    Timer {
        id: rep
        interval: key.repeatDelayMs; repeat: true
        onTriggered: { interval = key.repeatIntervalMs; key.fire() }
        onRunningChanged: if (!running) interval = key.repeatDelayMs
    }
}

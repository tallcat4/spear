// 起動時のスプラッシュ(別ウィンドウではなくコンテンツ領域に出す)。
// 名前の提示と同時に、立ち上げの経過(自己診断 → 装置の出現 → FPGA 書き込み(進捗)→ tune 確認)を見せる。
// 立ち上げ(Source::warm_up)が終わるまで App は始められない(Shell が拒否する)。装置の出現待ちだけは CONTINUE で打ち切れる。
import QtQuick
import Spear.Theme

Item {
    id: page
    readonly property bool loading: sys.deviceStateCode === 3 || sys.deviceStateCode === 4 || sys.deviceStateCode === 2   // STANDBY / INITIALIZING / NO_FIRMWARE: 書き込み中は打ち切れない
    readonly property bool canContinue: sys.warmingUp && !loading

    // ---- 名前 ----
    Column {
        id: title
        x: Theme.pad * 4; y: Theme.pad * 5
        spacing: 6
        Text { text: "S.P.E.A.R."; color: Theme.textBright; font.family: Theme.mono; font.pixelSize: 112; font.bold: true }
        Text { text: "Signal Processing & Emission Analysis Receiver"; color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsLarge }
        Text { text: "USRP B210  ·  FZ-G2"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
    }

    // ---- 装置の状態(塗り)と進捗 ----
    Row {
        id: stateRow
        x: title.x; y: title.y + title.height + Theme.pad * 4
        spacing: Theme.pad
        Rectangle {
            width: 260; height: 64
            color: Theme.stateFill(sys.deviceStateCode)
            Text { anchors.centerIn: parent; text: sys.deviceState; color: "#000000"; font.family: Theme.mono; font.pixelSize: Theme.fsLarge; font.bold: true }
        }
        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            Text { text: sys.warmingUp ? (page.loading ? "LOADING — do not disconnect" : "STARTING UP") : "READY"
                   color: page.loading ? Theme.amber : (sys.warmingUp ? Theme.text : Theme.green); font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
            Text { text: sys.deviceEvidence; width: page.width - stateRow.x - 260 - Theme.pad * 6; elide: Text.ElideRight
                   color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
        }
    }
    // FPGA 書き込みの進捗(UHD のログ "FPGA load: NN%" 由来)。取れないときは不定バー
    Item {
        id: progress
        x: title.x; y: stateRow.y + stateRow.height + Theme.pad * 2
        width: page.width - x * 2; height: 40
        visible: sys.warmingUp
        Rectangle { anchors.fill: parent; color: Theme.bg; border.color: Theme.line; border.width: 1 }
        Rectangle {
            x: 2; y: 2; height: parent.height - 4
            width: sys.deviceProgress >= 0 ? Math.round((parent.width - 4) * sys.deviceProgress / 100) : 0
            color: Theme.amber
        }
        // 進捗が取れない待ち(装置の出現待ち・open の前後)は走査線
        Rectangle {
            visible: sys.deviceProgress < 0
            y: 2; height: parent.height - 4; width: 120
            color: Theme.gridMajor
            NumberAnimation on x { from: 2; to: progress.width - 122; duration: 1600; loops: Animation.Infinite; easing.type: Easing.InOutSine; running: progress.visible && sys.deviceProgress < 0 }
        }
        Text { anchors.centerIn: parent
               text: sys.deviceProgress >= 0 ? "FPGA LOAD  " + sys.deviceProgress + " %" : (sys.deviceStateCode === 1 ? "WAITING FOR DEVICE" : "PLEASE WAIT")
               color: sys.deviceProgress >= 0 ? "#000000" : Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
    }

    // ---- 立ち上げの経過(自己診断の項目がそのまま並ぶ)----
    Rectangle {
        x: title.x; y: progress.y + progress.height + Theme.pad * 2
        width: page.width - x * 2; height: page.height - y - Theme.pad * 3
        color: Theme.panel; border.color: Theme.line; border.width: 1
        Text { x: Theme.pad; y: 6; text: "STARTUP DIAGNOSTICS"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
        Column {
            x: Theme.pad; y: 30; spacing: 2
            Repeater {
                model: sys.startupReport
                delegate: Text {
                    required property string modelData
                    required property int index
                    readonly property bool bad: modelData.indexOf("FAILED") >= 0 || modelData.indexOf("MISSING") >= 0
                    readonly property bool last: index === sys.startupReport.length - 1
                    text: (bad ? "✗ " : (last && sys.warmingUp ? "▸ " : "✓ ")) + modelData
                    color: bad ? Theme.red : (last && sys.warmingUp ? Theme.amber : Theme.text)
                    font.family: Theme.mono; font.pixelSize: Theme.fsBase
                }
            }
            Text { visible: sys.startupReport.length === 0; text: sys.sourceName === "b210" ? "..." : "source: " + sys.sourceName + " (no device start-up)"
                   color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
        }
        Text { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: Theme.pad
               text: page.canContinue ? "CONTINUE skips the device wait; apps will keep waiting for the device" : ""
               color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
    }
}

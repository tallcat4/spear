// 起動 → App 一覧 → 選択 (要件 §9.1)。左: App 一覧、右: 装置状態と RF 宣言
import QtQuick
import Spear.Theme
import Spear.Widgets
import Spear.Input

Item {
    id: page
    signal selected(int index)
    signal askGain()
    signal askPort()

    // ---- 左: App 一覧 ----
    Column {
        id: list
        x: Theme.pad * 2; y: Theme.pad * 2
        width: parent.width * 0.55
        spacing: 8
        Text { text: "SELECT APPLICATION"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase; bottomPadding: 8 }
        Repeater {
            model: shell.apps
            // タッチ専用(FZ-G2 タブレットモード)。選択は即座に App へ遷移するので、選択済みの表示は持たない。
            // 反転は押している間だけ(押下の手応え)。
            delegate: Rectangle {
                required property int index
                required property var modelData
                readonly property bool pressedNow: ma.pressed
                width: list.width; height: 96
                color: pressedNow ? Theme.invertBg : Theme.bg
                border.color: Theme.line; border.width: 1
                Text { x: 18; anchors.verticalCenter: parent.verticalCenter; text: (index + 1).toString()
                       color: pressedNow ? Theme.invertText : Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsHuge }
                Column {
                    x: 80; anchors.verticalCenter: parent.verticalCenter; spacing: 4
                    width: parent.width - 80 - 90   // 右端の方向表示(RX/TX)の分を空ける
                    Text { width: parent.width; text: modelData.name; color: pressedNow ? Theme.invertText : Theme.textBright
                           font.family: Theme.mono; font.pixelSize: Theme.fsLarge; font.bold: true; elide: Text.ElideRight }
                    Text { width: parent.width; text: modelData.description; color: pressedNow ? Theme.invertText : Theme.textDim
                           font.family: Theme.mono; font.pixelSize: Theme.fsBase; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight }
                }
                Text { anchors.right: parent.right; anchors.rightMargin: 18; anchors.verticalCenter: parent.verticalCenter
                       text: modelData.direction; color: pressedNow ? Theme.invertText : Theme.textDim
                       font.family: Theme.mono; font.pixelSize: Theme.fsBase }
                MouseArea { id: ma; anchors.fill: parent; onClicked: page.selected(index) }   // 操作音は Main.qml(busy で弾いたときは鳴らさない)
            }
        }
        Text { visible: shell.lastError.length > 0; width: list.width; wrapMode: Text.Wrap; topPadding: 12
               text: "APP START REJECTED: " + shell.lastError; color: Theme.red; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
        Text { visible: shell.busy; topPadding: 12; text: "APPLYING RF CONFIGURATION ..."; color: Theme.amber
               font.family: Theme.mono; font.pixelSize: Theme.fsBase }
    }

    // ---- 右: 装置状態 ----
    Column {
        x: list.x + list.width + Theme.pad * 3; y: Theme.pad * 2
        width: parent.width - x - Theme.pad * 2
        spacing: Theme.pad
        Text { text: "USRP"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase; bottomPadding: 8 }
        Rectangle {
            width: parent.width; height: 64
            color: Theme.stateFill(sys.deviceStateCode)
            Text { anchors.centerIn: parent; text: sys.deviceState; color: "#000000"
                   font.family: Theme.mono; font.pixelSize: Theme.fsHuge; font.bold: true }
        }
        Text { width: parent.width; wrapMode: Text.Wrap; text: sys.deviceEvidence; color: Theme.text
               font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
        Grid {
            columns: 2; columnSpacing: Theme.pad; rowSpacing: Theme.pad
            Readout { label: "SERIAL"; value: sys.serial.length ? sys.serial : "--------"; width: 220 }
            Readout { label: "FX3"; value: sys.fx3State.length ? sys.fx3State : "----"; width: 220 }
            Readout { label: "STAGE"; value: sys.stage + " / 6"; width: 220 }
            Readout { label: "GENERATION"; value: sys.generation.toString(); width: 220 }
        }
        // CENTER / RATE は各 App が決めるのでここには置かない(誤設定で App が動かなくなる)。GAIN と RX PORT(受信端子)が装置の共通設定
        Text { text: "RF  (center / rate are set by each application)"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase; topPadding: 12 }
        Grid {
            columns: 2; columnSpacing: Theme.pad; rowSpacing: Theme.pad
            ValueField {
                label: "GAIN"; value: shell.draftGain.toFixed(1) + " dB"; width: 300; onActivated: page.askGain()
                // AGC のときも数値(AGC を切ったときに戻る値)は残し、白線の X で「無効」を示し、
                // 中央の白い小箱に黒字で AGC — AGC が gain をオーバーライドしていることが読める
                Item {
                    anchors.fill: parent; visible: shell.draftAgc
                    Rectangle { anchors.centerIn: parent; width: Math.sqrt(parent.width * parent.width + parent.height * parent.height); height: 2; color: Theme.textBright
                                rotation: Math.atan2(parent.height, parent.width) * 180 / Math.PI }
                    Rectangle { anchors.centerIn: parent; width: Math.sqrt(parent.width * parent.width + parent.height * parent.height); height: 2; color: Theme.textBright
                                rotation: -Math.atan2(parent.height, parent.width) * 180 / Math.PI }
                    Rectangle {
                        anchors.centerIn: parent; width: agcLabel.implicitWidth + 16; height: agcLabel.implicitHeight + 6; color: Theme.textBright
                        Text { id: agcLabel; anchors.centerIn: parent; text: "AGC"; color: Theme.invertText; font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
                    }
                }
            }
            // 受信端子。選ぶと App 起動時に適用され、運転中はその端子の LED が点く
            ValueField { label: "RX PORT"; value: shell.draftPort; width: 220; onActivated: page.askPort() }
            ValueField { label: "SOURCE"; value: sys.sourceName; width: 300; editable: false; valueColor: Theme.text }
            Readout { label: "LAST RATE"; value: Theme.fmtRate(shell.draftRate); width: 220 }
            Readout { label: "LAST CENTER"; value: Theme.fmtFreq(shell.draftFreq); width: 300 }
        }
    }
}

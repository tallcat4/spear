// 常時表示の状態行: 装置状態(塗りつぶし)/ 個体 / RF / 欠落カウンタ / センサ / host / 時計
import QtQuick
import Spear.Theme
import Spear.Widgets

Rectangle {
    id: bar
    height: Theme.statusBarH
    color: Theme.panel
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }

    Text {
        id: rightText
        anchors.right: parent.right; anchors.rightMargin: Theme.pad; anchors.verticalCenter: parent.verticalCenter
        text: (sys.activeApp.length ? sys.activeApp + "   " : "") + sys.clock
        color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase
    }
    Item {
        anchors.left: parent.left; anchors.right: rightText.left; anchors.rightMargin: Theme.pad; height: parent.height
        clip: true
    Row {
        anchors.verticalCenter: parent.verticalCenter
        x: 0
        spacing: 0

        // 装置状態: 状態色で塗り、文字は黒(最も目に入る要素)
        Rectangle {
            width: 190; height: Theme.statusBarH - 8
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.stateFill(sys.deviceStateCode)
            Text { anchors.centerIn: parent; text: sys.deviceState; color: "#000000"
                   font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
        }
        Item { width: Theme.pad; height: 1 }
        Cell { text: sys.serial.length ? sys.serial : sys.sourceName.toUpperCase(); dim: true }
        Cell { text: "g" + sys.generation; dim: true }
        Sep {}
        Cell { text: Theme.fmtFreq(sys.centerFreq); bright: true }
        Cell { text: Theme.fmtRate(sys.sampleRate) }
        Cell { text: sys.agc ? "AGC" : "G" + sys.gain.toFixed(0) }
        Cell { text: sys.port }   // 受信端子。運転中にどの端子で受けているかを常時見せる(端子の LED と一致するはず)
        Sep {}
        Cell { text: "OVF " + sys.overflowCount; warn: sys.overflowCount > 0 }
        Cell { text: "OOS " + sys.oosCount; warn: sys.oosCount > 0 }
        Cell { text: "TMO " + sys.timeoutCount; warn: sys.timeoutCount > 0 }
        Cell { text: "DISC " + sys.discontinuityCount; warn: sys.discontinuityCount > 0 }
        Sep {}
        Cell { text: sys.sensorsValid ? (sys.loLocked ? "LO LOCK" : "LO UNLOCK") : "LO ----"
               warn: sys.sensorsValid && !sys.loLocked }
        Cell { text: sys.sensorsValid ? sys.rxTemp.toFixed(0) + "°C" : "--°C" }
        Sep {}
        Cell { text: "CPU " + (sys.cpuMhz / 1000).toFixed(1) + "G " + sys.pkgTemp.toFixed(0) + "°C"; warn: sys.pkgTemp >= 90 }
        Cell { text: sys.onAc ? "AC" : "BAT"; warn: !sys.onAc }
        Cell { text: sys.diskFreeGb.toFixed(0) + "G"; warn: sys.diskFreeGb < 2 }
    }
    }

    component Cell: Item {
        property string text: ""
        property bool warn: false
        property bool dim: false
        property bool bright: false
        width: t.implicitWidth + 16; height: Theme.statusBarH
        Text { id: t; anchors.centerIn: parent; text: parent.text
               color: parent.warn ? Theme.red : (parent.bright ? Theme.textBright : (parent.dim ? Theme.textDim : Theme.text))
               font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: parent.warn || parent.bright }
    }
    component Sep: Rectangle { width: 1; height: Theme.statusBarH - 14; anchors.verticalCenter: parent.verticalCenter; color: Theme.line }
}

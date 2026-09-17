// Spear.Widgets — LevelMeter: dB スケールの横バー(Numeric monitor の一種、§9.3)。閾値マーカー付き。
import QtQuick
import Spear.Theme

Item {
    id: meter
    property string label: ""
    property double value: -999      // dB
    property double minDb: -100
    property double maxDb: 0
    property double threshold: NaN   // 表示するなら dB 値
    property color barColor: Theme.green
    implicitWidth: 400; implicitHeight: 56

    function frac(db) { return Math.max(0, Math.min(1, (db - minDb) / (maxDb - minDb))) }

    Text { x: 0; y: 0; text: meter.label; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
    Text { anchors.right: parent.right; y: 0; text: value > -900 ? value.toFixed(1) + " dB" : "--"; color: Theme.text
           font.family: Theme.mono; font.pixelSize: Theme.fsSmall; font.bold: true }
    Rectangle {
        id: track
        x: 0; y: 22; width: parent.width; height: 20
        color: Theme.bg; border.color: Theme.line; border.width: 1
        Rectangle { x: 1; y: 1; height: parent.height - 2; width: Math.round((track.width - 2) * meter.frac(meter.value)); color: meter.barColor }
        // 目盛 10 dB ごと
        Repeater {
            model: Math.round((meter.maxDb - meter.minDb) / 10) + 1
            delegate: Rectangle { required property int index; x: Math.round((track.width - 1) * index * 10 / (meter.maxDb - meter.minDb)); y: 0; width: 1; height: parent.height; color: Theme.gridMajor }
        }
        Rectangle { visible: !isNaN(meter.threshold); x: Math.round((track.width - 3) * meter.frac(meter.threshold)); y: -3; width: 3; height: parent.height + 6; color: Theme.amber }
    }
    Text { x: 0; y: 44; text: meter.minDb.toFixed(0); color: Theme.textDim; font.family: Theme.mono; font.pixelSize: 12 }
    Text { anchors.right: parent.right; y: 44; text: meter.maxDb.toFixed(0); color: Theme.textDim; font.family: Theme.mono; font.pixelSize: 12 }
}

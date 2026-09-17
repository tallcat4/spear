// 設計方針: 黒基調・フラット・高コントラスト・等幅数値。グラデーション/透過/角丸/影/発光は使わない。
// 色は「意味」にのみ使う(緑=正常, 琥珀=注意/遷移中, 赤=異常, 水色=待機/補助)。
pragma Singleton
import QtQuick

QtObject {
    readonly property color bg:        "#000000"
    readonly property color panel:     "#0a0a0a"
    readonly property color line:      "#3a3a3a"
    readonly property color lineDim:   "#202020"
    readonly property color grid:      "#1c1c1c"
    readonly property color gridMajor: "#2c2c2c"
    readonly property color text:      "#d8d8d8"
    readonly property color textDim:   "#7c7c7c"
    readonly property color textBright:"#ffffff"
    readonly property color green:     "#38d038"
    readonly property color amber:     "#f0b020"
    readonly property color red:       "#f03030"
    readonly property color cyan:      "#40c0e0"
    readonly property color gray:      "#606060"
    readonly property color trace:     "#3cf03c"
    readonly property color maxHold:   "#907018"
    readonly property color invertBg:  "#d8d8d8"
    readonly property color invertText:"#000000"

    readonly property string mono: "Hack"
    readonly property int fsSmall: 15
    readonly property int fsBase:  18
    readonly property int fsLarge: 24
    readonly property int fsHuge:  34
    readonly property int statusBarH: 44
    readonly property int softKeyH: 76
    readonly property int pad: 12

    // DeviceState → 色。塗り(fill)はその状態の背景、text は文字色
    function stateFill(code) {
        switch (code) {
        case 6: return green;    // STREAMING
        case 5: return cyan;     // READY
        case 4: return amber;    // INITIALIZING
        case 3: return amber;    // STANDBY
        case 2: return amber;    // NO_FIRMWARE
        case 1: return gray;     // DISCONNECTED
        case 7: return red;      // FAULT
        case 8: return red;      // LOST
        default: return gray;    // UNKNOWN
        }
    }
    function severityColor(s) {
        return s >= 3 ? red : (s == 2 ? amber : (s == 1 ? cyan : textDim))
    }
    function fmtFreq(hz) {
        if (hz >= 1e9) return (hz / 1e9).toFixed(6) + " GHz"
        if (hz >= 1e6) return (hz / 1e6).toFixed(6) + " MHz"
        return (hz / 1e3).toFixed(3) + " kHz"
    }
    function fmtRate(hz) { return (hz / 1e6).toFixed(3) + " Msps" }
    function fmtHz(hz) {
        var a = Math.abs(hz), s = hz < 0 ? "-" : "+"
        if (a >= 1e6) return s + (a / 1e6).toFixed(3) + " MHz"
        return s + (a / 1e3).toFixed(1) + " kHz"
    }
}

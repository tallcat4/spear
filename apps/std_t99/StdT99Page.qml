// STD-T99 DIGICOM のページ (1920×1200)
//   上 : チャネル格子(上段 CH1–9 = 142.934375 MHz〜、下段 CH10–18 = 146.934375 MHz〜。CH18 は呼出チャネル)。タップで選局
//   左 : 通話(状態の帯・発信者・発信者名・宛先・種別。緊急は赤、緊急応答は琥珀の帯)/ 位置(数値と地図。最後の送信は赤いピン、
//        履歴のほかの発信者は橙の点。地図データと下地は widgets/map で ADS-B と共有)
//   右 : 帯域スペクトラム(std_t99.band 400 kHz)/ アイパターン / 受信品質と待受 / 受信履歴
import QtQuick
import Spear.Theme
import Spear.Widgets
import Spear.Input
import Spear.Apps.Std_t99

Item {
    id: page
    required property var app
    required property var ui

    readonly property var call: app.call
    readonly property var st: app.status
    readonly property var standbyNames: ["MONITOR", "INDIVIDUAL", "GROUP"]

    readonly property var softKeys: [
        { label: "STANDBY\n" + standbyNames[app.standby], action: "standby" },
        { label: "OWN ID", action: "ownid" }, { label: "OWN GROUP", action: "group" },
        { label: app.saving ? "SAVE\nSTOP" : "SAVE", action: "save", active: app.saving },
        { label: app.recording ? "REC IQ\n" + app.recordSeconds.toFixed(0) + " s" : "REC IQ", action: "rec", active: app.recording },
        { label: "MUTE", action: "mute", active: app.mute }, { label: "DIAG", action: "diag" } ]
    function softKey(action) {
        switch (action) {
        case "standby": app.standby = (app.standby + 1) % 3; break
        case "ownid":
            ui.askLevel("OWN DEVICE ID  1/2: MAKER CODE (0-127)", "", 0, 127, app.ownMaker, (m) => {
                app.ownMaker = m
                ui.askLevel("OWN DEVICE ID  2/2: INDIVIDUAL ID (0-2097151)", "", 0, 2097151, app.ownIndividual, (v) => app.ownIndividual = v)
            })
            break
        case "group": ui.askLevel("OWN GROUP NUMBER (0 = ALL GROUPS)", "", 0, 63, app.ownGroup, (v) => app.ownGroup = v); break
        case "save": app.toggleSaving(); break
        case "rec": app.toggleRecording(); break
        case "mute": app.mute = !app.mute; break
        case "diag": ui.showDiagnostics(); break
        }
    }

    // 呼制御付加情報: 1 緊急通信 1、2 緊急通信 2、3 緊急応答
    function controlText(c) { return c === 1 ? "EMERGENCY 1" : c === 2 ? "EMERGENCY 2" : c === 3 ? "EMERGENCY RESPONSE" : "" }
    function controlColor(c) { return c === 1 || c === 2 ? Theme.red : c === 3 ? Theme.amber : Theme.gray }

    // ---- 上: チャネル格子 ----
    Item {
        id: grid
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: Theme.pad; anchors.rightMargin: Theme.pad; anchors.topMargin: 6
        height: 2 * 78
        readonly property int cols: 9
        readonly property real cw: (width - (cols - 1) * 4) / cols
        Repeater {
            model: app.numChannels
            delegate: Rectangle {
                required property int index
                readonly property int ch: index + 1
                readonly property bool sel: ch === app.selectedChannel
                x: (index % grid.cols) * (grid.cw + 4); y: Math.floor(index / grid.cols) * 78
                width: grid.cw; height: 72
                color: sel ? "#141414" : Theme.panel
                border.color: sel ? Theme.amber : Theme.line; border.width: sel ? 2 : 1
                Text { x: 10; y: 6; text: "CH " + ch; color: sel ? Theme.amber : Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsLarge; font.bold: true }
                Text { anchors.right: parent.right; anchors.rightMargin: 10; y: 10; visible: ch === 18; text: "CALL"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                Text { x: 10; y: 42; text: (app.channelFreqHz(ch) / 1e6).toFixed(6) + " MHz"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                // 受信中の目印(選択チャネルだけ。ほかのチャネルは受けていない)
                Rectangle { anchors.right: parent.right; anchors.rightMargin: 10; y: 44; width: 14; height: 14; radius: 7
                            visible: sel && page.call.present && page.call.active; color: page.call.emergency ? Theme.red : Theme.green }
                MouseArea { anchors.fill: parent; onClicked: if (!sel) { Feedback.tap(); app.selectedChannel = ch } }
            }
        }
    }

    // ---- 左: 通話 / 位置 / 待受 ----
    Item {
        id: left
        anchors.top: grid.bottom; anchors.bottom: parent.bottom; anchors.left: parent.left
        anchors.margins: Theme.pad; anchors.topMargin: 8
        width: Math.round(parent.width * 0.47)

        // 状態の帯: 受信中(緑)/ 緊急(赤)/ 緊急応答(琥珀)/ 待機
        Rectangle {
            id: banner
            x: 0; y: 0; width: parent.width; height: 64
            readonly property bool active: page.call.present && page.call.active
            color: !active ? Theme.gray : page.call.callControl >= 1 ? page.controlColor(page.call.callControl) : Theme.green
            Text { anchors.left: parent.left; anchors.leftMargin: Theme.pad; anchors.verticalCenter: parent.verticalCenter
                   text: !banner.active ? (page.st.locked ? "SYNC" : "SEARCHING") : (page.controlText(page.call.callControl) || "RECEIVING")
                   color: "#000000"; font.family: Theme.mono; font.pixelSize: Theme.fsHuge; font.bold: true }
            Text { anchors.right: parent.right; anchors.rightMargin: Theme.pad; anchors.verticalCenter: parent.verticalCenter
                   text: "CH " + app.selectedChannel + "  " + (app.channelFreqHz(app.selectedChannel) / 1e6).toFixed(6) + " MHz"
                   color: "#000000"; font.family: Theme.mono; font.pixelSize: Theme.fsLarge; font.bold: true }
        }
        Text { x: 0; y: banner.height + 4; visible: app.tuneWarning !== ""; text: app.tuneWarning; color: Theme.red; font.family: Theme.mono; font.pixelSize: Theme.fsBase }

        // 通話
        Rectangle {
            id: callCard
            x: 0; y: banner.height + 30; width: parent.width; height: 330
            color: Theme.panel; border.color: Theme.line; border.width: 1
            Text { x: Theme.pad; y: 6; text: "CALL" + (page.call.present ? "  #" + page.call.number + "  " + page.call.time : ""); color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            Column {
                x: Theme.pad; y: 30; spacing: 6; width: parent.width - 2 * Theme.pad
                Row {
                    spacing: 6
                    Readout { label: "CALLER"; value: page.call.caller || "—"; width: 300; height: 64; valueColor: Theme.textBright; valueSize: Theme.fsHuge }
                    Readout { label: "CALLER NAME" + (page.call.nameFailed ? "  (CHECK FAILED)" : ""); value: page.call.name || "—"; width: callCard.width - 2 * Theme.pad - 306; height: 64
                              valueColor: Theme.amber; valueSize: Theme.fsHuge }
                }
                Row {
                    spacing: 6
                    Readout { label: "DESTINATION"; value: page.call.destination || "—"; width: 300; height: 58; valueColor: Theme.textBright }
                    Readout { label: "CALL TYPE"; value: page.call.callType || "—"; width: 250; height: 58 }
                    Readout { label: "MESSAGE"; value: page.call.kind || "—"; width: callCard.width - 2 * Theme.pad - 562; height: 58 }
                }
                Row {
                    spacing: 6
                    // 待受の判定(表 Annex 2-4-14): 再生する / しない / 未確定
                    Readout { label: "STANDBY " + page.standbyNames[app.standby]; width: 300; height: 58
                              value: !page.call.present ? "—" : page.call.accepted === 1 ? "PLAY" : page.call.accepted === 0 ? "NOT FOR US" : "PENDING"
                              valueColor: page.call.accepted === 1 ? Theme.green : page.call.accepted === 0 ? Theme.textDim : Theme.amber }
                    Readout { label: "ID EXTENSION"; value: page.call.extension || "—"; width: 250; height: 58 }
                    Readout { label: "FRAMES"; value: page.call.present ? page.call.frames.toFixed(0) + (page.call.ended ? "  END" : "") : "—"; width: callCard.width - 2 * Theme.pad - 562; height: 58 }
                }
                Readout { label: "REQUESTS RECEIVED"; value: page.call.requests || "—"; width: parent.width; height: 58; valueColor: page.call.requests ? Theme.amber : Theme.text }
            }
        }

        // 位置(GPS data 1 / 2、PICH2): 左に数値、右に地図
        Rectangle {
            id: posCard
            x: 0; y: callCard.y + callCard.height + Theme.pad; width: parent.width; height: parent.height - y
            color: Theme.panel; border.color: Theme.line; border.width: 1
            Text { x: Theme.pad; y: 6; text: "POSITION" + (page.call.hasPosition && !page.call.gpsValid ? "  (NO FIX: V)" : ""); color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            Column {
                id: posCol
                x: Theme.pad; y: 30; spacing: 6; width: 300
                Readout { label: "LATITUDE"; value: !page.call.hasPosition ? "—" : page.call.gpsValid ? page.call.latText : "NO FIX"; width: parent.width; height: 58; valueColor: Theme.textBright }
                Readout { label: "LONGITUDE"; value: !page.call.hasPosition ? "—" : page.call.gpsValid ? page.call.lonText : "NO FIX"; width: parent.width; height: 58; valueColor: Theme.textBright }
                Readout { label: "ALTITUDE"; value: page.call.hasPosition && page.call.gpsValid ? page.call.altitude + " m" : "—"; width: parent.width; height: 58 }
                Readout { label: "GPS TIME"; value: page.call.hasGpsTime ? page.call.gpsTime : "—"; width: parent.width; height: 58; valueSize: Theme.fsBase }
                Readout { label: "SPEED / DIR"; value: page.call.hasGpsTime && page.call.gpsTime !== "NO FIX" ? page.call.speedKn.toFixed(1) + " kn  " + page.call.direction + "°" : "—"; width: parent.width; height: 58 }
            }
            Text { x: posCol.x + posCol.width + Theme.pad; y: 6
                   text: "MAP  N up  " + (posMap.autoFit ? "AUTO FIT" : "MANUAL") + "  " + posMap.spanKm.toFixed(0) + " km" + (posMap.pinCount === 0 ? "  NO POSITION" : "")
                   color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            Rectangle {
                x: posCol.x + posCol.width + Theme.pad; y: 30; width: parent.width - x - Theme.pad; height: parent.height - y - Theme.pad
                color: "#000000"; border.color: Theme.lineDim; border.width: 1; clip: true
                PositionMap {
                    id: posMap
                    anchors.fill: parent; anchors.margins: 1
                    pins: app.pins
                    landColor: "#161616"; coastColor: Theme.line; lineColor: Theme.gridMajor; textColor: Theme.text; dimColor: Theme.textDim
                    pinColor: Theme.red; otherColor: Theme.amber; fontFamily: Theme.mono
                    // タッチ: ドラッグでパン、ピンチでズーム(操作すると自動フィットが切れる。FIT で戻す)
                    PinchArea {
                        anchors.fill: parent
                        property real startSpan: 0
                        onPinchStarted: startSpan = posMap.spanKm
                        onPinchUpdated: (p) => { if (p.scale > 0) posMap.zoom((startSpan / p.scale) / posMap.spanKm) }
                        MouseArea {
                            anchors.fill: parent
                            property real lx: 0; property real ly: 0
                            onPressed: (m) => { lx = m.x; ly = m.y }
                            onPositionChanged: (m) => { if (!pressed) return; posMap.pan(m.x - lx, m.y - ly); lx = m.x; ly = m.y }
                        }
                    }
                }
                KeyButton { anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 6; width: 90; height: 50; label: "FIT"; labelSize: Theme.fsBase; enabled: !posMap.autoFit; onPressed: posMap.autoFit = true }
            }
        }
    }

    // ---- 右: スペクトラム / アイ / 品質 / 履歴 ----
    Item {
        id: right
        anchors.top: grid.bottom; anchors.bottom: parent.bottom; anchors.right: parent.right; anchors.left: left.right
        anchors.margins: Theme.pad; anchors.topMargin: 8

        SpectrumView {
            id: band
            x: 0; y: 0; width: parent.width; height: 260
            compact: true; showWaterfall: true
            source: app.bandView; centerFreq: app.channelFreqHz(app.selectedChannel); centerEditable: false
            markerHz: 0; markerLabel: "CH " + app.selectedChannel
            // 同じ群(CH1–9 / CH10–18)のチャネル。選択チャネルが中心
            markers: {
                var m = [], f0 = app.channelFreqHz(app.selectedChannel)
                for (var c = 1; c <= app.numChannels; ++c) {
                    var d = app.channelFreqHz(c) - f0
                    if (c !== app.selectedChannel && Math.abs(d) < 190e3) m.push({ hz: d, label: "" + c, color: Theme.gridMajor })
                }
                return m
            }
            onAskRef: ui.askRef(app.bandView.dbMax, (v) => { app.bandView.dbMin = v - 90; app.bandView.dbMax = v })
        }
        EyeDiagram {
            id: eye
            x: 0; y: band.height + Theme.pad; width: Math.round(parent.width * 0.48); height: 290
            source: app.eye
            title: "EYE  CH " + app.selectedChannel + "  4FSK 2400 Bd"
            yMin: -5; yMax: 5; levels: [-3, -1, 1, 3]
        }
        // 受信品質
        Rectangle {
            x: eye.width + Theme.pad; y: eye.y; width: parent.width - x; height: eye.height
            color: Theme.panel; border.color: Theme.line; border.width: 1
            Grid {
                x: Theme.pad; y: Theme.pad; columns: 2; spacing: 6
                readonly property int cw: (parent.width - 2 * Theme.pad - 6) / 2
                Readout { label: "SYNC"; value: page.st.locked ? "LOCKED" : "SEARCH"; width: parent.cw; height: 58; valueColor: page.st.locked ? Theme.green : Theme.textDim }
                Readout { label: "CRC OK / FRAMES"; value: page.st.crcFrames.toFixed(0) + " / " + page.st.frames.toFixed(0); width: parent.cw; height: 58 }
                Readout { label: "LEVEL"; value: page.st.powerDb > -900 ? page.st.powerDb.toFixed(1) + " dBFS" : "—"; width: parent.cw; height: 58 }
                Readout { label: "FREQ OFFSET"; value: page.st.haveOffset ? page.st.offsetHz.toFixed(0) + " Hz" : "—"; width: parent.cw; height: 58 }
                Readout { label: "DSP / DROPS"; value: (app.dspLoad * 100).toFixed(0) + " %  " + app.losslessDrops.toFixed(0) + (app.voiceDrops > 0 ? "  V" + app.voiceDrops.toFixed(0) : "")
                          width: parent.cw; height: 58; valueColor: app.losslessDrops > 0 || app.voiceDrops > 0 ? Theme.red : Theme.text }
                Readout { label: "AUDIO"; value: app.audioError !== "" ? app.audioError : (app.mute ? "MUTE" : "TOKUDER 8 kHz"); width: parent.cw; height: 58
                          valueColor: app.audioError !== "" ? Theme.red : Theme.text }
                Readout { label: "STANDBY  /  OWN GROUP"; value: page.standbyNames[app.standby] + "  " + ("0" + app.ownGroup).slice(-2); width: parent.cw; height: 58; valueColor: Theme.amber }
                Readout { label: "OWN DEVICE ID"; value: app.ownId; width: parent.cw; height: 58 }
            }
            Text { x: Theme.pad; anchors.bottom: parent.bottom; anchors.bottomMargin: 6; width: parent.width - 2 * Theme.pad; elide: Text.ElideMiddle
                   visible: app.recording || app.recordError !== ""; text: app.recordError !== "" ? app.recordError : "REC " + app.recordPath
                   color: app.recordError !== "" ? Theme.red : Theme.amber; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
        }
        // 受信履歴(送信ごと、新しい順)
        Rectangle {
            id: hist
            x: 0; y: eye.y + eye.height + Theme.pad; width: parent.width; height: parent.height - y
            color: Theme.panel; border.color: Theme.line; border.width: 1
            Text { x: Theme.pad; y: 6; text: "HISTORY  (" + app.history.length + ")    TIME      CALLER        DESTINATION   TYPE         NAME"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            ListView {
                x: Theme.pad; y: 30; width: parent.width - 2 * Theme.pad; height: parent.height - 36
                clip: true; model: app.history; interactive: true; boundsBehavior: Flickable.StopAtBounds
                delegate: Item {
                    required property var modelData
                    width: ListView.view.width; height: 30
                    Rectangle { anchors.left: parent.left; width: 6; height: 24; y: 3; color: page.controlColor(modelData.callControl); visible: modelData.callControl >= 1 }
                    Text { x: 14; y: 3; width: parent.width - 14; elide: Text.ElideRight
                           text: ("#" + modelData.number + "     ").slice(0, 6) + modelData.time + "  " + ((modelData.caller || "—") + "             ").slice(0, 13) + " "
                                 + ((modelData.destination || "—") + "             ").slice(0, 13) + " " + ((modelData.callType || "—") + "            ").slice(0, 12) + " " + modelData.name
                           color: modelData.emergency ? Theme.red : Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
                }
            }
        }
    }
}

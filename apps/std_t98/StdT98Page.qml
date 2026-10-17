// STD-T98 MONITOR のページ (1920×1200)
//   上 : 30ch 俯瞰スペクトラム(std_t98.band 400 kHz、チャネルマーカー付き)
//   中 : チャネル格子(30 ch: 電力・CSM、タップで選択。数字は増やさない)
//   下 : 選択チャネル — アイパターン / チャネル IQ スペクトラム / 読み出し(フレーム統計・秘話の鍵)
import QtQuick
import Spear.Theme
import Spear.Widgets
import Spear.Input

Item {
    id: page
    required property var app
    required property var ui

    readonly property var sel: app.selected
    readonly property var chans: app.channels

    // チャネル選択は格子のタップで行う(CH−/CH+ は置かない)。
    // 帯域中心・LO オフセットは規格と App が決める値なので操作キーは置かない(誤設定で受信できなくなる)。個体差は site.conf。
    readonly property var softKeys: [
        { label: "ALL CH AUDIO", action: "all", active: app.allChannelAudio },
        { label: "MUTE", action: "mute", active: app.mute },
        null, null, null, null, { label: "DIAG", action: "diag" } ]
    function softKey(action) {
        switch (action) {
        case "all": app.allChannelAudio = !app.allChannelAudio; break
        case "diag": ui.showDiagnostics(); break
        case "mute": app.mute = !app.mute; break
        }
    }
    // チャネルの活動はフレーム同期(と CRC)だけで示す。緑 = 同期中(同期を失って 1 s 以内も含む)。電力は判定に使わない
    function chColor(c) { return c.active ? Theme.green : Theme.gray }
    // チャネルマーカー(帯域中心からのオフセット)。選択 ch は琥珀
    readonly property var markers: {
        var m = []
        for (var i = 0; i < app.numChannels; ++i)
            m.push({ hz: app.channelOffsetHz(i), label: (i % 5 === 0 || i === app.numChannels - 1) ? String(i + 1) : "",
                     color: i === app.selectedChannel ? Theme.amber : Theme.gridMajor })
        return m
    }

    // ---- 上: 俯瞰スペクトラム ----
    SpectrumView {
        id: band
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        height: Math.round(parent.height * 0.36)
        source: app.bandView; centerFreq: app.bandCenterHz; centerEditable: false
        markers: page.markers
        markerHz: app.channelOffsetHz(app.selectedChannel); markerLabel: "CH " + (app.selectedChannel + 1)
        onAskRef: ui.askRef(app.bandView.dbMax, (v) => { app.bandView.dbMin = v - 90; app.bandView.dbMax = v })
    }

    // ---- 中: チャネル格子 ----
    Item {
        id: grid
        anchors.top: band.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: Theme.pad; anchors.rightMargin: Theme.pad
        height: 2 * 68 + 6
        readonly property int cols: 15
        readonly property real cw: (width - (cols - 1) * 4) / cols
        Repeater {
            model: page.chans
            delegate: Rectangle {
                required property var modelData
                required property int index
                readonly property bool selectedCh: index === app.selectedChannel
                x: (index % grid.cols) * (grid.cw + 4); y: Math.floor(index / grid.cols) * 68 + 6
                width: grid.cw; height: 62
                color: selectedCh ? "#141414" : Theme.panel
                border.color: selectedCh ? Theme.amber : Theme.line; border.width: selectedCh ? 2 : 1
                Text { x: 8; y: 3; text: "CH " + modelData.ch; color: selectedCh ? Theme.amber : Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
                Text { anchors.right: parent.right; anchors.rightMargin: 8; y: 5; text: modelData.active ? "SYNC" : ""; color: Theme.green; font.family: Theme.mono; font.pixelSize: Theme.fsSmall; font.bold: true }
                Text { x: 8; y: 24; text: modelData.csm.length ? "CSM " + modelData.csm : (modelData.freq / 1e6).toFixed(5) + " MHz"
                       color: modelData.csm.length ? Theme.textBright : Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                // 活動の帯(同期中は緑)。電力のバーは置かない(AGC で揺れて閾値の近くでちらつくため)
                Rectangle { x: 8; y: 46; width: parent.width - 16; height: 6; color: page.chColor(modelData); opacity: modelData.active ? 1 : 0.25 }
                MouseArea { anchors.fill: parent; onClicked: if (index !== app.selectedChannel) { Feedback.tap(); app.selectedChannel = index } }
            }
        }
    }

    // ---- 下: 選択チャネル ----
    Item {
        id: lower
        anchors.top: grid.bottom; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: Theme.pad; anchors.topMargin: 6

        EyeDiagram {
            id: eye
            x: 0; y: 0; width: Math.round(parent.width * 0.36); height: parent.height
            source: app.eye
            title: "EYE  CH " + (app.selectedChannel + 1) + "  " + Theme.fmtFreq(app.channelFreqHz(app.selectedChannel)) + "  4FSK 2400 Bd  (2 symbols)"
            yMin: -4.5; yMax: 4.5; levels: [-3, -1, 0, 1, 3]
        }
        // チャネル IQ スペクトラム(観測点: std_t98.channel 62.5 kHz)
        Item {
            id: chanSpec
            x: eye.width + Theme.pad; y: 0; width: Math.round(parent.width * 0.22); height: parent.height
            Text { x: Theme.pad; y: 4; text: "CHANNEL IQ  std_t98.channel  62.5 kHz"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            SpectrumView {
                anchors.fill: parent; anchors.topMargin: 20
                compact: true
                source: app.channelView; centerFreq: app.channelFreqHz(app.selectedChannel); centerEditable: false
                // dB レンジは帯域表示に追従(App 側で同期)。REF は帯域側を編集する
                onAskRef: ui.askRef(app.bandView.dbMax, (v) => { app.bandView.dbMin = v - 90; app.bandView.dbMax = v })
            }
        }
        // 読み出しとフレームログ
        Rectangle {
            x: chanSpec.x + chanSpec.width + Theme.pad; y: 0; width: parent.width - x; height: parent.height
            color: Theme.panel; border.color: Theme.line; border.width: 1
            Column {
                x: Theme.pad; y: Theme.pad; spacing: 6
                Row {
                    spacing: 6
                    Rectangle {
                        width: 130; height: 52; color: page.chColor(page.sel)
                        Text { anchors.centerIn: parent; text: page.sel.active ? "SYNC" : "IDLE"; color: "#000000"
                               font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
                    }
                    Readout { label: "POWER"; value: page.sel.power > -900 ? page.sel.power.toFixed(1) + " dB" : "--"; width: 130; height: 52 }
                    Readout { label: "SYNC / BEST SSE"; value: page.sel.syncs + " / " + (page.sel.bestSse < 1e8 ? page.sel.bestSse.toFixed(2) : "--"); width: 200; height: 52 }
                    Readout { label: "SPS"; value: page.sel.sps > 0 ? page.sel.sps.toFixed(3) : "--"; width: 110; height: 52 }
                    Readout { label: "FREQ ERR"; value: page.sel.active ? page.sel.freqErrEst.toFixed(0) + " Hz" : "--"; width: 130; height: 52 }
                }
                Row {
                    spacing: 6
                    Readout { label: "FRAMES"; value: String(page.sel.frames); width: 110; height: 52; valueColor: Theme.green }
                    Readout { label: "SACCH OK"; value: String(page.sel.sacchOk); width: 120; height: 52 }
                    Readout { label: "PICH OK"; value: String(page.sel.pichOk); width: 110; height: 52 }
                    Readout { label: "LOCK / FLY / DROP"; value: page.sel.locks + " / " + page.sel.flywheel + " / " + page.sel.dropped; width: 170; height: 52 }
                    Readout { label: "CSM"; value: page.sel.csm.length ? page.sel.csm : "--"; width: 170; height: 52; valueColor: Theme.textBright }
                    Readout { label: "TOTAL / DROPS / DSP"; value: app.totalFrames.toFixed(0) + " / " + app.losslessDrops.toFixed(0) + " / " + (app.dspLoad * 100).toFixed(0) + "%"; width: 200; height: 52
                              valueColor: app.losslessDrops > 0 ? Theme.red : Theme.text }
                }
                Row {
                    spacing: 6
                    // 秘話: 呼の状態と鍵。無音の解析またはモデルによる探索で見つけ、呼をまたいで保持する
                    Readout { label: "SECRET"; width: 214; height: 52
                              value: !page.sel.secret ? (page.sel.key > 0 ? "clear  (last K " + page.sel.key + ")" : "clear")
                                     : (page.sel.key > 0 ? "KEY " + page.sel.key + (page.sel.secretStatus === "searching" ? "  ..." : "")
                                        : page.sel.secretStatus.toUpperCase())
                              valueSize: page.sel.secret ? Theme.fsLarge : Theme.fsBase
                              valueColor: !page.sel.secret ? Theme.textDim : (page.sel.key > 0 ? Theme.green : (page.sel.secretStatus === "miss" ? Theme.red : Theme.amber)) }
                    Readout { label: "LAST FRAME"; width: 500; height: 52
                              value: page.sel.lastFrame.type !== undefined
                                     ? (page.sel.lastFrame.type + "  RICH M=" + page.sel.lastFrame.richM
                                        + (page.sel.lastFrame.msgType !== undefined ? "  msg=" + page.sel.lastFrame.msgType + " call=" + page.sel.lastFrame.callStat + " user=" + page.sel.lastFrame.userCode : "")
                                        + (page.sel.lastFrame.crcOk !== undefined ? (page.sel.lastFrame.crcOk ? "  CRC OK" : "  CRC BAD") : ""))
                                     : "--"
                              valueSize: Theme.fsBase
                              valueColor: page.sel.lastFrame.crcOk === false ? Theme.red : Theme.text }
                }
                Text { visible: app.audioError.length > 0; text: "AUDIO ERROR  " + app.audioError; color: Theme.red; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            }
        }
    }
}

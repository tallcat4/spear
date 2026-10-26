// STD-T98 MONITOR のページ (1920×1200)
//   上 : 97ch 俯瞰スペクトラム(std_t98.band 800 kHz、チャネルマーカー付き)
//   中 : 絞り込み付き97ch一覧(可変列数、スクロール、タップで選択)
//   下 : 選択チャネル — アイパターン / 読み出し(フレーム統計・秘話の鍵)
import QtQuick
import QtQuick.Controls
import Spear.Theme
import Spear.Widgets
import Spear.Input

Item {
    id: page
    required property var app
    required property var ui

    readonly property var sel: app.selected
    readonly property var chans: app.channels

    property int channelFilter: 0
    readonly property var visibleChannels: chans.filter(function(c) {
        return channelFilter === 0 || (channelFilter === 1 && c.index < 30)
            || (channelFilter === 2 && c.index >= 30 && c.index < 82)
            || (channelFilter === 3 && c.index >= 82) || (channelFilter === 4 && c.active)
    })
    readonly property int activeCount: chans.filter(function(c) { return c.active }).length

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
            m.push({ hz: app.channelOffsetHz(i), label: (Math.round(app.channelOffsetHz(i) / app.channelSpacingHz) % 5 === 0) ? app.channelLabel(i) : "",
                     color: i === app.selectedChannel ? Theme.amber : Theme.gridMajor })
        return m
    }

    // ---- 上: 俯瞰スペクトラム ----
    SpectrumView {
        id: band
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        height: Math.round(parent.height * 0.30)
        source: app.bandView; centerFreq: app.bandCenterHz; centerEditable: false
        markers: page.markers
        markerHz: app.channelOffsetHz(app.selectedChannel); markerLabel: "CH " + app.channelLabel(app.selectedChannel)
        onAskRef: ui.askRef(app.bandView.dbMax, (v) => { app.bandView.dbMin = v - 90; app.bandView.dbMax = v })
    }

    // Filters change only the view: all 97 receivers and audio continue running.
    Flow {
        id: filters
        anchors.top: band.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: Theme.pad; anchors.rightMargin: Theme.pad
        spacing: 8
        Repeater {
            model: ["ALL 97", "CH 1–30", "CH 31–82", "AIR S1–S15", "ACTIVE " + page.activeCount]
            delegate: Button {
                required property string modelData
                required property int index
                text: modelData; checkable: true; checked: page.channelFilter === index
                height: 56
                leftPadding: 20; rightPadding: 20
                implicitWidth: Math.max(140, contentItem.implicitWidth + leftPadding + rightPadding)
                background: Rectangle { color: parent.checked ? Theme.amber : Theme.panel; border.color: Theme.line }
                contentItem: Text { text: parent.text; color: parent.checked ? "#000000" : Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                onClicked: { page.channelFilter = index; grid.positionViewAtBeginning() }
            }
        }
        Button {
            text: "SELECTED: " + app.channelLabel(app.selectedChannel)
            height: 56
            leftPadding: 20; rightPadding: 20
            implicitWidth: Math.max(140, contentItem.implicitWidth + leftPadding + rightPadding)
            background: Rectangle { color: Theme.panel; border.color: Theme.amber }
            contentItem: Text { text: parent.text; color: Theme.amber; font.family: Theme.mono; font.pixelSize: Theme.fsBase; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            onClicked: { page.channelFilter = 0; grid.positionViewAtIndex(app.selectedChannel, GridView.Contain) }
        }
    }
    GridView {
        id: grid
        anchors.top: filters.bottom; anchors.topMargin: 8; anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: Theme.pad; anchors.rightMargin: Theme.pad
        height: Math.min(216, page.height * 0.28)
        clip: true
        cellWidth: width / Math.max(1, Math.floor((width - 16) / 160))
        cellHeight: 72
        model: page.visibleChannels
        ScrollBar.vertical: ScrollBar { }
        delegate: Rectangle {
            required property var modelData
            readonly property bool selectedCh: modelData.index === app.selectedChannel
            width: grid.cellWidth - 8; height: 66
            color: selectedCh ? "#141414" : Theme.panel
            border.color: selectedCh ? Theme.amber : Theme.line; border.width: selectedCh ? 2 : 1
            Text { x: 8; y: 3; text: "CH " + modelData.ch; color: selectedCh ? Theme.amber : Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
            Text { anchors.right: parent.right; anchors.rightMargin: 8; y: 5; text: modelData.active ? "SYNC" : ""; color: Theme.green; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            Text { x: 8; y: 25; width: parent.width - 16; elide: Text.ElideRight
                text: modelData.csm.length ? "CSM " + modelData.csm : (modelData.freq / 1e6).toFixed(5) + " MHz"
                color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            Rectangle { x: 8; y: 50; width: parent.width - 16; height: 6; color: page.chColor(modelData); opacity: modelData.active ? 1 : 0.25 }
            MouseArea { anchors.fill: parent; onClicked: { Feedback.tap(); app.selectedChannel = modelData.index } }
        }
        Text { anchors.centerIn: parent; visible: grid.count === 0; text: "No active channels"; color: Theme.textDim }
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
            title: "EYE  CH " + app.channelLabel(app.selectedChannel) + "  " + Theme.fmtFreq(app.channelFreqHz(app.selectedChannel)) + "  4FSK 2400 Bd  (2 symbols)"
            yMin: -10; yMax: 10; levels: [-10, -3, -1, 0, 1, 3, 10]
        }
        // 読み出しとフレームログ
        Rectangle {
            x: eye.width + Theme.pad; y: 0; width: parent.width - x; height: parent.height
            color: Theme.panel; border.color: Theme.line; border.width: 1
            ScrollView {
                id: details
                anchors.fill: parent; anchors.margins: Theme.pad
                contentWidth: availableWidth
                clip: true
                Flow {
                    width: details.availableWidth; spacing: 6
                    Rectangle {
                        width: 130; height: 52; color: page.chColor(page.sel)
                        Text { anchors.centerIn: parent; text: page.sel.active ? "SYNC" : "IDLE"; color: "#000000"
                               font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
                    }
                    Readout { label: "POWER"; value: page.sel.power > -900 ? page.sel.power.toFixed(1) + " dB" : "--"; width: 130; height: 52 }
                    Readout { label: "SYNC / BEST SSE"; value: page.sel.syncs + " / " + (page.sel.bestSse < 1e8 ? page.sel.bestSse.toFixed(2) : "--"); width: 200; height: 52 }
                    Readout { label: "SPS"; value: page.sel.sps > 0 ? page.sel.sps.toFixed(3) : "--"; width: 110; height: 52 }
                    Readout { label: "FREQ ERR"; value: page.sel.active ? page.sel.freqErrEst.toFixed(0) + " Hz" : "--"; width: 130; height: 52 }
                    Readout { label: "FRAMES"; value: String(page.sel.frames); width: 110; height: 52; valueColor: Theme.green }
                    Readout { label: "SACCH OK"; value: String(page.sel.sacchOk); width: 120; height: 52 }
                    Readout { label: "PICH OK"; value: String(page.sel.pichOk); width: 110; height: 52 }
                    Readout { label: "LOCK / FLY / DROP"; value: page.sel.locks + " / " + page.sel.flywheel + " / " + page.sel.dropped; width: 170; height: 52 }
                    Readout { label: "CSM"; value: page.sel.csm.length ? page.sel.csm : "--"; width: 170; height: 52; valueColor: Theme.textBright }
                    Readout { label: "TOTAL / DROPS / DSP"; value: app.totalFrames.toFixed(0) + " / " + app.losslessDrops.toFixed(0) + " / " + (app.dspLoad * 100).toFixed(0) + "%"; width: 200; height: 52
                              valueColor: app.losslessDrops > 0 ? Theme.red : Theme.text }
                    // 秘話: 系列とコード。ソルバー誘導AIで判定し、呼ごとに確認する
                    Readout { label: "SECRET"; width: 250; height: 52
                              value: !page.sel.secret ? (page.sel.key > 0 ? "clear  (last K " + page.sel.key + ")" : "clear")
                                     : (page.sel.key > 0 ? "S" + page.sel.series + " / K " + page.sel.key + (page.sel.secretStatus === "searching" ? "  ..." : "")
                                        : page.sel.confirmed ? "CLEAR (S0 / K0)" : page.sel.secretStatus.toUpperCase())
                              valueSize: page.sel.secret ? Theme.fsLarge : Theme.fsBase
                              valueColor: !page.sel.secret ? Theme.textDim : (page.sel.confirmed ? Theme.green : (page.sel.secretStatus === "miss" ? Theme.red : Theme.amber)) }
                    Readout { label: "LAST FRAME"; width: Math.min(500, parent.width); height: 52
                              value: page.sel.lastFrame.type !== undefined
                                     ? (page.sel.lastFrame.type + "  RICH M=" + page.sel.lastFrame.richM
                                        + (page.sel.lastFrame.msgType !== undefined ? "  msg=" + page.sel.lastFrame.msgType + " call=" + page.sel.lastFrame.callStat + " user=" + page.sel.lastFrame.userCode : "")
                                        + (page.sel.lastFrame.crcOk !== undefined ? (page.sel.lastFrame.crcOk ? "  CRC OK" : "  CRC BAD") : ""))
                                     : "--"
                              valueSize: Theme.fsBase
                              valueColor: page.sel.lastFrame.crcOk === false ? Theme.red : Theme.text }
                Text { visible: app.audioError.length > 0; text: "AUDIO ERROR  " + app.audioError; color: Theme.red; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                }
            }
        }
    }
}

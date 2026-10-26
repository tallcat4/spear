// STD-T98 MONITOR のページ (1920×1200)
//   上 : 97ch 俯瞰スペクトラム(std_t98.band 800 kHz、チャネルマーカー付き)
// Channel navigation uses filters and fixed pages; no flicking or scrolling.
//   下 : 選択チャネル — アイパターン / 読み出し(フレーム統計・SECRETの鍵)
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
    property int channelPage: 0
    readonly property var channelIndices: {
        if (channelFilter === 4) return chans.filter(c => c.active).map(c => c.index)
        var indices = []
        for (var i = 0; i < app.numChannels; ++i)
            if (channelFilter === 0 || (channelFilter === 1 && i < 30)
                || (channelFilter === 2 && i >= 30 && i < 82)
                || (channelFilter === 3 && i >= 82)) indices.push(i)
        return indices
    }
    readonly property int channelColumns: Math.max(1, Math.floor(grid.width / 160))
    readonly property int channelRows: page.height < 850 ? 2 : 3
    readonly property int pageSize: channelColumns * channelRows
    readonly property int pageCount: Math.max(1, Math.ceil(channelIndices.length / pageSize))
    readonly property int currentPage: Math.min(channelPage, pageCount - 1)
    onPageCountChanged: channelPage = Math.min(channelPage, pageCount - 1)
    function selectFilter(index) {
        channelFilter = index
        channelPage = 0
    }
    function showSelected() {
        channelFilter = 0
        channelPage = Math.floor(app.selectedChannel / pageSize)
    }
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
                onClicked: page.selectFilter(index)
            }
        }
        Button {
            text: "SELECTED: " + app.channelLabel(app.selectedChannel)
            height: 56
            leftPadding: 20; rightPadding: 20
            implicitWidth: Math.max(140, contentItem.implicitWidth + leftPadding + rightPadding)
            background: Rectangle { color: Theme.panel; border.color: Theme.amber }
            contentItem: Text { text: parent.text; color: Theme.amber; font.family: Theme.mono; font.pixelSize: Theme.fsBase; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            onClicked: page.showSelected()
        }
    }
    Row {
        id: pageControls
        anchors.top: filters.bottom; anchors.topMargin: 6
        anchors.left: parent.left; anchors.leftMargin: Theme.pad
        spacing: 8
        Button {
            text: "PREV"; enabled: page.currentPage > 0; width: 100; height: 48
            onClicked: { Feedback.tap(); page.channelPage = page.currentPage - 1 }
            background: Rectangle { color: Theme.panel; border.color: Theme.line }
            contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        }
        ComboBox {
            width: 180; height: 48
            model: page.pageCount; currentIndex: page.currentPage
            displayText: "PAGE " + (page.currentPage + 1) + " / " + page.pageCount
            delegate: ItemDelegate { required property int index; width: 180; height: 48; text: "PAGE " + (index + 1); highlighted: index === page.currentPage }
            onActivated: function(index) { page.channelPage = index }
            background: Rectangle { color: Theme.panel; border.color: Theme.line }
            contentItem: Text { text: parent.displayText; color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase; leftPadding: 12; verticalAlignment: Text.AlignVCenter }
        }
        Button {
            text: "NEXT"; enabled: page.currentPage + 1 < page.pageCount; width: 100; height: 48
            onClicked: { Feedback.tap(); page.channelPage = page.currentPage + 1 }
            background: Rectangle { color: Theme.panel; border.color: Theme.line }
            contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        }
        Text {
            height: 48; verticalAlignment: Text.AlignVCenter
            text: page.channelFilter === 4 ? "LIVE ACTIVE CHANNELS" : ""
            color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall
        }
    }
    Item {
        id: grid
        anchors.top: pageControls.bottom; anchors.topMargin: 6; anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: Theme.pad; anchors.rightMargin: Theme.pad
        height: page.channelRows * 72
        clip: true
        readonly property real cellWidth: width / page.channelColumns
        // Fixed slot delegates survive meter updates; only their readouts change.
        Repeater {
            model: page.pageSize
            delegate: Rectangle {
                required property int index
                readonly property int channelIndex: page.channelIndices[page.currentPage * page.pageSize + index] ?? -1
                readonly property var modelData: channelIndex >= 0 ? page.chans[channelIndex] : ({index: -1, ch: "", active: false, csm: "", freq: 0})
                visible: channelIndex >= 0
                x: (index % page.channelColumns) * grid.cellWidth
                y: Math.floor(index / page.channelColumns) * 72
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
        }
        Text { anchors.centerIn: parent; visible: page.channelIndices.length === 0; text: "No active channels"; color: Theme.textDim }
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
                Column {
                    width: details.availableWidth; spacing: 10
                    Column {
                        width: parent.width; spacing: 2
                        Readout { label: "CSM"; value: page.sel.csm.length ? page.sel.csm : "—"; width: parent.width; height: 64; valueSize: 32; valueColor: Theme.textBright; framed: false }
                        Text { x: Theme.pad; width: parent.width - 2 * Theme.pad; text: page.sel.petname || "—"
                            color: Theme.amber; font.family: Theme.mono; font.pixelSize: Theme.fsBase; wrapMode: Text.Wrap }
                    }
                    Row {
                        width: parent.width; spacing: 8
                        Readout { label: "UC"; value: page.sel.userCode >= 0 ? String(page.sel.userCode).padStart(3, "0") : "—"; width: (parent.width - 8) / 2; height: 60; valueColor: Theme.textBright }
                        Readout { label: "MANUFACTURER CODE"; value: page.sel.makerCode >= 0 ? String(page.sel.makerCode) : "—"; width: (parent.width - 8) / 2; height: 60; valueColor: Theme.textBright }
                    }
                    Flow {
                        width: parent.width; spacing: 6
                        Readout { label: "RECEPTION"; value: page.sel.active ? "SYNC" : "IDLE / LAST RECEIVED"; width: Math.min(300, parent.width); height: 56; valueSize: Theme.fsBase; valueColor: page.sel.active ? Theme.green : Theme.textDim }
                        Readout { label: "SECRET"; width: Math.min(300, parent.width); height: 56
                            value: !page.sel.active ? "STANDBY" : !page.sel.secret ? "CLEAR" : page.sel.confirmed ? (page.sel.key === 0 ? "CLEAR (00000)" : (page.sel.series ? String(page.sel.series).padStart(2, "0") + "-" : "") + String(page.sel.key).padStart(5, "0")) : page.sel.secretStatus.toUpperCase()
                            valueColor: !page.sel.active ? Theme.textDim : page.sel.confirmed ? Theme.green : Theme.amber }
                    }
                    Text { width: parent.width; visible: app.cacheWarning.length > 0; text: app.cacheWarning; wrapMode: Text.Wrap; color: Theme.red; font.pixelSize: Theme.fsSmall }
                    Text { width: parent.width; visible: app.audioError.length > 0; text: "AUDIO ERROR  " + app.audioError; wrapMode: Text.Wrap; color: Theme.red; font.pixelSize: Theme.fsSmall }
                    Button {
                        id: diagnosticsToggle
                        text: checked ? "DETAILS ▴" : "DETAILS ▾"
                        checkable: true; height: 48
                        leftPadding: 20; rightPadding: 20
                        background: Rectangle { color: Theme.panel; border.color: diagnosticsToggle.checked ? Theme.amber : Theme.line }
                        contentItem: Text { text: diagnosticsToggle.text; color: Theme.text; font.pixelSize: Theme.fsBase; verticalAlignment: Text.AlignVCenter }
                    }
                    Flow {
                        visible: diagnosticsToggle.checked
                        width: parent.width; spacing: 6
                    Readout { label: "POWER"; value: page.sel.power > -900 ? page.sel.power.toFixed(1) + " dB" : "--"; width: 130; height: 52 }
                    Readout { label: "SYNC / BEST SSE"; value: page.sel.syncs + " / " + (page.sel.bestSse < 1e8 ? page.sel.bestSse.toFixed(2) : "--"); width: 200; height: 52 }
                    Readout { label: "SPS"; value: page.sel.sps > 0 ? page.sel.sps.toFixed(3) : "--"; width: 110; height: 52 }
                    Readout { label: "FREQ ERR"; value: page.sel.active ? page.sel.freqErrEst.toFixed(0) + " Hz" : "--"; width: 130; height: 52 }
                    Readout { label: "FRAMES"; value: String(page.sel.frames); width: 110; height: 52; valueColor: Theme.green }
                    Readout { label: "SACCH OK"; value: String(page.sel.sacchOk); width: 120; height: 52 }
                    Readout { label: "PICH OK"; value: String(page.sel.pichOk); width: 110; height: 52 }
                    Readout { label: "LOCK / FLY / DROP"; value: page.sel.locks + " / " + page.sel.flywheel + " / " + page.sel.dropped; width: 170; height: 52 }
                    Readout { label: "TOTAL / DROPS / DSP"; value: app.totalFrames.toFixed(0) + " / " + app.losslessDrops.toFixed(0) + " / " + (app.dspLoad * 100).toFixed(0) + "%"; width: 200; height: 52
                              valueColor: app.losslessDrops > 0 ? Theme.red : Theme.text }
                    Readout { label: "LAST FRAME"; width: Math.min(500, parent.width); height: 52
                              value: page.sel.lastFrame.type !== undefined
                                     ? (page.sel.lastFrame.type + "  RICH M=" + page.sel.lastFrame.richM
                                        + (page.sel.lastFrame.msgType !== undefined ? "  msg=" + page.sel.lastFrame.msgType + " call=" + page.sel.lastFrame.callStat + " user=" + page.sel.lastFrame.userCode : "")
                                        + (page.sel.lastFrame.crcOk !== undefined ? (page.sel.lastFrame.crcOk ? "  CRC OK" : "  CRC BAD") : ""))
                                     : "--"
                              valueSize: Theme.fsBase
                              valueColor: page.sel.lastFrame.crcOk === false ? Theme.red : Theme.text }
                    }
                }
            }
        }
    }
}

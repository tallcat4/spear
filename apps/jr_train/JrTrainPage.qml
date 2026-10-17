// JR TRAIN RADIO のページ (1920×1200)
//   上 : 帯域スペクトラム(jr_train.band 240 kS/s、チャネルマーカー付き)
//   中 : チャネルのタイル(同期・通話・EVM・周波数誤差。タップで選択して固定)
//   下 : 選択チャネル — VIEW で切り替え: コンスタレーション / 受信データ(電文)/ 運行把握。右に読み出しと保存の状態
// 受信の可否はフレーム同期(マーカー 2/3)で判断する。搬送波ロックは空きチャネルでも隣接波の漏れで立つことがあるので補助表示。
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
    readonly property var ss: app.saveStatus
    readonly property var viewNames: ({ "const": "CONST", "data": "DATA", "ops": "OPS" })

    // 帯域中心・LO オフセット・プリセット・保存内容は site.conf(jr_train.*)。画面からは変えない
    readonly property var softKeys: [
        { label: "AUTO", action: "auto", active: app.autoSelect },
        { label: "MUTE", action: "mute", active: app.mute },
        { label: "VIEW " + page.viewNames[app.lowerView], action: "view" },
        { label: app.saving ? "STOP SAVE" : "SAVE", action: "save", active: app.saving },
        { label: app.recording ? "STOP REC IQ" : "REC IQ", action: "rec", active: app.recording },
        { label: "ROUTE", action: "route", active: app.routeSelection.length > 0 },
        { label: "DIAG", action: "diag" } ]
    function softKey(action) {
        switch (action) {
        case "auto": app.autoSelect = !app.autoSelect; break
        case "mute": app.mute = !app.mute; break
        case "view": app.lowerView = app.lowerView === "const" ? "data" : (app.lowerView === "data" ? "ops" : "const"); break
        case "save": app.toggleSaving(); break
        case "rec": app.toggleRecording(); break
        case "route": routePicker.open(); break
        case "diag": ui.showDiagnostics(); break
        }
    }
    function tileColor(c) {
        if (c.call) return Theme.amber
        if (c.active) return Theme.green
        if (c.locked) return Theme.cyan
        return Theme.gray
    }
    function stateText(c) {
        if (c.call) return "VOICE"
        if (c.active) return "SYNC"
        if (c.locked) return "LOCK"
        return c.carrier ? "carrier" : "--"
    }
    function num(v) { return v === undefined || v === null ? "-" : String(v) }
    readonly property var markers: {
        var m = []
        for (var i = 0; i < page.chans.length; ++i) {
            var c = page.chans[i]
            m.push({ hz: c.offset, label: "", color: i === app.selectedChannel ? Theme.amber : (c.call ? Theme.amber : (c.active ? Theme.green : Theme.gridMajor)) })
        }
        return m
    }

    // ---- 上: 帯域スペクトラム ----
    SpectrumView {
        id: band
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        height: Math.round(parent.height * 0.30)
        source: app.bandView; centerFreq: app.bandCenterHz; centerEditable: false
        markers: page.markers
        markerHz: app.channelOffsetHz(app.selectedChannel); markerLabel: page.sel.label !== undefined ? page.sel.label : ""
        onAskRef: ui.askRef(app.bandView.dbMax, (v) => { app.bandView.dbMin = v - 90; app.bandView.dbMax = v })
    }

    // ---- 中: チャネルのタイル(2 段)----
    Item {
        id: grid
        anchors.top: band.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: Theme.pad; anchors.rightMargin: Theme.pad
        readonly property int cols: Math.max(1, Math.ceil(page.chans.length / 2))
        readonly property real cw: (width - (cols - 1) * 4) / cols
        height: 2 * 82 + 6
        Repeater {
            model: page.chans
            delegate: Rectangle {
                required property var modelData
                required property int index
                readonly property bool selectedCh: index === app.selectedChannel
                readonly property bool playing: index === app.monitoredChannel && modelData.call
                x: (index % grid.cols) * (grid.cw + 4); y: Math.floor(index / grid.cols) * 82 + 6
                width: grid.cw; height: 76
                color: selectedCh ? "#141414" : Theme.panel
                border.color: selectedCh ? Theme.amber : Theme.line; border.width: selectedCh ? 2 : 1
                Text { x: 8; y: 3; text: (playing ? "♪ " : "") + modelData.label; color: selectedCh ? Theme.amber : Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
                Text { anchors.right: parent.right; anchors.rightMargin: 8; y: 5; text: page.stateText(modelData); color: page.tileColor(modelData)
                       font.family: Theme.mono; font.pixelSize: Theme.fsSmall; font.bold: modelData.active || modelData.call }
                Text { x: 8; y: 26; text: (modelData.freq / 1e6).toFixed(5); color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                Text { x: 8; y: 48
                       text: modelData.carrier ? (modelData.evm.toFixed(0) + "%  " + (modelData.freqErr >= 0 ? "+" : "") + modelData.freqErr.toFixed(0) + "Hz"
                                                 + (modelData.messages > 0 ? "  ✉" + modelData.messages.toFixed(0) : "")) : ""
                       color: modelData.active ? Theme.text : Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                Rectangle { x: 0; y: parent.height - 4; width: parent.width; height: 4; color: page.tileColor(modelData); visible: modelData.active || modelData.locked || modelData.call }
                MouseArea { anchors.fill: parent; onClicked: { Feedback.tap(); app.selectChannelByTouch(index) } }
            }
        }
    }

    // ---- 下: 選択チャネル ----
    Item {
        id: lower
        anchors.top: grid.bottom; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
        anchors.margins: Theme.pad; anchors.topMargin: 6
        readonly property real leftW: Math.round(width * 0.58)

        // 左: VIEW で切り替え
        ConstellationView {
            visible: app.lowerView === "const"
            x: 0; y: 0; height: parent.height; width: lower.leftW
            points: app.constellation
            references: [0.7071, 0.7071, -0.7071, 0.7071, -0.7071, -0.7071, 0.7071, -0.7071]
            title: "CONSTELLATION  " + (page.sel.label !== undefined ? page.sel.label : "") + "  π/4-DQPSK 4800 Bd (differential)"
            info: page.sel.evm !== undefined && page.sel.carrier ? "EVM " + page.sel.evm.toFixed(1) + "%" : ""
        }
        Rectangle {
            visible: app.lowerView === "data"
            x: 0; y: 0; height: parent.height; width: lower.leftW
            color: Theme.bg; border.color: Theme.line; border.width: 1; clip: true
            Text { x: Theme.pad; y: 4; text: "DATA  " + (page.sel.label !== undefined ? page.sel.label : "") + "  (newest first, " + app.messages.length + ")"
                   color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            ListView {
                x: Theme.pad; y: 26; width: parent.width - 2 * Theme.pad; height: parent.height - 30
                clip: true; spacing: 4
                model: app.messages
                delegate: Column {
                    required property var modelData
                    width: ListView.view.width
                    Text { text: modelData.kind + "  " + modelData.identity + (modelData.timestamp.length ? "  " + modelData.timestamp : "") + (modelData.receptions > 1 ? "  ×" + modelData.receptions : "")
                           color: Theme.textBright; font.family: Theme.mono; font.pixelSize: Theme.fsSmall; font.bold: true }
                    Text { width: parent.width; text: modelData.fields; wrapMode: Text.WrapAnywhere; maximumLineCount: 3; elide: Text.ElideRight
                           color: Theme.text; font.pixelSize: Theme.fsSmall }
                }
            }
            Text { anchors.centerIn: parent; visible: app.messages.length === 0; text: "no messages"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
        }
        Rectangle {
            visible: app.lowerView === "ops"
            x: 0; y: 0; height: parent.height; width: lower.leftW
            color: Theme.bg; border.color: Theme.line; border.width: 1; clip: true
            readonly property var op: app.operation
            Text { x: Theme.pad; y: 4
                   text: "OPERATION (運行把握)" + (parent.op.rows !== undefined ? "  received " + parent.op.lastReceived.substring(0, 19).replace("T", " ") + " UTC  ×" + parent.op.receptions : "")
                   color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            Column {
                x: Theme.pad; y: 30; spacing: 6
                visible: parent.op.rows !== undefined
                Row {
                    spacing: 0
                    Repeater {
                        model: [["LINE", 200], ["DELAYED", 120], ["ON LINE", 120], ["3-9", 80], ["10-19", 80], ["20+", 80], ["MAX min", 120], ["TRAIN", 140]]
                        delegate: Text { required property var modelData; width: modelData[1]; text: modelData[0]; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                    }
                }
                Repeater {
                    model: parent.parent.op.rows !== undefined ? parent.parent.op.rows : []
                    delegate: Row {
                        required property var modelData
                        Text { width: 200; text: modelData.line; color: Theme.textBright; font.pixelSize: Theme.fsBase }
                        Text { width: 120; text: page.num(modelData.delayed); color: modelData.delayed > 0 ? Theme.amber : Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
                        Text { width: 120; text: page.num(modelData.onLine); color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
                        Text { width: 80; text: page.num(modelData.class1); color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
                        Text { width: 80; text: page.num(modelData.class2); color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
                        Text { width: 80; text: page.num(modelData.class3); color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
                        Text { width: 120; text: page.num(modelData.maxDelay); color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
                        Text { width: 140; text: modelData.trainNo.length ? modelData.trainNo : "-"; color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
                    }
                }
            }
            Text { anchors.centerIn: parent; visible: parent.op.rows === undefined; text: "no operation table yet (common channel)"; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
        }

        // 右: 読み出し
        Rectangle {
            x: lower.leftW + Theme.pad; y: 0; width: parent.width - x; height: parent.height
            color: Theme.panel; border.color: Theme.line; border.width: 1
            Column {
                x: Theme.pad; y: Theme.pad; spacing: 6
                Row {
                    spacing: 6
                    Rectangle {
                        width: 150; height: 52; color: page.tileColor(page.sel)
                        Text { anchors.centerIn: parent; color: "#000000"; font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true
                               text: page.sel.call ? "VOICE" : (page.sel.active ? "FRAME SYNC" : (page.sel.locked ? "LOCKED" : (page.sel.carrier ? "CARRIER" : "IDLE"))) }
                    }
                    Readout { label: "CHANNEL"; value: page.sel.freq !== undefined ? Theme.fmtFreq(page.sel.freq) : "--"; width: 220; height: 52 }
                    Readout { label: "TYPE"; value: page.sel.service !== undefined ? page.sel.service : "--"; width: 110; height: 52 }
                    Readout { label: "FREQ ERR / EVM"; value: page.sel.carrier ? (page.sel.freqErr >= 0 ? "+" : "") + page.sel.freqErr.toFixed(0) + " Hz / " + page.sel.evm.toFixed(1) + "%" : "--"; width: 210; height: 52 }
                }
                Row {
                    spacing: 6
                    Readout { label: "FRAMES / SYNCED"; value: page.sel.frames !== undefined ? page.sel.frames.toFixed(0) + " / " + page.sel.synced.toFixed(0) : "--"; width: 200; height: 52; valueColor: Theme.green }
                    Readout { label: "VOICE / AUDIO"; value: page.sel.voiceFrames !== undefined ? page.sel.voiceFrames.toFixed(0) + " / " + page.sel.audioFrames.toFixed(0) : "--"; width: 180; height: 52 }
                    Readout { label: "DATA OK / MSGS"; value: page.sel.dataFrames !== undefined ? page.sel.dataFrames.toFixed(0) + " / " + page.sel.messages.toFixed(0) : "--"; width: 180; height: 52 }
                    Readout { label: "EPOCH / RESETS"; value: page.sel.epoch !== undefined ? page.sel.epoch.toFixed(0) + " / " + page.sel.resets.toFixed(0) : "--"; width: 150; height: 52 }
                }
                Row {
                    spacing: 6
                    Readout { label: "AUDIO"; width: 260; height: 52; valueSize: Theme.fsBase
                              value: (app.mute ? "MUTE  " : "") + (app.monitoredChannel >= 0 && app.monitoredChannel < page.chans.length ? page.chans[app.monitoredChannel].label : "--")
                                     + (app.autoSelect ? "  (AUTO)" : "  (FIXED)")
                              valueColor: app.mute ? Theme.textDim : Theme.text }
                    Readout { label: "CALLS"; value: app.activeCalls.length > 0 ? app.activeCalls.map(i => page.chans[i] ? page.chans[i].label : "?").join(" ") : "--"; width: 270; height: 52; valueSize: Theme.fsBase }
                    Readout { label: "SYNC CH"; value: app.activeChannels + " / " + app.numChannels; width: 130; height: 52; valueColor: app.activeChannels > 0 ? Theme.green : Theme.text }
                }
                Row {
                    spacing: 6
                    Readout { label: "PRESET / RX SET"; value: app.presetId.toUpperCase() + "  " + app.receiveSet; width: 300; height: 52; valueSize: Theme.fsBase }
                    Readout { label: "DROPS / GAPS"; value: app.losslessDrops.toFixed(0) + " / " + app.discontinuities.toFixed(0); width: 160; height: 52
                              valueColor: app.losslessDrops > 0 ? Theme.red : Theme.text }
                    Readout { label: "DSP (" + app.dspThreads + " thr)"; value: (app.dspLoad * 100).toFixed(0) + " %"; width: 140; height: 52
                              valueColor: app.dspLoad > 0.8 ? Theme.red : Theme.text }
                    Readout { label: "AUDIO UNDERRUN"; value: app.audioUnderruns.toFixed(0); width: 160; height: 52 }
                }
                Readout { label: "SAVE (decoded: WAV / raw frames / messages)"; width: 740; height: 52; valueSize: Theme.fsSmall
                          value: page.ss.state === "idle" ? "--"
                                 : (page.ss.state.toUpperCase() + "  " + page.ss.files.toFixed(0) + " wav " + page.ss.seconds.toFixed(0) + " s  "
                                    + page.ss.rawFrames.toFixed(0) + " frames  " + page.ss.messages.toFixed(0) + " msgs  " + page.ss.directory)
                          valueColor: app.saving ? Theme.red : (page.ss.error.length ? Theme.red : Theme.text) }
                Readout { label: "REC IQ (radio.rx SigMF)"; width: 740; height: 52; valueSize: Theme.fsSmall
                          value: app.recording ? ("● " + app.recordSeconds.toFixed(1) + " s  " + app.recordPath)
                                               : (app.recordPath.length ? "last " + app.recordSeconds.toFixed(1) + " s  " + app.recordPath : "--")
                          valueColor: app.recording ? Theme.red : Theme.text }
                Text { visible: page.ss.error.length > 0; width: 740; wrapMode: Text.WordWrap; text: "SAVE ERROR  " + page.ss.error; color: Theme.red; font.pixelSize: Theme.fsSmall }
                Text { visible: app.recordError.length > 0; text: "REC ERROR  " + app.recordError; color: Theme.red; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                Text { visible: app.presetError.length > 0; width: 740; wrapMode: Text.WordWrap; text: "PRESET FILE ERROR  " + app.presetError; color: Theme.red; font.pixelSize: Theme.fsSmall }
                Text { visible: app.audioError.length > 0; text: "AUDIO ERROR  " + app.audioError; color: Theme.red; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                Text { visible: page.sel.note !== undefined && page.sel.note.length > 0; width: 740; wrapMode: Text.WordWrap; text: page.sel.note !== undefined ? page.sel.note : ""
                       color: Theme.textDim; font.pixelSize: Theme.fsSmall }
            }
        }
    }

    // ---- 路線から選ぶ(原本の「路線から選ぶ」)。タッチだけで: D 番号で絞り込み、区間をタップで選ぶ ----
    Rectangle {
        id: routePicker
        anchors.fill: parent
        visible: false
        z: 1000
        color: Theme.bg
        property var picked: []          // 選んだ路線 ID
        property bool common: true
        property string filter: ""       // "" = すべて、"D1".."D7"
        readonly property var shown: {
            var out = []
            var rs = app.routes
            for (var i = 0; i < rs.length; ++i)
                if (filter === "" || rs[i].designation === filter) out.push(rs[i])
            return out
        }
        readonly property var preview: app.previewRoutes(picked, common)
        function open() {
            picked = app.routeSelection.length ? app.routeSelection.split(",") : []
            common = app.includeCommon
            filter = ""
            visible = true
        }
        function toggle(id) {
            var p = picked.slice()
            var i = p.indexOf(id)
            if (i >= 0) p.splice(i, 1); else p.push(id)
            picked = p
        }
        MouseArea { anchors.fill: parent }
        Text { x: Theme.pad; y: Theme.pad; text: "ROUTE  " + app.presetLabel + "  (" + app.routes.length + " sections)  — tap sections, then APPLY"
               color: Theme.textBright; font.family: Theme.mono; font.pixelSize: Theme.fsLarge }
        Row {
            id: filters
            x: Theme.pad; y: 56; spacing: 6
            Repeater {
                model: ["", "D1", "D2", "D3", "D4", "D5", "D6", "D7"]
                delegate: Rectangle {
                    required property var modelData
                    width: 110; height: 56
                    color: routePicker.filter === modelData ? Theme.invertBg : Theme.panel; border.color: Theme.line
                    Text { anchors.centerIn: parent; text: modelData === "" ? "ALL" : modelData
                           color: routePicker.filter === modelData ? Theme.invertText : Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsBase; font.bold: true }
                    MouseArea { anchors.fill: parent; onClicked: { Feedback.tap(); routePicker.filter = modelData } }
                }
            }
        }
        ListView {
            id: routeList
            x: Theme.pad; y: filters.y + 70; width: parent.width * 0.62; height: parent.height - y - 100
            clip: true; spacing: 4
            model: routePicker.shown
            delegate: Rectangle {
                required property var modelData
                readonly property bool on: routePicker.picked.indexOf(modelData.id) >= 0
                width: routeList.width; height: 58
                color: on ? "#1a2a1a" : Theme.panel; border.color: on ? Theme.green : Theme.line; border.width: on ? 2 : 1
                Text { x: 12; y: 6; text: (on ? "✔ " : "   ") + modelData.designation + "  " + modelData.line + "  " + modelData.section
                       color: on ? Theme.textBright : Theme.text; font.pixelSize: Theme.fsBase }
                Text { x: 12; y: 32; text: "V " + (modelData.voice1 / 1e6).toFixed(5) + " / " + (modelData.voice2 / 1e6).toFixed(5) + "   DATA " + (modelData.data / 1e6).toFixed(5)
                       + (modelData.note.length ? "   " + modelData.note : "")
                       color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
                MouseArea { anchors.fill: parent; onClicked: { Feedback.tap(); routePicker.toggle(modelData.id) } }
            }
        }
        Column {
            x: routeList.x + routeList.width + Theme.pad; y: routeList.y; width: parent.width - x - Theme.pad; spacing: 8
            Text { text: "SELECTED " + routePicker.picked.length + "  →  " + (routePicker.picked.length ? routePicker.preview.length + " ch" : "ALL channels  " + routePicker.preview.length + " ch")
                   color: Theme.textBright; font.family: Theme.mono; font.pixelSize: Theme.fsBase }
            Text { width: parent.width; wrapMode: Text.WordWrap
                   text: routePicker.preview.map(c => c.label + " " + (c.freq / 1e6).toFixed(5)).join("\n")
                   color: Theme.text; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
            Rectangle {
                width: parent.width; height: 56; color: routePicker.common ? Theme.invertBg : Theme.panel; border.color: Theme.line
                Text { anchors.centerIn: parent; text: "COMMON (旅客一斉情報 / 運行把握) " + (routePicker.common ? "ON" : "OFF")
                       color: routePicker.common ? Theme.invertText : Theme.text; font.pixelSize: Theme.fsBase }
                MouseArea { anchors.fill: parent; onClicked: { Feedback.tap(); routePicker.common = !routePicker.common } }
            }
            Text { width: parent.width; wrapMode: Text.WordWrap; color: Theme.textDim; font.pixelSize: Theme.fsSmall
                   text: "区間の表は原本の収録(出典: odaQshirei 氏のまとめ)。現在の運用を保証しない。同じ周波数の別路線も受信する。APPLY で受信をやり直す(保存中なら、新しいチャネルの組で保存を始め直す)。" }
        }
        Row {
            anchors.bottom: parent.bottom; anchors.bottomMargin: Theme.pad; x: Theme.pad; spacing: 8
            Repeater {
                model: [["APPLY", "apply"], ["CLEAR", "clear"], ["ALL CHANNELS", "all"], ["CANCEL", "cancel"]]
                delegate: Rectangle {
                    required property var modelData
                    width: 260; height: 70; color: Theme.panel; border.color: Theme.line
                    Text { anchors.centerIn: parent; text: modelData[0]; color: Theme.textBright; font.family: Theme.mono; font.pixelSize: Theme.fsLarge }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            Feedback.tap()
                            switch (modelData[1]) {
                            case "apply": app.applyRoutes(routePicker.picked, routePicker.common); routePicker.visible = false; break
                            case "clear": routePicker.picked = []; break
                            case "all": app.applyRoutes([], routePicker.common); routePicker.visible = false; break
                            case "cancel": routePicker.visible = false; break
                            }
                        }
                    }
                }
            }
        }
    }
}

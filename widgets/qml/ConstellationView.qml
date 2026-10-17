// Spear.Widgets — ConstellationView: 複素シンボルの散布図 (要件 §8.3, §12 の Constellation)
// points は [x0, y0, x1, y1, ...](古い順)。値の正規化と基準点の意味は App が決める(references に理想点を渡す)。
// 新しい点ほど明るく描く。点の数は App 側で絞る(数百点まで。毎回 Canvas を描き直す)。
import QtQuick
import Spear.Theme

Item {
    id: root
    property var points: []
    property var references: []           // [x0, y0, ...] 理想点(十字で描く)
    property double range: 1.5            // 軸の ±範囲
    property string title: "CONSTELLATION"
    property string info: ""

    Rectangle { anchors.fill: parent; color: Theme.bg; border.color: Theme.line; border.width: 1 }
    Text { x: Theme.pad; y: 4; text: root.title; color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }
    Text { anchors.right: parent.right; anchors.rightMargin: Theme.pad; y: 4; text: root.info
           color: Theme.textDim; font.family: Theme.mono; font.pixelSize: Theme.fsSmall }

    Canvas {
        id: canvas
        readonly property real side: Math.min(parent.width - 2 * Theme.pad, parent.height - 24 - Theme.pad)
        x: Math.round((parent.width - side) / 2); y: 24
        width: side; height: side
        onPaint: {
            var ctx = getContext("2d")
            var w = width, h = height, r = root.range
            function px(v) { return (v + r) / (2 * r) * w }
            function py(v) { return (r - v) / (2 * r) * h }
            ctx.reset()
            ctx.fillStyle = Theme.bg
            ctx.fillRect(0, 0, w, h)
            // 軸と単位円
            ctx.strokeStyle = Theme.gridMajor
            ctx.lineWidth = 1
            ctx.beginPath(); ctx.moveTo(px(0), 0); ctx.lineTo(px(0), h); ctx.moveTo(0, py(0)); ctx.lineTo(w, py(0)); ctx.stroke()
            ctx.strokeStyle = Theme.grid
            ctx.beginPath(); ctx.arc(px(0), py(0), w / (2 * r), 0, 2 * Math.PI); ctx.stroke()
            // 点(古い → 新しい)
            var p = root.points, n = p.length / 2
            for (var i = 0; i < n; ++i) {
                ctx.globalAlpha = 0.25 + 0.75 * (i + 1) / n
                ctx.fillStyle = Theme.trace
                ctx.fillRect(px(p[2 * i]) - 1.5, py(p[2 * i + 1]) - 1.5, 3, 3)
            }
            ctx.globalAlpha = 1
            // 理想点
            ctx.strokeStyle = Theme.amber
            var q = root.references
            for (var k = 0; k + 1 < q.length; k += 2) {
                var cx = px(q[k]), cy = py(q[k + 1])
                ctx.beginPath(); ctx.moveTo(cx - 7, cy); ctx.lineTo(cx + 7, cy); ctx.moveTo(cx, cy - 7); ctx.lineTo(cx, cy + 7); ctx.stroke()
            }
        }
    }
    onPointsChanged: canvas.requestPaint()
    onReferencesChanged: canvas.requestPaint()
    onRangeChanged: canvas.requestPaint()
}

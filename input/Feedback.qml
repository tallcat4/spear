// Spear.Input — Feedback: 操作音の入口(singleton)。
//
// 規約: アクションが実行された箇所で tap() を呼ぶ(押下開始や MouseArea の存在ではなく「受け付けた」事象に結びつける)。
// 空白のタップ・モーダルの背景・無効キー・ドラッグ/ピンチ/フリックのような連続操作では呼ばない。
// reject() は数値入力が値を拒否した(赤で理由が出た)事象と 1:1。
// ここは信号を出すだけで音は鳴らさない。シェル(gui/qml/Main.qml)が C++ の TapSound に接続する。C++ 非依存なので
// このモジュールは単体で使え、接続が無ければ無音。
pragma Singleton
import QtQuick

QtObject {
    signal tapped()
    signal rejected()
    function tap() { tapped() }
    function reject() { rejected() }
}

# SPECTRUM — 欠陥レポート(2026-09-17、SDK への移植時)

**検証対象**: App SDK の最小形、SpectrumView 部品、Core への retune。

**プラットフォームに追加したもの**
* `appfw::GuiApp`(QObject + spear::App)、`AppInfo`、ビルド時レジストリ、`spear_add_app()`
* `on_start / on_stop` を GUI thread で呼ぶ保証(初版は worker thread で呼んでいて `QObject::startTimer` が失敗した)
* `Spear.Widgets/SpectrumView`(任意の ViewSource に接続できる spectrum + waterfall)
* ページへの `app / ui` 注入(`Loader.setSource` の required property)

**残った課題**
* 周波数以外(gain / bandwidth / antenna)の運転中変更 API が無い(Source::retune のみ)

**Off-air 検証(2026-09-17)**
center 422.000 MHz、特定小電力トランシーバー(422.200 MHz)を送信 → +200 kHz に正しくピーク。
これで tune の絶対精度、テンキー → timed retune の経路、FFT の周波数軸の向き(fftshift / bin→offset の符号)、
RBW 表示(4 Msps / 1024 = 3.9 kHz)が実信号で整合していることを確認。合成トーンでは軸の向きの誤りは検出できないため、
新しい表示部品・DSP 段を足したときは既知周波数の実信号で同様の確認を行うこと。

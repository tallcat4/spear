# IQ RECORDER — 欠陥レポート(2026-09-17、SDK への移植時)

**検証対象**: App 固有 State の所有(録音状態)、Lossless 不変条件の画面化、SigMF sidecar、`configure()` による設定注入。

**プラットフォームに追加したもの**
* `GuiApp::configure(QVariantMap)`(録音先ディレクトリ)
* 録音中は FREQ 系ソフトキーを無効化(`enabled: false`)— 部品側は既存の仕組みで足りた

**残った課題**
* 録音一覧・再生元選択(`Spear.Input` に ListPicker が要る)
* 録音中の drop は画面に出るが、sidecar の discontinuity 数をページに出す経路が無い(SigmfRecorder::meta() を App が読む形で足りるか要検討)
* ディスク残量による自動停止

# State の単一所有(規約)

要件 §4.1 の 3 分類のうち **State**(現在の状態)は、システム内に**所有者を 1 つ**だけ持つ。
2026-09-17 に「Spectrum で周波数を変えても左上に反映されず、App を再起動すると累積した値が現れる」という
不具合が出た。原因は中心周波数が 4 か所(GUI の草案 / Source の宣言 / Radio の実値 / GUI が event から復元した値)に
コピーされ、しかも B210 の retune 要求が前の境界待ちの間に捨てられていたこと。個別修正ではなく規約で防ぐ。

## 規約

1. **宣言(declared)の所有者は `Source`**。`Source::config()` が唯一の現在値。変更は `configure()` / `retune()` の
   API だけを通り、呼んだ時点で即座に `config()` に現れる(スレッド安全)。
2. **適用(applied)は event で報告する**。`Retune` event の `value` が実周波数、`range.begin` が境界 sample index。
   B210 の timed retune も Synthetic / Recording も同じ経路を通る(テストで同一に検証できる)。
3. **要求は失われない**。適用待ちがあっても要求は保持され、最新値が勝つ(合流)。適用された値は必ず報告される。
4. **`state_version()`** は宣言・装置状態が変わるたびに進む。観測側はこれで「変わった」を安価に検知できる。
5. **GUI / App は Core の state をコピーして表示に使わない**。`SystemModel` は `Source::config()` /
   `device_status()` を読むだけで、状態に関わる event(Retune / DeviceState / StartupStage / app start)で
   即時に読み直す(ポーリング周期を待たない)。
6. GUI が編集中の値(App 起動前の RF 宣言など)は **草案(draft)** として名前で区別する(`Shell::draftFreq` 等)。
   草案は App 起動時に `Core::run_app` → `Source::configure` で宣言になる。運転中の値は必ず `sys`(Core)から読む。
7. 新しい状態(例: ゲイン、帯域、アンテナ)を追加するときは、1〜6 を同じ形で満たすこと。
   回帰テスト: `tests/test_state.cpp`(retune 即時反映・event 発行・version 更新)。
8. **再起動をまたぐ保存は所有者を増やさない**。`appfw::SettingsStore`(`~/spear/state.conf`)は所有者の Q_PROPERTY を NOTIFY で写す
   **日誌**であり、読むのは起動時の復元(`setProperty`)の 1 回だけ。App は `persist({...})` で名前を宣言するだけで、保存コードを書かない。
   優先順位は 既定値 < 保存値 < `--set`(site.conf)。運転中の値は今までどおり所有者(App / Core)から読む。
   2026-09-17 に「再起動のたびにスケルチや AGC が初期化される」不具合を、App ごとの保存ではなくこの 1 つの仕組みで直した。

## チェックリスト(レビュー時)
* その値の所有者は誰か。所有者以外が書いていないか。
* GUI に `xxx_display_` のような「event から復元した状態」が増えていないか。
* 要求を `exchange(0)` で取り出して捨てる経路がないか。
* 状態変化 event で `SystemModel` が即時更新されるか。
* 保存したい値は `persist()` で宣言しているか(自前でファイルに書いていないか)。派生値・観測値を宣言していないか。

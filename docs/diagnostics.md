# 診断と USRP セットアップ (要件 §12.1, §17.1, §17.2) — 実装状況

最重要部分。推定ではなく一次ソースで判定し、根拠(evidence)を必ず付ける。

## 起動シーケンス(各段を個別に報告: `startup_stage` / `error` event, value = 段番号)

| 段 | 内容 | 一次ソース | 失敗時 |
|---|---|---|---|
| 0 | 自己診断: UHD version、FW/FPGA image の存在、RLIMIT_RTPRIO / MEMLOCK | `uhd::find_image_path`, `getrlimit` | image 欠落 → FAULT(装置に触る前に分かる) |
| — | 待機: 副作用なしの列挙 + FX3 状態レジスタ | `uhd::usrp::b2xx::probe()`(パッチ #2) | DISCONNECTED / NO_FIRMWARE / STANDBY / FAULT(別 serial・権限・他プロセス占有・FX3 error) |
| 1 | device 列挙 → serial 確認 | `uhd::device::find`(FW 書き込みの副作用あり。存在確認には使わない) | DISCONNECTED |
| 2 | FW/FPGA version、FX3 running 確認、USB link 帯域照合 | tree `fw_version` `fpga_version` `fx3_state_code` `link_max_rate` | version 不一致 → Warning / 帯域 > 95 % → FAULT |
| 3 | clock source → ref_locked | `get_mboard_sensor("ref_locked")`(external/gpsdo のみ必須。B2xx では internal で unlocked が正常) | FAULT |
| 4 | tune: 宣言を能力範囲と照合 → 設定 → coercion 検出 → lo_locked | `get_rx_freq_range/gain_range/rates/antennas`、設定後の `get_rx_*` と要求の差、`get_rx_sensor("lo_locked")` | FAULT(§8.1: App 起動失敗) |
| 5 | stream 開始、settling、時刻基準の記録 | `get_time_now()` 往復中点 ↔ host monotonic ↔ UTC | — |
| 6 | 定常監視 | 下記 | — |

## 定常監視(受信中)

| 項目 | 一次ソース | 周期 | 備考 |
|---|---|---|---|
| overflow / out_of_sequence / timeout / late / broken_chain / alignment / bad_packet | `rx_metadata_t::error_code`, `out_of_sequence` | block 毎 | 区別したまま flag + event + カウンタ |
| sample 欠落 | `time_spec` の tick 差分と sample_index の不連続 | block 毎 | UHD が overflow を報告しなくても検出 |
| FX3 状態レジスタ | tree `fx3_state_code`(EP0) | **recv timeout 時のみ** | 受信中に読むとバルク転送が ~13 ms 止まる(実測、毎回 overflow)。定期読みは禁止 |
| LO ロック / AD9361 温度 / RSSI / hw time | `get_rx_sensor("lo_locked"/"temp"/"rssi")`, `get_time_now()`(EP4) | 1 s | 受信を撹乱しないことを実測済み。LO unlock は `lo_unlock` event |
| クロックドリフト | 時刻基準からの hw vs host monotonic | 1 s | 不確かさ(往復遅延/経過時間)付き。実測 −6.0 ± 1.7 ppm @ 90 s |
| host: CPU MHz / governor / package 温度 / throttle / AC / USB autosuspend / disk / RSS / MemAvailable / rlimit | sysfs, /proc, getrlimit | 1 s | `HealthMonitor` |
| UHD 自身のログ | `uhd::log::add_logger` | 随時 | `uhd_log` event。`Loading FPGA image` / `FPGA load: NN%` → STANDBY、`Loading firmware image` → NO_FIRMWARE |
| USB 消失 | `recv()` の `usb_error` / watchdog(連続 timeout) | — | LOST → 後始末(パッチ #1 が前提)→ probe へ |

## 永続化
* `EventLogFile`: 全 event を JSON lines で追記、8 MB × 3 世代ローテーション(`spear-soak --event-log`)
* 録音の sidecar: events / tuning / discontinuities / `time_references`(generation ごとの hw↔mono↔UTC と往復時間)

## 未実装・今後
* TX 側(`recv_async_msg`: underflow / seq_error / time_error)— TX App と同時に
* libudev による USB add/remove の監視(kernel 側の一次ソース。UHD より先に切断を知れる)— 任意
* rtprio: `/etc/security/limits.d` 未設定のため SCHED_FIFO 無効(stage 0 で報告される)
* FPGA 進捗の STANDBY 表示は次回の抜き差しで確認

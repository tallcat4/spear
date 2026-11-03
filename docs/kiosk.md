# キオスクセッションと OS の調整(要件 §11 の段階 2、§11.1)

Plasma は残したまま、デスクトップのアイコンをタップすると S.P.E.A.R. だけのセッションに切り替わり、EXIT で Plasma に戻る
(SteamOS のゲームモードとデスクトップモードの往復と同じ形)。2026-11-03 に、バッテリー駆動での取りこぼしの調査から前倒しで入れた。

## 仕組み

```
Plasma のデスクトップのアイコン
  → sudo -n spear-session-select spear --restart     次の自動ログイン先を spear にして SDDM を再起動
  → SDDM が自動ログイン: wayland セッション spear.desktop → /usr/local/bin/spear-session
      1. spear-session-select plasma                  次の自動ログイン先を先に Plasma に戻す(spear が落ちても Plasma に戻れる)
      2. cage -s -- <repo>/spear.sh                   キオスク用コンポジタの上で spear-gui だけを全画面・等倍(1920×1200 を 1:1)
      3. spear-session-select plasma --restart        EXIT(または異常終了)で SDDM を再起動 → Plasma に自動ログイン
```

- 自動ログインの利用者は既存の `/etc/sddm.conf.d/autologin.conf` のまま。`spear-session-select` は drop-in
  `/etc/sddm.conf.d/zz-spear-autologin.conf` の `Session=` だけを書く。
- 切り替えは SDDM の再起動で行う。再起動直後の SDDM は必ず自動ログインするので、設定の読み直しや `Relogin` に頼らない。
  Plasma 側の未保存の状態はログアウトと同じ扱いになる。
- SDDM を再起動する前に、利用者の `graphical-session.target` を止める。Plasma 6 は kwin / plasmashell を利用者の systemd のサービスとして
  動かすので、SDDM の再起動では終わらない。残ると、キオスクの裏で Plasma が動き続け、戻ったときには新しい Plasma が kwin を起動せず
  画面が暗いままになる(2026-11-03 に起きた)。手順は呼び出し元ごと止まるので、systemd の一時ユニット(`systemd-run`)で切り離して行う。
- root が要るのは `spear-session-select` だけで、sudoers でこれだけをパスワードなしで許す。root 所有の `/usr/local/bin` に置く
  (リポジトリの中のスクリプトを sudoers に載せると、利用者が書き換えて root で動かせてしまう)。
- キオスクセッションでは `SPEAR_KIOSK=1` が立ち、EXIT の確認の文言が「デスクトップに戻る」になる。
- キオスクセッションには powerdevil が無いので、画面の消灯やスリープは起きない(装置として意図どおり)。
- キオスクセッションには Plasma の音量の表示も音量キーの処理も無いので、spear が扱う(ステータスバーの `♪` と音量キー、`docs/audio.md`)。
- cage の座席管理は logind + polkit(SDDM のセッション)を使う。`seatd.service` は有効にしない(座席管理が二重になる)。

## 入れ方と外し方

```
sudo pacman -S cage
sudo packaging/kiosk/install.sh           # セッション・sudoers・udev・limits を入れる
scripts/install-desktop.sh                # デスクトップのアイコンを「キオスクへ切り替え」にする(メニューとタスクバーは Plasma の中で起動のまま)
sudo packaging/kiosk/install.sh --remove  # 全部外す(Plasma だけに戻る)
```

戻れなくなったとき: キーボードで Ctrl+Alt+F3 → ログイン → `sudo /usr/local/bin/spear-session-select plasma --restart`。

## OS の調整

| 項目 | 内容 | 場所 |
|---|---|---|
| CPU の性能 | App の実行中だけ power-profiles-daemon に `performance` を hold する(下記) | Core(`core/power_hold.*`) |
| USB autosuspend | B2xx(vendor 2500)だけ `power/control=on` | `/etc/udev/rules.d/70-spear-b2xx.rules` |
| rtprio / memlock | 利用者に `rtprio 99` / `memlock unlimited`。UHD の RX thread が SCHED_FIFO になる(次のログインから) | `/etc/security/limits.d/spear.conf` |
| ファイル索引 | baloo は無効(`balooctl6 disable`、`~/.config/baloofilerc` に残る)。録音ファイルの索引で extractor が落ちて core dump を繰り返し、受信中の CPU を奪っていた(2026-11-03) | 利用者の設定 |

`scripts/tune-power.sh` は M-1 の再現用に残す(governor を全体に固定する。普段は使わない)。

### CPU の性能(PerformanceHold)

FZ-G2 のバッテリー駆動で、spear の受信中の負荷(8.5 ms ごとの短い処理が 8 論理コアに散る)に対して、HWP は EPP が performance でない限り
900 MHz を選び続ける。ハードウェアの上限ではない(負荷ループだけなら同じバッテリー駆動で 3 GHz・37 W 出る)。
power-profiles-daemon 0.30 の balanced はバッテリー時に `balance_power` を使う(2026-09-15 の M-1 の時点では `balance_performance` だった)。

JR TRAIN RADIO、22 波、B210 のライブ受信(雑音のみ)、バッテリー駆動、2026-11-03:

| EPP | クロック | 電力(電池) | DSP(4 スレッド) |
|---|---|---|---|
| 0(performance) | 4.0 GHz | 19.9 W | 12 % |
| 32 | 3.6 GHz | 18.7 W | 17 % |
| 64 | 3.6 GHz | 19.8 W | 19 % |
| 96 | 1.3 GHz | 11.5 W | 42 % |
| 128(balance_performance) | 0.9 GHz | 10.2 W | 54 % |
| balance_power | 0.9 GHz | 10 W | 57 % |

電力を抑えられる中間の値は無い。そこで Core が App の実行中(`run_app` 〜 `stop_app`)だけ ppd に `HoldProfile("performance")` を出す。
- hold は D-Bus の接続に結びつくので、spear が落ちても ppd が外す。待機中(App なし)は元の省電力のまま。
- polkit の `hold-profile` は `allow_active`: アクティブなセッション(Plasma でもキオスクでも)からは許可され、ssh や端末の外からは拒否される。
  拒否・ppd なしは Warning event を 1 回出して続ける(受信は止めない)。
- ppd は誰かが `SetProfile` すると全 hold を外す(Plasma の電源設定が AC とバッテリーの切り替えで呼ぶ)。HealthMonitor が 1 s ごとに EPP を読み、
  要求中なのに performance でなければ掛け直して event を出す(最短 10 s 間隔)。
- DIAGNOSTICS の HOST に `EPP` の行がある(`performance  HOLD`。要求中に performance でなければ橙)。
- 切るには `--set power.performance_hold=0`(site.conf に書ける)。
- 受信中の電力は約 10 W → 約 20 W になる(バッテリーでの受信時間はおよそ半分)。

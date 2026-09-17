# パッチ済み libuhd

UHD 4.9 は USB device 消失後の最初の送信で、shared_ptr の deleter(noexcept)から `uhd::usb_error` を
投げて `std::terminate` する(`libusb1_zero_copy.cpp` の `release()` → `enqueue_buffer()`)。
device object の破棄すらできず、`uhd::device::make` は死んだ object をキャッシュから返し続けるため、
アプリ側の工夫だけでは再接続できない(2026-09-15 実機で確認、`docs/m1/RESULTS.md`)。

`0001-usb-do-not-throw-from-buffer-release.patch` はその例外を握りつぶして transport を
`STATUS_ERROR` にする。以後の操作は timeout になり、UHD の destructor は `UHD_SAFE_CALL` で
それを処理して正常に破棄できる。

`0002-b2xx-public-probe-and-fx3-state.patch` は装置状態の一次ソースを公開する:
`uhd/usrp/b2xx_probe.hpp` の `uhd::usrp::b2xx::probe()`(副作用なしの列挙 + FX3 状態レジスタ)と、
open 中の device 用 property `/mboards/0/fx3_state` / `fx3_state_code`。

upstream には送らない(このプロジェクト固有の運用とする)。

## 2 つのプロファイル

* **デフォルト(フル構成 + パッチ)**: 共用の開発機はこちら。公式と同じ device 対応・C API・RFNoC を持ち、
  `libuhd.so` のシンボル集合は公式 + 追加分なので、gnuradio / soapyuhd / python-uhd / sdrangel 等の
  他アプリを壊さない(2026-09-15 に実機で確認)。
* **`SPEAR_B200_ONLY=1 makepkg -s`**: B200 以外の全 device / MPMD / DPDK / C API を無効化した Appliance 用。
  `libuhd.so` 13 MB → 5.3 MB、共有ライブラリ依存 45 → 14。**共用機には入れない**(B2xx 以外の USRP や
  C API を使う他アプリが動かなくなる)。

## ビルドとインストール

```
cd packaging/libuhd
makepkg -s            # 20〜40 分 (i5-10310U)
sudo pacman -U libuhd-4.9.0.1-8.2-x86_64.pkg.tar.zst libuhd-utils-4.9.0.1-8.2-x86_64.pkg.tar.zst
```

`/etc/pacman.conf` に `IgnorePkg = libuhd libuhd-utils` を追加し、公式更新で上書きされないようにする。
公式の libuhd が更新されたら PKGBUILD の `pkgver`/`sha256sums` を追従させて再ビルドする。
`python-uhd` は公式パッケージのまま(同一ソース・同一 ABI)。

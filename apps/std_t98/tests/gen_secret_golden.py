#!/usr/bin/env python3
"""秘話(鍵探索)の golden 生成 — std-t98-tools の Python 実装(torch)を真値にする。

std-t98-tools の venv(torch)で実行。モデルはこのリポジトリの apps/std_t98/models(C++ に埋め込まれるものと同じファイル)を読む:
  ../std-t98-tools/env/bin/python apps/std_t98/tests/gen_secret_golden.py [--tools ../std-t98-tools] [--golden ~/spear/golden/std_t98]
入力: <golden>/ch3_payloads.txt(実録音の TCH ペイロード "ch sym hex36"、spear-std-t98-decode --payloads で生成、音声を含むのでリポジトリ外)
出力: <golden>/secret_golden.txt(音声由来なのでリポジトリ外。無ければ C++ テストは skip)
  ffnn   <980bit hex> <logit_plain> <logit_enc>      torch の ffnn 出力(乱数ビット + 実ペイロード由来)
  hybrid <980bit hex> <logit_plain> <logit_enc>      torch の hybrid 出力
  crack  <key> <bursts> <current_key> <resolved> <source>   実ペイロードを key でスクランブルし SecretCracker.resolve_key した結果
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np


def bits_to_hex(bits: np.ndarray) -> str:
    return np.packbits(bits.astype(np.uint8)).tobytes().hex()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tools", default=str(Path(__file__).resolve().parents[4] / "std-t98-tools"))
    ap.add_argument("--golden", default=str(Path.home() / "spear" / "golden" / "std_t98"))
    args = ap.parse_args()
    tools = Path(args.tools).resolve()
    sys.path.insert(0, str(tools))
    import torch  # noqa: E402
    import pyambelib  # noqa: E402
    from core.audio.ambe_adapter import fec_demod_to_2450_payload  # noqa: E402
    from core.crypto.pn_sequence import generate_pn_sequence_196  # noqa: E402
    from core.crypto.secret_voice import descramble_burst  # noqa: E402
    from core.secret.cracker import (  # noqa: E402
        SecretCracker, _burst_payload_to_raw_bits, INPUT_DIM, FRAMES_PER_BLOCK, BURSTS_PER_BLOCK,
    )
    del pyambelib

    golden = Path(args.golden).expanduser()
    payload_lines = [l.split() for l in (golden / "ch3_payloads.txt").read_text().splitlines() if l.strip()]
    bursts_3600 = [bytes.fromhex(l[2]) for l in payload_lines]
    # 3600 → 2450(7 byte × 4)。ffnn/hybrid の入力はこの 49 bit(ThumbDV 順)
    bursts_2450 = [[fec_demod_to_2450_payload(b[k * 9 : (k + 1) * 9]) for k in range(4)] for b in bursts_3600]
    assert len(bursts_2450) >= 2 * BURSTS_PER_BLOCK, "need at least 10 bursts"

    models = Path(__file__).resolve().parents[1] / "models"
    cracker = SecretCracker(models / "ambe2_ffnn.safetensors", models / "ambe2_hybrid.safetensors")
    rng = np.random.default_rng(7)
    out = []

    def scramble(bursts, key):
        pn = generate_pn_sequence_196(key)
        return [descramble_burst(b, pn) for b in bursts]   # XOR なので scramble == descramble

    def block_bits(bursts):   # 5 バースト → 980 bit(バースト順・フレーム順、ThumbDV 順)
        return np.vstack([_burst_payload_to_raw_bits(b"".join(b)) for b in bursts[:BURSTS_PER_BLOCK]]).reshape(-1)

    # ---- モデル出力(乱数 32 + 実ペイロード由来 32: 平文 / 鍵つき) ----
    inputs = [rng.integers(0, 2, size=INPUT_DIM).astype(np.uint8) for _ in range(32)]
    for i in range(32):
        key = int(rng.integers(0, 32768)) if i % 4 else 0
        start = int(rng.integers(0, len(bursts_2450) - BURSTS_PER_BLOCK + 1))
        b = bursts_2450[start : start + BURSTS_PER_BLOCK]
        inputs.append(block_bits(scramble(b, key) if key else b))
    with torch.no_grad():
        for x in inputs:
            xt = torch.from_numpy(x.astype(np.float32)).unsqueeze(0)
            lf = cracker.ffnn_model(xt)[0].tolist()
            lh = cracker.hybrid_model(xt)[0].tolist()
            out.append(f"ffnn {bits_to_hex(x)} {lf[0]:.7g} {lf[1]:.7g}")
            out.append(f"hybrid {bits_to_hex(x)} {lh[0]:.7g} {lh[1]:.7g}")

    # ---- 鍵探索 E2E(実音声を既知鍵でスクランブル → resolve_key) ----
    cases = [(1, 5, 0), (12345, 5, 0), (32767, 10, 0), (20000, 10, 0), (777, 10, 777), (777, 5, 12345), (0, 10, 0)]
    for key, n, current in cases:
        cracker.global_cache = []
        b = bursts_2450[:n]
        if key:
            b = scramble(b, key)
        payload = b"".join(b"".join(x) for x in b)
        t0 = time.time()
        r = cracker.resolve_key(current, payload, n)
        dt = time.time() - t0
        out.append(f"crack {key} {n} {current} {r.resolved_key} {r.result_source}")
        print(f"key={key:5d} bursts={n:2d} current={current:5d} -> resolved={r.resolved_key:5d} source={r.result_source} ({dt:.2f} s)")
    (golden / "secret_golden.txt").write_text(
        "# secret golden (std-t98-tools torch). ffnn/hybrid: <980bit hex> <logit_plain> <logit_enc>; crack: <key> <bursts> <current_key> <resolved> <source>\n"
        + "\n".join(out) + "\n")
    print(f"wrote {golden / 'secret_golden.txt'} ({len(out)} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

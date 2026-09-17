#!/usr/bin/env python3
"""ambe2_hybrid(safetensors)→ ONNX 変換 + 等価性確認(要件 §3.4 道 2)。再学習したときだけ実行する。

std-t98-tools の venv(torch + safetensors + onnx + onnxruntime)で実行する:
  ../std-t98-tools/env/bin/python apps/std_t98/tools/export_secret_onnx.py [--tools ../std-t98-tools] [--out apps/std_t98/models]
入力: <tools>/models/secret_voice/ の safetensors(+ .json)。<out> にコピーした上で、hybrid を <out>/ambe2_hybrid.onnx に変換する
(入力 "input" [N, 980] float32、出力 "logits" [N, 2]、N は動的)。ffnn は C++ が safetensors を直接読むので変換しない。
<out> の中身は CMake がバイナリに埋め込む(secret/models.cpp)。
確認: 乱数ビット列で torch と onnxruntime の logits を比較し、最大差と argmax 一致率を表示する(差 ≥ 1e-3 か不一致があれば非 0 で終わる)。
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import numpy as np


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tools", default=str(Path(__file__).resolve().parents[4] / "std-t98-tools"), help="std-t98-tools のパス")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parents[1] / "models"))
    ap.add_argument("--opset", type=int, default=17)
    ap.add_argument("--samples", type=int, default=512)
    args = ap.parse_args()

    tools = Path(args.tools).resolve()
    sys.path.insert(0, str(tools))
    import torch  # noqa: E402
    from core.secret.cracker import _load_checkpoint, INPUT_DIM  # noqa: E402

    models = tools / "models" / "secret_voice"
    out = Path(args.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    for name in ("ambe2_ffnn.safetensors", "ambe2_ffnn.json", "ambe2_hybrid.safetensors", "ambe2_hybrid.json"):
        shutil.copy2(models / name, out / name)

    device = torch.device("cpu")
    hybrid, meta = _load_checkpoint(models / "ambe2_hybrid.safetensors", device)
    hybrid.eval()
    onnx_path = out / "ambe2_hybrid.onnx"
    dummy = torch.zeros(2, INPUT_DIM, dtype=torch.float32)
    with torch.no_grad():
        torch.onnx.export(
            hybrid, (dummy,), str(onnx_path),
            input_names=["input"], output_names=["logits"],
            dynamic_axes={"input": {0: "batch"}, "logits": {0: "batch"}},
            opset_version=args.opset, dynamo=False, do_constant_folding=True,
        )
    print(f"wrote {onnx_path} ({onnx_path.stat().st_size} bytes), meta={meta}")

    # ---- 等価性確認 ----
    import onnxruntime as ort  # noqa: E402
    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    rng = np.random.default_rng(1)
    x = rng.integers(0, 2, size=(args.samples, INPUT_DIM)).astype(np.float32)
    with torch.no_grad():
        ref = hybrid(torch.from_numpy(x)).numpy()
    got = sess.run(["logits"], {"input": x})[0]
    diff = np.abs(ref - got).max()
    agree = float((ref.argmax(1) == got.argmax(1)).mean())
    print(f"hybrid torch vs onnxruntime: max |Δlogit| = {diff:.3e}, argmax agreement = {agree:.4f} on {args.samples} random inputs")
    # バッチ 1 でも同じ(動的軸の確認)
    got1 = np.vstack([sess.run(["logits"], {"input": x[i : i + 1]})[0] for i in range(8)])
    print(f"batch-1 vs batch-N max diff = {np.abs(got1 - got[:8]).max():.3e}")
    return 0 if diff < 1e-3 and agree == 1.0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

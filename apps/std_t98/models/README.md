# STD-T98 秘話の学習済みモデル

`../std-t98-tools/models/secret_voice/` と同じもの(作者本人が学習、ライセンスは本体と同じ GPL-3.0-or-later)。
バイナリに埋め込まれる(`secret/models.cpp` の `.incbin`)ので実行時のパスは無い。

| ファイル | 内容 | 用途 |
|---|---|---|
| `ambe2_ffnn.safetensors` / `.json` | `AMBE2Classifier`(Linear 980→128, ReLU, Linear 128→2)、val acc 0.9967 | 全 32767 鍵のふるい分け(C++ 推論 `secret/ffnn.*`、safetensors を直接読む) |
| `ambe2_hybrid.safetensors` / `.json` | `AMBE2HybridClassifier`(Conv1d×3 + BN + GELU、BiLSTM×2、attention pooling)、val acc 1.0 | 学習の原本(実行時には使わない) |
| `ambe2_hybrid.onnx` | 上を opset 17 で変換したもの(入力 `input` [N, 980] float32、出力 `logits` [N, 2]) | 「そもそも平文か」判定と候補の採点(ONNX Runtime、`secret/hybrid.*`) |

入力は AMBE 2450 の 49 bit × 20 フレーム(5 TCH フレーム分)を ThumbDV 順に並べた 0/1、出力は logits[0] = 平文、[1] = 暗号。

## 更新するとき(再学習したときだけ)
```
../std-t98-tools/env/bin/python apps/std_t98/tools/export_secret_onnx.py     # safetensors をコピーし、hybrid を ONNX に変換して torch と比較
../std-t98-tools/env/bin/python apps/std_t98/tests/gen_secret_golden.py      # テスト用の golden(torch が真値)を ~/spear/golden/std_t98 に
```
変換時の等価性: 512 個の乱数入力で torch との logits 最大差 5.7e-6、argmax 一致 100%(2026-09-17)。

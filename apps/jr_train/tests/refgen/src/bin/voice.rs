//! JR TRAIN RADIO の音声合成の参照ベクトル生成器(開発用)。
//!
//! 原本 jr_conventional_voice(v1.3.0)の receiver::Voice に決定的なペイロード列を与え、出力をすべて書き出す。
//! C++ 移植(apps/jr_train/voice)は同じ列で同じ PCM(ビット一致)を出すことをテストする。
//!
//! 作り直し(spear と原本を並べて置いた状態で):
//!   cd apps/jr_train/tests/refgen
//!   CARGO_TARGET_DIR=/tmp/jr-refgen cargo run --release --bin voice -- ../vectors/voice
//!
//! platform に生成した OS-アーキテクチャ(例 macos-aarch64)を書く。C++ のテストはこれが一致するときだけビット一致を要求する。
//! 各シナリオ <name> について:
//!   <name>.events  1 行 1 操作: "push <index> <40 桁 hex | ->"(- は欠落スロット)、最後に "flush"
//!   <name>.out     出力の列: [u32 操作番号][u32 index][320 × i16](すべて LE)。操作番号は、その出力を読んだ直前の操作(flush は events の行数 − 1)
use jr_conventional_voice::receiver::Voice;
use std::fmt::Write as _;
use std::fs;
use std::path::Path;

type Event = (u32, Option<[u8; 20]>);

/// tests/synthesis_stream.rs の payload(LCG)
fn lcg_payload(index: u32) -> [u8; 20] {
    let mut state = index as u64 + 1;
    let mut result = std::array::from_fn(|_| {
        state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
        (state >> 32) as u8
    });
    result[19] &= 63;
    result
}

/// tests/planner.rs の payload
fn seed_payload(seed: u8) -> [u8; 20] {
    let mut bytes: [u8; 20] = std::array::from_fn(|i| seed.wrapping_mul(29).wrapping_add((i as u8).wrapping_mul(17)) ^ 0x5a);
    bytes[19] &= 0x3f;
    bytes
}

/// ゆっくり変わるペイロード(連続した音声に近い: 少数のビットだけがフレームごとに変わる)
fn drifting(count: u32, seed: u64) -> Vec<[u8; 20]> {
    let mut state = seed;
    let mut current = lcg_payload(seed as u32);
    let mut out = Vec::new();
    for _ in 0..count {
        for _ in 0..12 {
            state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            let bit = ((state >> 33) % 158) as usize;
            current[bit / 8] ^= 1 << (bit % 8);
        }
        current[19] &= 0x3f;
        out.push(current);
    }
    out
}

fn scenarios() -> Vec<(&'static str, Vec<Event>)> {
    let mut s = Vec::new();
    // 1. 連続 40 フレーム
    s.push(("continuous", (0..40).map(|i| (i, Some(lcg_payload(i)))).collect()));
    // 2. ゆっくり変わる 60 フレーム(index は 1000 から)
    s.push(("drifting", drifting(60, 7).into_iter().enumerate().map(|(i, p)| (1000 + i as u32, Some(p))).collect()));
    // 3. 欠落スロットと index の飛び
    let mut gaps = Vec::new();
    let mut index = 0u32;
    for run in 0..10u32 {
        for _ in 0..(1 + (run * 5) % 13) {
            gaps.push((index, Some(lcg_payload(index ^ 0x55))));
            index += 1;
        }
        match run % 3 {
            0 => gaps.push((index, None)),
            1 => index += 3,
            _ => {
                gaps.push((index, None));
                index += 2;
                gaps.push((index, None));
            }
        }
        index += 1;
    }
    s.push(("gaps", gaps));
    // 4. 原本 tests/planner.rs の欠落パターン
    s.push((
        "missing",
        vec![
            (0, Some(seed_payload(0))),
            (1, Some(seed_payload(1))),
            (2, None),
            (5, None),
            (6, Some(seed_payload(2))),
            (7, Some(seed_payload(3))),
            (8, None),
            (9, None),
            (10, Some(seed_payload(4))),
            (11, None),
        ],
    ));
    // 5. 原本 synthesis_stream.rs の repeated gaps(欠落イベントあり)
    let mut runs = Vec::new();
    let mut index = 0u32;
    for run in 0..24u32 {
        for _ in 0..1 + (run * 7) % 19 {
            runs.push((index, Some(lcg_payload(index))));
            index += 1;
        }
        runs.push((index, None));
        index += 1;
    }
    s.push(("runs", runs));
    // 6. u32 の上端
    s.push((
        "max_index",
        vec![
            (u32::MAX - 12, Some(lcg_payload(1))),
            (u32::MAX - 11, Some(lcg_payload(2))),
            (u32::MAX - 10, Some(lcg_payload(3))),
            (u32::MAX - 9, Some(lcg_payload(4))),
            (u32::MAX - 8, Some(lcg_payload(5))),
            (u32::MAX - 4, Some(lcg_payload(6))),
            (u32::MAX - 2, None),
            (u32::MAX - 1, Some(lcg_payload(7))),
            (u32::MAX, Some(lcg_payload(8))),
        ],
    ));
    // 7. 同じペイロードの繰り返し(定常)とすべて 0
    s.push(("repeated", (0..24).map(|i| (i, Some(seed_payload(9)))).collect()));
    s.push(("zeros", (0..16).map(|i| (i, Some([0u8; 20]))).collect()));
    s
}

fn main() {
    let out = std::env::args().nth(1).expect("usage: voice <output dir>");
    let out = Path::new(&out);
    fs::create_dir_all(out).unwrap();
    // 浮動小数の結果は libm(tanhf / expf / logf …)に依存する。ビット一致は同じ OS・アーキテクチャでだけ比べる
    fs::write(out.join("platform"), format!("{}-{}\n", std::env::consts::OS, std::env::consts::ARCH)).unwrap();
    let mut total = 0;
    for (name, events) in scenarios() {
        let mut voice = Voice::default();
        let mut text = String::new();
        let mut bin = Vec::new();
        let mut outputs = 0;
        let record = |voice: &mut Voice, step: u32, bin: &mut Vec<u8>, outputs: &mut usize| {
            while let Some(frame) = voice.read() {
                bin.extend_from_slice(&step.to_le_bytes());
                bin.extend_from_slice(&frame.index.to_le_bytes());
                for v in frame.pcm {
                    bin.extend_from_slice(&v.to_le_bytes());
                }
                *outputs += 1;
            }
        };
        for (step, &(index, payload)) in events.iter().enumerate() {
            match payload {
                Some(p) => {
                    let hex: String = p.iter().map(|b| format!("{b:02x}")).collect();
                    writeln!(text, "push {index} {hex}").unwrap();
                }
                None => writeln!(text, "push {index} -").unwrap(),
            }
            voice.push(index, payload).unwrap();
            record(&mut voice, step as u32, &mut bin, &mut outputs);
        }
        writeln!(text, "flush").unwrap();
        voice.flush().unwrap();
        record(&mut voice, events.len() as u32, &mut bin, &mut outputs);
        fs::write(out.join(format!("{name}.events")), text).unwrap();
        fs::write(out.join(format!("{name}.out")), &bin).unwrap();
        eprintln!("{name}: {} events, {outputs} outputs", events.len());
        total += bin.len();
    }
    eprintln!("total {total} bytes");
}

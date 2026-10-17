//! JR TRAIN RADIO の参照ベクトル生成器(開発用)。
//!
//! 合成した 240 kS/s の帯域 IQ(複数チャネルの π/4-DQPSK、マーカー入りのフレーム)を作り、
//! 原本の Rust 実装(jr_conventional_dsp と receiver-core の frame.rs)に通した結果を書き出す。
//! C++ 移植(apps/jr_train)は同じ IQ を読んで同じ結果になることをテストする。
//!
//! 使い方: cargo run --release -- <出力ディレクトリ>
use jr_conventional_dsp::receiver::Dsp;
use jr_conventional_dsp::Complex32;
use std::f64::consts::PI;
use std::fmt::Write as _;
use std::fs;
use std::path::Path;

#[allow(dead_code)]
#[path = "../../../../../../jr-conventional-line-radio-receiver/crates/receiver-core/src/frame.rs"]
mod frame;

const RATE: f64 = 240_000.0;
const CENTER: f64 = 352_603_125.0;
const SPS: usize = 50; // 240000 / 4800
const SECONDS: f64 = 1.2;
const BLOCK: usize = 4096;

struct Rng(u32);
impl Rng {
    fn next(&mut self) -> u32 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 17;
        self.0 ^= self.0 << 5;
        self.0
    }
    fn uniform(&mut self) -> f64 {
        self.next() as f64 / 4294967296.0
    }
}

/// 合成するチャネル(東日本の割当から、帯域の両端と中央付近)
struct Tx {
    frequency_hz: f64,
    amplitude: f64,
    carrier_offset_hz: f64,
    echo: f64,           // 1 シンボル遅れの反射の振幅
    timing: usize,       // 開始の sample ずれ(シンボル内の位相)
    fade: Option<(f64, f64)>, // 振幅を 2% にする区間 [s]
    present: bool,
}

fn rrc(count: usize, sps: f64) -> Vec<f64> {
    const A: f64 = 0.2;
    (0..count)
        .map(|i| {
            let t = (i as f64 - (count - 1) as f64 / 2.0) / sps;
            if t == 0.0 {
                1.0 - A + 4.0 * A / PI
            } else if ((4.0 * A * t).abs() - 1.0).abs() < 1e-9 {
                A / 2f64.sqrt() * ((1.0 + 2.0 / PI) * (PI / (4.0 * A)).sin() + (1.0 - 2.0 / PI) * (PI / (4.0 * A)).cos())
            } else {
                ((PI * t * (1.0 - A)).sin() + 4.0 * A * t * (PI * t * (1.0 + A)).cos()) / (PI * t * (1.0 - 16.0 * A * A * t * t))
            }
        })
        .collect()
}

/// 384 bit のフレーム列(bit 0/128/256 にマーカー、他は乱数)を dibit にする
fn frame_dibits(symbols: usize, rng: &mut Rng) -> Vec<u8> {
    let mut bits = Vec::with_capacity(symbols * 2);
    while bits.len() < symbols * 2 {
        let mut f = [0u8; 384];
        for b in f.iter_mut() {
            *b = (rng.next() >> 31) as u8;
        }
        for (m, marker) in [0x3d22u16, 0xe867, 0xe867].iter().enumerate() {
            for i in 0..16 {
                f[128 * m + i] = ((marker >> (15 - i)) & 1) as u8;
            }
        }
        bits.extend_from_slice(&f);
    }
    bits.truncate(symbols * 2);
    bits.chunks(2).map(|p| (p[0] << 1) | p[1]).collect()
}

fn main() {
    let out = std::env::args().nth(1).expect("usage: jr-train-refgen <output dir>");
    let out = Path::new(&out);
    fs::create_dir_all(out).unwrap();

    let txs = [
        Tx { frequency_hz: 352_537_500.0, amplitude: 0.20, carrier_offset_hz: 120.0, echo: 0.0, timing: 0, fade: None, present: true },
        Tx { frequency_hz: 352_600_000.0, amplitude: 0.12, carrier_offset_hz: -250.0, echo: 0.45, timing: 17, fade: None, present: true },
        Tx { frequency_hz: 352_625_000.0, amplitude: 0.25, carrier_offset_hz: 40.0, echo: 0.2, timing: 33, fade: Some((0.55, 0.62)), present: true },
        Tx { frequency_hz: 352_643_750.0, amplitude: 0.0, carrier_offset_hz: 0.0, echo: 0.0, timing: 0, fade: None, present: false },
        Tx { frequency_hz: 352_668_750.0, amplitude: 0.05, carrier_offset_hz: 300.0, echo: 0.0, timing: 8, fade: None, present: true },
    ];
    let samples = (RATE * SECONDS) as usize;
    let pulse = rrc(16 * SPS + 1, SPS as f64);
    let mut band = vec![(0.0f64, 0.0f64); samples];
    let mut rng = Rng(0x8123_4567);
    let mut sent = Vec::new();
    for tx in &txs {
        let symbols = samples / SPS + 32;
        let dibits = frame_dibits(symbols, &mut rng);
        sent.push(dibits.clone());
        if !tx.present {
            continue;
        }
        // π/4-DQPSK: 位相増分 = π/4 + q·π/2、q は Gray(dibit 0,1,3,2 → q 0,1,2,3)
        let mut phase = 0.0f64;
        let mut base = vec![(0.0f64, 0.0f64); symbols * SPS + pulse.len()];
        for (n, &d) in dibits.iter().enumerate() {
            let q = [0usize, 1, 3, 2][d as usize];
            phase += PI / 4.0 + q as f64 * PI / 2.0;
            let (s, c) = phase.sin_cos();
            for (j, &h) in pulse.iter().enumerate() {
                let k = n * SPS + j;
                base[k].0 += c * h;
                base[k].1 += s * h;
            }
        }
        let offset = tx.frequency_hz - CENTER + tx.carrier_offset_hz;
        for i in 0..samples {
            let k = i + tx.timing + pulse.len() / 2;
            let (mut re, mut im) = base[k];
            if tx.echo != 0.0 {
                let e = base[k - SPS];
                re += tx.echo * e.0;
                im += tx.echo * e.1;
            }
            let t = i as f64 / RATE;
            let mut a = tx.amplitude;
            if let Some((s, e)) = tx.fade {
                if t >= s && t < e {
                    a *= 0.02;
                }
            }
            let w = 2.0 * PI * offset * t;
            let (s, c) = w.sin_cos();
            band[i].0 += a * (re * c - im * s);
            band[i].1 += a * (re * s + im * c);
        }
    }
    // 帯域全体の雑音(一様乱数 4 個の和 ≒ ガウス)と sc16 量子化
    let mut iq16 = Vec::with_capacity(samples * 4);
    let mut iq = Vec::with_capacity(samples);
    for v in &band {
        let mut n = [0.0f64; 2];
        for x in n.iter_mut() {
            *x = ((0..4).map(|_| rng.uniform()).sum::<f64>() - 2.0) * 0.004;
        }
        let re = ((v.0 + n[0]) * 32768.0).round().clamp(-32768.0, 32767.0) as i16;
        let im = ((v.1 + n[1]) * 32768.0).round().clamp(-32768.0, 32767.0) as i16;
        iq16.extend_from_slice(&re.to_le_bytes());
        iq16.extend_from_slice(&im.to_le_bytes());
        iq.push(Complex32 { real: re as f32 / 32768.0, imaginary: im as f32 / 32768.0 });
    }
    fs::write(out.join("band_240k.sc16"), &iq16).unwrap();

    let mut meta = String::new();
    writeln!(meta, "# jr_train 参照ベクトル(tests/refgen が生成。手で編集しない)").unwrap();
    writeln!(meta, "rate {RATE}").unwrap();
    writeln!(meta, "center {CENTER}").unwrap();
    writeln!(meta, "samples {samples}").unwrap();
    writeln!(meta, "block {BLOCK}").unwrap();
    for (c, tx) in txs.iter().enumerate() {
        writeln!(meta, "channel {c} {} {}", tx.frequency_hz, u8::from(tx.present)).unwrap();
    }
    fs::write(out.join("meta.txt"), meta).unwrap();
    // 生成した環境(C++ のテストはこれが一致するときだけビット一致を要求する: tests/platform.hpp)
    fs::write(out.join("platform"), format!("{}-{}\n", std::env::consts::OS, std::env::consts::ARCH)).unwrap();

    for (c, tx) in txs.iter().enumerate() {
        let mut dsp = Dsp::new(RATE, tx.frequency_hz - CENTER).unwrap();
        let mut sync = frame::Synchronizer::default();
        let mut dibits = Vec::new();
        let mut frames = Vec::new();
        let mut snap = None;
        let mut buf = Vec::new();
        for chunk in iq.chunks(BLOCK) {
            snap = Some(dsp.process(chunk, &mut buf).unwrap());
            for &d in &buf {
                sync.push(d, &mut frames);
            }
            dibits.extend_from_slice(&buf);
        }
        let snap = snap.unwrap();
        let (points, evm) = dsp.signal();
        fs::write(out.join(format!("ch{c}_dibits.bin")), &dibits).unwrap();
        let mut p = Vec::new();
        for pt in &points {
            p.extend_from_slice(&pt[0].to_le_bytes());
            p.extend_from_slice(&pt[1].to_le_bytes());
        }
        fs::write(out.join(format!("ch{c}_constellation.cf32")), &p).unwrap();
        let mut s = String::new();
        writeln!(s, "symbols {}", snap.symbols).unwrap();
        writeln!(s, "locked_symbols {}", snap.locked_symbols).unwrap();
        writeln!(s, "carrier_error {:?}", snap.carrier_error).unwrap();
        writeln!(s, "frequency_offset_hz {:?}", snap.frequency_offset_hz).unwrap();
        writeln!(s, "carrier_locked {}", snap.carrier_locked).unwrap();
        writeln!(s, "evm {:?}", evm).unwrap();
        writeln!(s, "points {}", points.len()).unwrap();
        writeln!(s, "frames {}", sync.frames).unwrap();
        writeln!(s, "synchronized_frames {}", sync.synchronized_frames).unwrap();
        writeln!(s, "frame_locked {}", u8::from(sync.locked)).unwrap();
        for f in &frames {
            let hex: String = f.received.iter().map(|b| format!("{b:02x}")).collect();
            writeln!(s, "frame {} {} {} {}", f.bit_offset, u8::from(f.sync_matched), f.marker_errors, hex).unwrap();
        }
        fs::write(out.join(format!("ch{c}_result.txt")), s).unwrap();
        eprintln!(
            "ch{c} {:.4} MHz: dibits {} carrier_locked {} evm {:.2}% frames {} synced {} offset {:.1} Hz",
            tx.frequency_hz / 1e6,
            dibits.len(),
            snap.carrier_locked,
            evm,
            sync.frames,
            sync.synchronized_frames,
            snap.frequency_offset_hz
        );
        // 送った dibit と比べた誤り(参考)
        let _ = &sent[c];
    }
}

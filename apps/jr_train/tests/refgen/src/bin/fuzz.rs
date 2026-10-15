//! JR TRAIN RADIO の差分ファジング(開発用)。乱数で作った大量の入力を原本の Rust に通し、入力と結果を書き出す。
//! C++ 側は tests/test_fuzz.cpp(環境変数 JR_TRAIN_FUZZ_DIR があるときだけ走る)が同じ入力で同じ結果になることを確かめる。
//! 符号化の補助は bin/protocol.rs と同じもの(複製)。
//!
//! 使い方(apps/jr_train/tests/refgen で):
//!   CARGO_TARGET_DIR=/tmp/jr-refgen cargo run --release --bin fuzz -- <出力ディレクトリ> [規模(既定 1)]
//!   JR_TRAIN_FUZZ_DIR=<出力ディレクトリ> ./spear_jr_train_tests --gtest_filter='JrTrainFuzz*'
#![allow(dead_code)]
use jr_conventional_dsp::receiver::Dsp;
use jr_conventional_voice::receiver::Voice;
use receiver_core::data::{decode_frame, Decoder, Format};
use receiver_core::frame::Frame;
use receiver_core::Complex32;
use std::f64::consts::PI;
use std::fs;
use std::io::Write as _;
use std::path::Path;

// 原本の非公開モジュール(LH5 と文字列)を直接テストするための取り込み
mod shim {
    pub type Result<T> = std::result::Result<T, &'static str>;
    pub use receiver_core::data::Field;
    #[allow(dead_code)]
    #[path = "../../../../../../../../jr-conventional-line-radio-receiver/crates/receiver-core/src/data/lha.rs"]
    pub mod lha;
    #[allow(dead_code)]
    #[path = "../../../../../../../../jr-conventional-line-radio-receiver/crates/receiver-core/src/data/text.rs"]
    pub mod text;
    pub fn jis(row: u8, cell: u8) -> Result<String> {
        text::jis(row, cell)
    }
}

const COORDINATES: [usize; 188] = include!("../../../../../../../jr-conventional-line-radio-receiver/crates/receiver-core/src/voice_coordinates.in");
const CRC_POS: [usize; 8] = [0, 1, 2, 3, 82, 83, 84, 85];

struct Rng(u32);
impl Rng {
    fn next(&mut self) -> u32 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 17;
        self.0 ^= self.0 << 5;
        self.0
    }
    fn byte(&mut self) -> u8 {
        (self.next() >> 24) as u8
    }
    fn below(&mut self, n: u32) -> u32 {
        self.next() % n
    }
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|v| format!("{v:02x}")).collect()
}

// ---- GF(256)(原本 coding.rs と同じ体)----
struct Gf {
    powers: [u8; 510],
    logs: [usize; 256],
}
impl Gf {
    fn new() -> Self {
        let mut g = Gf { powers: [0; 510], logs: [0; 256] };
        let mut v = 1usize;
        for i in 0..255 {
            g.powers[i] = v as u8;
            g.powers[i + 255] = v as u8;
            g.logs[v] = i;
            v <<= 1;
            if v & 256 != 0 {
                v ^= 0x11d;
            }
        }
        g
    }
    fn mul(&self, a: u8, b: u8) -> u8 {
        if a == 0 || b == 0 {
            0
        } else {
            self.powers[self.logs[a as usize] + self.logs[b as usize]]
        }
    }
}
fn crc16(bytes: &[u8]) -> u16 {
    let mut crc = 0xffffu16;
    for &b in bytes {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            crc = (crc << 1) ^ if crc & 0x8000 != 0 { 0x1021 } else { 0 };
        }
    }
    crc
}
fn sum(b: &[u8]) -> u16 {
    b.iter().fold(0u16, |s, b| s.wrapping_add(*b as u16))
}

/// 42 byte の符号語 → スクランブル + マーカー → 48 byte の無線フレーム(原本 descramble の逆)
fn envelope(word: &[u8; 42]) -> [u8; 48] {
    let mut received = [0u8; 48];
    for (i, marker) in [0x3d22u16, 0xe867, 0xe867].iter().enumerate() {
        received[16 * i..16 * i + 2].copy_from_slice(&marker.to_be_bytes());
    }
    let mut pn = [1u8; 336];
    for i in 0..336 {
        if i >= 9 {
            pn[i] = pn[i - 9] ^ pn[i - 5];
        }
        let pos = 128 * (i / 112) + 16 + i % 112;
        received[pos / 8] |= (((word[i / 8] >> (i % 8)) & 1) ^ pn[i]) << (7 - pos % 8);
    }
    received
}

/// データフレーム: info 28 byte → CRC-16 → RS(42,30)(12 根)→ 無線フレーム(原本 operation.rs のテストと同じ)
fn data_frame(gf: &Gf, info: &[u8; 28]) -> [u8; 48] {
    let mut generator = vec![1u8];
    for root in 0..12 {
        let mut next = vec![0; generator.len() + 1];
        for (i, c) in generator.iter().enumerate() {
            next[i] ^= c;
            next[i + 1] ^= gf.mul(*c, gf.powers[root]);
        }
        generator = next;
    }
    let mut word = [0u8; 42];
    word[..28].copy_from_slice(info);
    let crc = crc16(&word[..28]);
    word[28..30].copy_from_slice(&crc.to_be_bytes());
    let saved = word;
    for j in 0..30 {
        let c = word[j];
        for k in 0..13 {
            word[j + k] ^= gf.mul(c, generator[k]);
        }
    }
    word[..30].copy_from_slice(&saved[..30]);
    let e = envelope(&word);
    assert_eq!(decode_frame(&e).unwrap().0, *info);
    e
}

// ---- 音声フレーム(原本 voice_frame.rs の逆)----
fn parity(v: usize) -> u8 {
    (v.count_ones() & 1) as u8
}
fn bit(data: &[u8], i: usize) -> u8 {
    (data[i / 8] >> (i % 8)) & 1
}
fn set(data: &mut [u8], i: usize, v: u8) {
    data[i / 8] |= v << (i % 8);
}
fn crc8(data: &[u8]) -> u8 {
    let mut state = 255u16;
    for i in 0..86 {
        state = (state << 1) | if i < 78 { data[i] as u16 } else { 0 };
        if state & 256 != 0 {
            state ^= 0x19b;
        }
    }
    state as u8
}
fn voice_frame(gf: &Gf, payload: &[u8; 20], mode_bit: u8) -> [u8; 48] {
    let mut input = [0u8; 94];
    for i in 0..78 {
        input[4 + i] = bit(payload, i);
    }
    let c = crc8(&input[4..]);
    for (i, p) in CRC_POS.iter().enumerate() {
        input[*p] = (c >> i) & 1;
    }
    let mut encoded = [0u8; 176];
    let mut state = 0;
    for i in 0..94 {
        state = ((state << 1) | input[i] as usize) & 511;
        if COORDINATES[2 * i] != 255 {
            encoded[COORDINATES[2 * i]] = parity(state & 0x1af);
        }
        encoded[COORDINATES[2 * i + 1]] = parity(state & 0x11d);
    }
    let mut word = [0u8; 42];
    for i in 0..40 {
        set(&mut word, i, bit(payload, 78 + i));
        set(&mut word[16..], i, bit(payload, 118 + i));
    }
    for i in 0..88 {
        set(&mut word[5..], i, encoded[2 * i]);
        set(&mut word[21..], i, encoded[2 * i + 1]);
    }
    word[32] = 0x80;
    word[33] = mode_bit;
    let crc = crc16(&word[28..34]);
    word[34..36].copy_from_slice(&crc.to_be_bytes());
    let mut work = [0u8; 14];
    work[..8].copy_from_slice(&word[28..36]);
    for i in 0..8 {
        let c = work[i];
        for (j, g) in [1, 63, 1, 218, 32, 227, 38].iter().enumerate() {
            work[i + j] ^= gf.mul(c, *g);
        }
    }
    word[36..].copy_from_slice(&work[8..]);
    let e = envelope(&word);
    let s = receiver_core::voice_frame::decode(&e).expect("encoded voice frame decodes");
    assert_eq!(s.payload, *payload);
    assert_eq!(s.corrected_bits, 0);
    e
}
fn random_payload(rng: &mut Rng) -> [u8; 20] {
    let mut p = [0u8; 20];
    for b in p.iter_mut() {
        *b = rng.byte();
    }
    p[19] &= 0x3f;
    p
}
/// 包絡のマーカー以外(スクランブル区間)の n bit を反転
fn corrupt(rng: &mut Rng, e: &[u8; 48], n: u32) -> [u8; 48] {
    let mut out = *e;
    for _ in 0..n {
        let i = rng.below(336) as usize;
        let pos = 128 * (i / 112) + 16 + i % 112;
        out[pos / 8] ^= 1 << (7 - pos % 8);
    }
    out
}

// ---- パケット → フレーム ----
fn slotted_traffic(s: u8) -> bool {
    (3..=7).contains(&s) || (11..=15).contains(&s)
}
/// パケットを Slotted(トラフィックスロット、ヘッダ info[3]、24 byte/フレーム)または Common(ヘッダ info[0]、27 byte)で分割。
/// (bit_offset, info) を返す。Slotted のスロットは start_slot から順に進める
fn packet_infos(format: Format, bytes: &[u8], first_offset: u64, start_slot: u8) -> Vec<(u64, [u8; 28])> {
    let h = if format == Format::Common { 0 } else { 3 };
    let per = 27 - h;
    let count = bytes.len().div_ceil(per);
    let mut out = Vec::new();
    let mut offset = first_offset;
    let mut slot = start_slot;
    for (i, part) in bytes.chunks(per).enumerate() {
        let mut info = [0u8; 28];
        info[h] = if count == 1 {
            0xc0 | part.len() as u8
        } else if i == 0 {
            0x80 | (count - 1) as u8
        } else if i + 1 == count {
            0x40 | part.len() as u8
        } else {
            (count - i - 1) as u8
        };
        info[h + 1..h + 1 + part.len()].copy_from_slice(part);
        if format == Format::Slotted {
            info[0] = slot << 4;
            info[1] = 0x55;
            info[2] = 0xaa;
        }
        out.push((offset, info));
        // 次のフレーム位置
        let mut distance = 1u64;
        if format == Format::Slotted {
            while !slotted_traffic(((slot as u64 + distance) % 16) as u8) {
                distance += 1;
            }
            slot = ((slot as u64 + distance) % 16) as u8;
        }
        offset += 384 * distance;
    }
    out
}
fn with_len(mut b: Vec<u8>) -> Vec<u8> {
    b[1] = (b.len() - 2) as u8;
    b
}

// ---- データ電文の素材(すべて合成。録音由来のデータは使わない)----
fn presence() -> Vec<u8> {
    let mut b = vec![0x51, 0x10, 0, 0x12, 0x34, 0x05];
    b.extend([0xde; 12]);
    b
}
fn registration() -> Vec<u8> {
    let mut b = vec![5, 0, 0x02, 0x81, 0x01, 0x80 | 7, 0x12, 0x34, 3, 0x45, 0x67, 0x02, 0x11, 0xf2, 0x15, 0x12, 0x34, 0x09, 0x01, 0x23];
    b.extend([0xab, 0xcd, 0x56, 0x78, 0xef]);
    b.extend([0x25, 0x03, 0x14, 0x09, 0x26, 0x41]);
    with_len(b)
}
fn transfer() -> Vec<u8> {
    let mut b = vec![0x15, 0, 4, 0x12, 0x34, 0x05, 0x10, 4, 0x01, 0x02, 0x07, 0xaa, 0xbb, 0, 0];
    b.extend([0x25, 0x12, 0x31, 0x23, 0x59, 0x58, 0x09, 0x77, 0x88]);
    let s = sum(&b[15..]);
    b[13..15].copy_from_slice(&s.to_be_bytes());
    with_len(b)
}
fn notice(jis: &[u8]) -> Vec<u8> {
    let mut b = vec![0u8; 124];
    b[0] = 1;
    b[2] = 0;
    b[3..6].copy_from_slice(&[0x12, 0x34, 0x05]);
    b[8] = 0x0a;
    b[9] = 0x0b;
    b[10] = 0x10;
    b[14..18].copy_from_slice(&[1, 2, 3, 4]);
    b[18..20].copy_from_slice(&[0x00, 0x42]);
    b[29..32].copy_from_slice(&[0x12, 0x34, 5]);
    b[41..47].copy_from_slice(&[0x25, 0x03, 0x14, 0x09, 0x26, 0x00]);
    // 駅名(JIS、全角空白で埋める)
    let from = [0x45, 0x6c, 0x35, 0x7e, 0x21, 0x21, 0x21, 0x21]; // 東京
    let to = [0x42, 0x67, 0x35, 0x5c, 0x21, 0x21, 0x21, 0x21]; // 大宮
    b[97..105].copy_from_slice(&from);
    b[105..108].copy_from_slice(&[0, 1, 2]);
    b[108..116].copy_from_slice(&to);
    b[116..119].copy_from_slice(&[0, 3, 4]);
    b.extend_from_slice(jis);
    b.extend([0, 0]);
    let n = b.len();
    let s = sum(&b[34..n - 2]);
    b[n - 2..].copy_from_slice(&s.to_be_bytes());
    with_len(b)
}
fn directory(count: u8) -> Vec<u8> {
    let mut b = vec![0; 13];
    b[0] = 0x21;
    b[2] = 4;
    b.extend([0x25, 0x03, 0x14, 0x09, 0x26, count]);
    for id in 0..u16::from(count) {
        b.extend((0xa000 + id).to_be_bytes());
    }
    b.extend(sum(&b[13..]).to_be_bytes());
    with_len(b)
}
/// LH5(定数リテラルのブロックを 1 byte ずつ。原本 operation.rs のテストと同じ)
fn archive(data: &[u8]) -> Vec<u8> {
    let mut bits = Vec::new();
    for &byte in data {
        for (v, n) in [(1u16, 16), (0, 5), (0, 5), (0, 9), (byte as u16, 9), (0, 4), (0, 4)] {
            for bit in (0..n).rev() {
                bits.push(((v >> bit) & 1) as u8);
            }
        }
    }
    let mut packed = vec![0; bits.len().div_ceil(8)];
    for (i, bit) in bits.iter().enumerate() {
        packed[i / 8] |= bit << (7 - i % 8);
    }
    let mut a = vec![0u8; 28];
    a[0] = 26;
    a[2..7].copy_from_slice(b"-lh5-");
    a[7..11].copy_from_slice(&(packed.len() as u32).to_le_bytes());
    a[11..15].copy_from_slice(&(data.len() as u32).to_le_bytes());
    a[20] = 1;
    a[21] = 1;
    a[22] = b't';
    let mut crc = 0u16;
    for byte in data {
        crc ^= *byte as u16;
        for _ in 0..8 {
            crc = (crc >> 1) ^ if crc & 1 != 0 { 0xa001 } else { 0 };
        }
    }
    a[23..25].copy_from_slice(&crc.to_le_bytes());
    a[1] = a[2..].iter().fold(0u8, |s, b| s.wrapping_add(*b));
    a.extend(packed);
    a
}
/// 実際のハフマン符号を使う LH5(リテラルの長さ 8 固定の木 + 一致)。原本の復号器の一般経路を通すためのもの
fn archive_huffman(data: &[u8]) -> Vec<u8> {
    // ブロック 1 つ: 記号は 256 リテラル(長さ 8)+ 一致(長さ 3〜5)は使わず、全リテラルを 8 bit で符号化する。
    // codes(19 記号)の木: 記号 10(= 長さ 8)のみ使うので定数木にはできず、2 記号 {0, 10} に長さ 1 を与える
    let mut bits: Vec<u8> = Vec::new();
    let mut push = |v: usize, n: usize, bits: &mut Vec<u8>| {
        for b in (0..n).rev() {
            bits.push(((v >> b) & 1) as u8);
        }
    };
    push(data.len(), 16, &mut bits);
    // lengths(19, 5, 3): count = 11(記号 0..10)、長さ: 記号 0 = 1、記号 1..9 = 0、記号 10 = 1。skip = 3 の後に 2 bit の run
    push(11, 5, &mut bits);
    push(1, 3, &mut bits); // sym0 len 1
    push(0, 3, &mut bits); // sym1 len 0
    push(0, 3, &mut bits); // sym2 len 0
    push(0, 2, &mut bits); // run 0(i = 3 のまま)
    for _ in 3..10 {
        push(0, 3, &mut bits);
    }
    push(1, 3, &mut bits); // sym10 len 1
    // codes 木: sym0 → '0'、sym10 → '1'
    // literals: count = 256、各リテラル長 8 → code 記号 10(= 8 + 2)→ '1'
    push(256, 9, &mut bits);
    for _ in 0..256 {
        push(1, 1, &mut bits);
    }
    // distance(14, 4, MAX): count 0 → 定数
    push(0, 4, &mut bits);
    push(0, 4, &mut bits);
    for &b in data {
        push(b as usize, 8, &mut bits);
    }
    let mut packed = vec![0; bits.len().div_ceil(8)];
    for (i, bit) in bits.iter().enumerate() {
        packed[i / 8] |= bit << (7 - i % 8);
    }
    let mut a = archive(&[0])[..28].to_vec();
    a[7..11].copy_from_slice(&(packed.len() as u32).to_le_bytes());
    a[11..15].copy_from_slice(&(data.len() as u32).to_le_bytes());
    let mut crc = 0u16;
    for byte in data {
        crc ^= *byte as u16;
        for _ in 0..8 {
            crc = (crc >> 1) ^ if crc & 1 != 0 { 0xa001 } else { 0 };
        }
    }
    a[23..25].copy_from_slice(&crc.to_le_bytes());
    a[1] = a[2..].iter().fold(0u8, |s, b| s.wrapping_add(*b));
    a.extend(packed);
    a
}
fn operation_table(seed: u8) -> Vec<u8> {
    let mut bytes = vec![0x87];
    for i in 0..7u8 {
        bytes.extend([7, 0]);
        bytes.extend([0x3b, 0x6e, 0x38, 0x33, 0x23, 0x41 + i, 0x21, 0x21]); // 試験Ａ…Ｇ
        bytes.extend([0x80, 0x80 | (7 + i + seed), 0, 0xf0, 0x12, 3, 0x83, 0xa8, 0x82, 0x81, 0x80]);
    }
    bytes
}
fn operation_body(table: &[u8], huffman: bool) -> Vec<u8> {
    let mut b = vec![1, 0, 1];
    b.extend([0x19, 0x02, 0xaa, 0x7f, 0, 0xff]);
    b.extend(sum(table).to_be_bytes());
    b.extend(if huffman { archive_huffman(table) } else { archive(table) });
    b
}
/// 共用波の多パケット(運行把握 0x22/C9 または旅客案内 0x21/46・C6)
fn common_packets(kind: u8, code: u8, body: &[u8], serial: u16) -> Vec<Vec<u8>> {
    let total = body.len().div_ceil(240) as u8;
    body.chunks(240)
        .enumerate()
        .map(|(i, part)| {
            let mut bytes = vec![kind, (14 + part.len()) as u8, 5, 0, 0, 0, 0x10, 2];
            bytes.extend(serial.to_be_bytes());
            bytes.extend([1, 1, ((i as u8 + 1) << 4) | total, code, 1, 0x21]);
            bytes.extend(part);
            bytes
        })
        .collect()
}
fn service_content() -> Vec<u8> {
    let mut c = vec![0x25, 0x03, 0x14, 0x09, 0x26];
    // Line: 日本語 "東北線" + 区切り + 英語 "Tohoku Line"
    let mut field = |ja: &[[u8; 2]], en: &str, c: &mut Vec<u8>| {
        let mut units: Vec<Vec<u8>> = ja.iter().map(|p| vec![p[0] | 0x80, p[1] | 0x80]).collect();
        if !en.is_empty() {
            units.push(vec![0x2a, 0x6b]);
            for ch in en.chars() {
                units.push(match ch {
                    'ō' => vec![0xaa, 0xea],
                    'Ō' => vec![0xaa, 0xe5],
                    'ū' => vec![0xaa, 0xe8],
                    ' ' => vec![0xaa, 0xe0],
                    _ => vec![0xa9, ch as u8 | 0x80],
                });
            }
        }
        c.push(units.len() as u8);
        for u in units {
            c.extend(u);
        }
    };
    field(&[[0x45, 0x6c], [0x4b, 0x4c], [0x40, 0x7e]], "Tōhoku Line", &mut c); // 東北線
    field(&[[0x45, 0x6c], [0x35, 0x7e]], "Tokyo", &mut c); // 東京
    field(&[[0x40, 0x67], [0x42, 0x66]], "Sendai", &mut c); // 仙台
    field(&[[0x32, 0x3c], [0x24, 0x6a]], "", &mut c); // 下り
    field(&[[0x36, 0x2f], [0x49, 0x77]], "Strong wind", &mut c); // 強風
    field(&[[0x31, 0x3f], [0x45, 0x3e]], "", &mut c); // 運転
    // Description: 半角カナ・ASCII も
    c.push(4);
    c.extend([0x8e, 0xb1, b'A', b'1', 0x8e, 0xdf]);
    c
}
fn service_body(content: &[u8], compressed: bool) -> Vec<u8> {
    let stored = if compressed { archive(content) } else { content.to_vec() };
    let mut body = vec![1, 0x12, 0x34, 0x56, 0x78, 2, 0x11, 0x22, 0x99, 0x99];
    let s = sum(&body).wrapping_add(sum(content));
    body.extend(s.to_be_bytes());
    body.extend(stored);
    body
}

fn frames_for(gf: &Gf, format: Format, packets: &[Vec<u8>], mut offset: u64, slot: u8) -> Vec<(u64, [u8; 48])> {
    let mut out = Vec::new();
    for p in packets {
        let infos = packet_infos(format, p, offset, slot);
        let last = infos.last().unwrap().0;
        for (o, info) in infos {
            out.push((o, data_frame(gf, &info)));
        }
        // 共用波の分割電文(運行把握・旅客案内)は続きの部分が隣のフレームでなければならない(原本: first − last = 384)
        offset = if format == Format::Common { last + 384 } else { last + 384 * 16 };
    }
    out
}

fn fnv(b: &[u8]) -> u64 {
    let mut h = 0xcbf29ce484222325u64;
    for &x in b {
        h ^= x as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    h
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let out = Path::new(&args[1]);
    let scale: usize = args.get(2).map(|s| s.parse().unwrap()).unwrap_or(1);
    fs::create_dir_all(out).unwrap();
    fs::write(out.join("platform"), format!("{}-{}\n", std::env::consts::OS, std::env::consts::ARCH)).unwrap();
    let gf = Gf::new();
    let mut rng = Rng(0x1357_9bdf ^ (scale as u32).wrapping_mul(0x9e37_79b9));

    // ---- 1. 音声フレームとデータフレーム ----
    let mut f = fs::File::create(out.join("frames.txt")).unwrap();
    for n in 0..60_000 * scale {
        let kind = n % 6;
        let e = match kind {
            0 | 1 => {
                let clean = voice_frame(&gf, &random_payload(&mut rng), (n & 1) as u8);
                let k = rng.below(if kind == 0 { 12 } else { 40 });
                corrupt(&mut rng, &clean, k)
            }
            2 | 3 => {
                let mut info = [0u8; 28];
                for b in info.iter_mut() {
                    *b = rng.byte();
                }
                let clean = data_frame(&gf, &info);
                let k = rng.below(if kind == 2 { 8 } else { 20 });
                corrupt(&mut rng, &clean, k)
            }
            4 => {
                // 音声フレームの外側(byte 28–41)だけを壊す / モードを書き換える
                let clean = voice_frame(&gf, &random_payload(&mut rng), 0);
                let mut e = clean;
                for _ in 0..1 + rng.below(6) {
                    let i = 336 - 112 + rng.below(112) as usize;
                    let pos = 128 * (i / 112) + 16 + i % 112;
                    e[pos / 8] ^= 1 << (7 - pos % 8);
                }
                e
            }
            _ => {
                let mut e = [0u8; 48];
                for b in e.iter_mut() {
                    *b = rng.byte();
                }
                e
            }
        };
        let v = receiver_core::voice_frame::decode(&e);
        let d = decode_frame(&e);
        writeln!(
            f,
            "{} {} {} {} {}",
            hex(&e),
            v.as_ref().map_or("-".into(), |s| hex(&s.payload)),
            v.as_ref().map_or("-".into(), |s| s.corrected_bits.to_string()),
            d.as_ref().map_or("-".into(), |(w, _)| hex(w)),
            d.as_ref().map_or("-".into(), |(_, c)| c.to_string())
        )
        .unwrap();
    }

    // ---- 2. LH5・文字列 ----
    let mut f = fs::File::create(out.join("text.txt")).unwrap();
    let bases = [archive(&operation_table(1)), archive_huffman(&operation_table(2)), archive_huffman(&service_content()), archive(&service_content())];
    for n in 0..20_000 * scale {
        let mut a = bases[n % bases.len()].clone();
        match rng.below(4) {
            0 => {
                for _ in 0..1 + rng.below(4) {
                    let i = rng.below(a.len() as u32) as usize;
                    a[i] ^= 1 << rng.below(8);
                }
            }
            1 => {
                let cut = rng.below(a.len() as u32 + 1) as usize;
                a.truncate(cut);
            }
            2 => {
                let i = 27 + rng.below((a.len() - 27) as u32) as usize;
                a[i] = rng.byte();
            }
            _ => {}
        }
        match shim::lha::expand(&a) {
            Ok(o) => writeln!(f, "L {} ok {}", hex(&a), hex(&o)).unwrap(),
            Err(e) => writeln!(f, "L {} err {e}", hex(&a)).unwrap(),
        }
    }
    for _ in 0..20_000 * scale {
        let n = rng.below(48) as usize;
        let bytes: Vec<u8> = (0..n)
            .map(|_| match rng.below(6) {
                0 => 0x21 + rng.below(94) as u8,
                1 => 0xa1 + rng.below(94) as u8,
                2 => rng.byte(),
                3 => [0, 13, 10, 0x21, 0x8e, 0xa9, 0xaa, 0x2a, 0x6b][rng.below(9) as usize],
                4 => 0x20 + rng.below(0x5f) as u8,
                _ => 0x30 + rng.below(0x40) as u8,
            })
            .collect();
        let trim = rng.below(2) == 1;
        writeln!(f, "N {} {} {}", u8::from(trim), hex(&bytes), hex(shim::text::notice(&bytes, trim).as_bytes())).unwrap();
        let mut svc = vec![0x25, 0x03, 0x14, 0x09, 0x26];
        svc.extend(&bytes);
        match shim::text::service(&svc) {
            Ok(fields) => writeln!(f, "S {} ok {}", hex(&svc), serde_json::to_string(&fields).unwrap()).unwrap(),
            Err(e) => writeln!(f, "S {} err {e}", hex(&svc)).unwrap(),
        }
    }
    // 旅客案内の本文を少しずつ壊したもの(正常に読める入力の近く)
    let content = service_content();
    for _ in 0..5_000 * scale {
        let mut c = content.clone();
        let i = 5 + rng.below((c.len() - 5) as u32) as usize;
        c[i] = if rng.below(2) == 0 { rng.byte() } else { c[i] ^ (1 << rng.below(8)) };
        if rng.below(4) == 0 {
            let cut = 5 + rng.below((c.len() - 4) as u32) as usize;
            c.truncate(cut);
        }
        match shim::text::service(&c) {
            Ok(fields) => writeln!(f, "S {} ok {}", hex(&c), serde_json::to_string(&fields).unwrap()).unwrap(),
            Err(e) => writeln!(f, "S {} err {e}", hex(&c)).unwrap(),
        }
    }

    // ---- 3. データ電文の列(パケット組立・電文解析)----
    let mut f = fs::File::create(out.join("data.txt")).unwrap();
    for seq in 0..1_500 * scale {
        let format = if seq % 2 == 0 { Format::Slotted } else { Format::Common };
        let mut packets: Vec<Vec<u8>> = Vec::new();
        for _ in 0..1 + rng.below(4) {
            let jis: Vec<u8> = (0..rng.below(20)).map(|_| 0x21 + rng.below(94) as u8).collect();
            let p = if format == Format::Slotted {
                match rng.below(6) {
                    0 => presence(),
                    1 => registration(),
                    2 => transfer(),
                    3 => notice(&jis),
                    4 => {
                        let n = 2 + rng.below(40) as usize;
                        let mut b: Vec<u8> = (0..n).map(|_| rng.byte()).collect();
                        b[1] = (n - 2) as u8;
                        b
                    }
                    _ => {
                        let mut b = registration();
                        let i = rng.below(b.len() as u32) as usize;
                        b[i] = rng.byte();
                        b[1] = (b.len() - 2) as u8;
                        b
                    }
                }
            } else {
                match rng.below(5) {
                    0 => directory(rng.below(20) as u8),
                    1 => {
                        let mut v = common_packets(0x21, 0x46, &service_body(&service_content(), false), rng.below(100) as u16);
                        packets.extend(v.drain(..v.len() - 1));
                        v.pop().unwrap()
                    }
                    2 => {
                        let mut v = common_packets(0x22, 0xc9, &operation_body(&operation_table(rng.below(5) as u8), rng.below(2) == 0), rng.below(100) as u16);
                        packets.extend(v.drain(..v.len() - 1));
                        v.pop().unwrap()
                    }
                    3 => {
                        let mut b = directory(3);
                        let i = 13 + rng.below((b.len() - 13) as u32) as usize;
                        b[i] = rng.byte();
                        b
                    }
                    _ => vec![0x23, 2, rng.byte(), rng.byte()],
                }
            };
            packets.push(p);
        }
        let start_slot = [3u8, 4, 5, 11, 15][rng.below(5) as usize];
        let mut frames = frames_for(&gf, format, &packets, 384 * rng.below(50) as u64, start_slot);
        // 単独フレーム(チャネル状況・呼設定)を混ぜる
        if format == Format::Slotted && rng.below(2) == 0 {
            let mut info = [0u8; 28];
            for b in info.iter_mut() {
                *b = rng.byte();
            }
            info[0] = if rng.below(2) == 0 { 0x80 } else { 0xa0 };
            info[1] = 0x20;
            info[8] = 0x20;
            info[9] = 3;
            let at = rng.below(frames.len() as u32 + 1) as usize;
            let off = if at < frames.len() { frames[at].0 } else { frames.last().map_or(0, |x| x.0 + 384) };
            frames.insert(at, (off, data_frame(&gf, &info)));
        }
        for fr in frames.iter_mut() {
            match rng.below(10) {
                0 => {
                    let k = rng.below(10);
                    fr.1 = corrupt(&mut rng, &fr.1, k)
                }
                1 => fr.1 = corrupt(&mut rng, &fr.1, 3),
                2 => fr.0 += 384,
                _ => {}
            }
        }
        writeln!(f, "C {}", if format == Format::Common { "common" } else { "slotted" }).unwrap();
        let mut d = Decoder::new(format);
        for (i, (off, e)) in frames.iter().enumerate() {
            writeln!(f, "F {off} {}", hex(e)).unwrap();
            for m in d.decode(&Frame { bit_offset: *off, sync_matched: true, marker_errors: 0, received: *e }) {
                writeln!(f, "M {i} {}", serde_json::to_string(&m).unwrap()).unwrap();
            }
        }
        let s = &d.stats;
        writeln!(f, "E {} {} {} {}", s.valid_frames, s.corrected_frames, s.rejected_frames, s.rejected_messages).unwrap();
    }

    // ---- 4. 音声合成(乱数のペイロード・欠落・index の飛び・flush)----
    let mut f = fs::File::create(out.join("voice.txt")).unwrap();
    for _ in 0..30 * scale {
        writeln!(f, "B").unwrap();
        let mut v = Voice::default();
        let mut index = rng.below(1000);
        for _ in 0..80 + rng.below(160) {
            index += 1 + if rng.below(8) == 0 { rng.below(12) } else { 0 };
            let payload = if rng.below(6) == 0 { None } else { Some(random_payload(&mut rng)) };
            let r = v.push(index, payload);
            writeln!(f, "P {index} {} {}", payload.map_or("-".into(), |p| hex(&p)), if r.is_ok() { "ok" } else { "err" }).unwrap();
            while let Some(o) = v.read() {
                let pcm: Vec<u8> = o.pcm.iter().flat_map(|s| s.to_le_bytes()).collect();
                writeln!(f, "O {} {:016x}", o.index, fnv(&pcm)).unwrap();
            }
        }
        let r = v.flush();
        writeln!(f, "X {}", if r.is_ok() { "ok" } else { "err" }).unwrap();
        while let Some(o) = v.read() {
            let pcm: Vec<u8> = o.pcm.iter().flat_map(|s| s.to_le_bytes()).collect();
            writeln!(f, "O {} {:016x}", o.index, fnv(&pcm)).unwrap();
        }
    }

    // ---- 5. 受信 DSP(極端な IQ: 無信号・飽和・DC・強い隣接波・大きな周波数ずれ・振幅の段差・途切れ)----
    let mut f = fs::File::create(out.join("dsp.txt")).unwrap();
    let mut iqf = fs::File::create(out.join("dsp_iq.cf32")).unwrap();
    for case in 0..24 * scale {
        let n = 24_000 + rng.below(24_000) as usize;
        let offset = [-65625.0, -3125.0, 21875.0, 65625.0][case % 4];
        let kind = case % 8;
        let freq_err = match kind {
            4 => 450.0 + rng.below(300) as f64,
            _ => rng.below(400) as f64 - 200.0,
        };
        let mut phase = 0.0f64;
        let mut iq = Vec::with_capacity(n);
        let mut sym_phase = 0.0f64;
        for i in 0..n {
            if i % 50 == 0 {
                sym_phase += PI / 4.0 + (rng.below(4) as f64) * PI / 2.0;
            }
            phase += 2.0 * PI * (offset + freq_err) / 240000.0;
            let noise = |r: &mut Rng| (r.next() as f64 / 4294967296.0 - 0.5) * 0.02;
            let amp = match kind {
                0 => 0.0,
                1 => 2.0,
                3 => if (i / 3000) % 2 == 0 { 0.3 } else { 0.003 },
                5 => if (i / 4000) % 3 == 0 { 0.0 } else { 0.2 },
                _ => 0.2,
            };
            let (mut re, mut im) = (amp * (sym_phase + phase).cos() + noise(&mut rng), amp * (sym_phase + phase).sin() + noise(&mut rng));
            if kind == 2 {
                re += 0.5;
            }
            if kind == 6 {
                let p2 = 2.0 * PI * (offset + 6250.0) * i as f64 / 240000.0;
                re += 0.8 * p2.cos();
                im += 0.8 * p2.sin();
            }
            if kind == 1 {
                re = re.clamp(-1.0, 1.0);
                im = im.clamp(-1.0, 1.0);
            }
            iq.push(Complex32 { real: re as f32, imaginary: im as f32 });
        }
        for s in &iq {
            iqf.write_all(&s.real.to_le_bytes()).unwrap();
            iqf.write_all(&s.imaginary.to_le_bytes()).unwrap();
        }
        let mut dsp = Dsp::new(240000.0, offset).unwrap();
        let mut dibits = Vec::new();
        let mut buf = Vec::new();
        let mut pos = 0;
        while pos < iq.len() {
            let k = (1 + rng.below(5000) as usize).min(iq.len() - pos);
            dsp.process(&iq[pos..pos + k], &mut buf).unwrap();
            dibits.extend_from_slice(&buf);
            pos += k;
        }
        let (_, evm) = dsp.signal();
        writeln!(f, "Q {n} {offset} {} {} {:?}", dibits.len(), hex(&dibits), evm).unwrap();
    }
    eprintln!("fuzz vectors written to {}", out.display());
}

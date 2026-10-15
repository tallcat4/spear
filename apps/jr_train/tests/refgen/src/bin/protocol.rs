//! JR TRAIN RADIO の参照ベクトル生成器: フレーム復号・データ電文・パイプライン(開発用)。
//!
//! 原本(receiver-core)の復号器に、ここで符号化した有効なフレーム(音声・データ)と、そこにビット誤りを入れたものを通し、
//! 結果を書き出す。C++ 移植(apps/jr_train/protocol、pipeline)は同じ入力で同じ結果になることをテストする。
//! 符号化器は原本の復号器(とそのテスト)の逆として書いたテスト用のもの。
//!
//! 使い方(apps/jr_train/tests/refgen で):
//!   CARGO_TARGET_DIR=/tmp/jr-refgen cargo run --release --bin protocol -- ../vectors/protocol ../../protocol/jis0208_table.cpp
use receiver_core::data::{decode_frame, Decoder, Field, Format};
use receiver_core::frame::{Frame, Synchronizer};
use receiver_core::pipeline::{ChannelConfig, Pipeline};
use receiver_core::Complex32;
use std::f64::consts::PI;
use std::fmt::Write as _;
use std::fs;
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

// ---- 出力 ----
struct Case {
    out: String,
}
impl Case {
    fn new(name: &str, format: Format) -> Self {
        Self {
            out: format!("case {name} {}\n", if format == Format::Common { "common" } else { "slotted" }),
        }
    }
}

fn data_case(gf: &Gf, rng: &mut Rng, name: &str, format: Format, frames: &[(u64, [u8; 48])]) -> String {
    let mut c = Case::new(name, format);
    let mut decoder = Decoder::new(format);
    let _ = rng;
    for (i, (offset, e)) in frames.iter().enumerate() {
        writeln!(c.out, "f {offset} {}", hex(e)).unwrap();
        let frame = Frame { bit_offset: *offset, sync_matched: true, marker_errors: 0, received: *e };
        for m in decoder.decode(&frame) {
            writeln!(c.out, "m {i} {}", serde_json::to_string(&m).unwrap()).unwrap();
        }
    }
    let s = &decoder.stats;
    writeln!(c.out, "s {} {} {} {}", s.valid_frames, s.corrected_frames, s.rejected_frames, s.rejected_messages).unwrap();
    let _ = gf;
    c.out
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

fn jis_table(path: &Path) {
    let mut s = String::new();
    s.push_str("// 自動生成: tests/refgen(bin protocol)。手で編集しない。\n");
    s.push_str("// JIS X 0208 の区点(0x21..0x7E × 0x21..0x7E)→ Unicode。原本と同じく encoding_rs の EUC-JP 復号で求めた値\n");
    s.push_str("// (WHATWG Encoding Standard の index-jis0208、CC0)。0 は対応なし。原本の特例(0x2141 等)と除外する区は text.cpp が扱う。\n");
    s.push_str("#include <cstdint>\n\nnamespace spear::jr_train::protocol {\n\nextern const uint16_t kJis0208[94 * 94] = {\n");
    for row in 0x21u8..=0x7e {
        s.push_str("   ");
        for cell in 0x21u8..=0x7e {
            let bytes = [row | 0x80, cell | 0x80];
            let v = encoding_rs::EUC_JP
                .decode_without_bom_handling_and_without_replacement(&bytes)
                .and_then(|t| {
                    let mut it = t.chars();
                    let c = it.next()?;
                    if it.next().is_some() || c as u32 > 0xffff {
                        panic!("unexpected multi-char/non-BMP at {row:02x}{cell:02x}");
                    }
                    Some(c as u32)
                })
                .unwrap_or(0);
            write!(s, " 0x{v:04x},").unwrap();
        }
        s.push('\n');
    }
    s.push_str("};\n\n} // namespace spear::jr_train::protocol\n");
    fs::write(path, s).unwrap();
}

/// π/4-DQPSK で 48 byte フレームの列を変調(src/main.rs と同じ規約)
fn modulate(frames: &[[u8; 48]], amplitude: f64, offset_hz: f64, rate: f64, samples: usize, timing: usize, rng: &mut Rng) -> Vec<(f64, f64)> {
    const SPS: usize = 50;
    let mut dibits = Vec::new();
    for f in frames {
        for byte in f {
            for shift in [6, 4, 2, 0] {
                dibits.push((byte >> shift) & 3);
            }
        }
    }
    // 先頭に乱数(同期前の区間)
    let mut lead: Vec<u8> = (0..400).map(|_| (rng.next() >> 30) as u8).collect();
    lead.extend(dibits);
    let dibits = lead;
    let pulse: Vec<f64> = {
        const A: f64 = 0.2;
        let count = 16 * SPS + 1;
        (0..count)
            .map(|i| {
                let t = (i as f64 - (count - 1) as f64 / 2.0) / SPS as f64;
                if t == 0.0 {
                    1.0 - A + 4.0 * A / PI
                } else if ((4.0 * A * t).abs() - 1.0).abs() < 1e-9 {
                    A / 2f64.sqrt() * ((1.0 + 2.0 / PI) * (PI / (4.0 * A)).sin() + (1.0 - 2.0 / PI) * (PI / (4.0 * A)).cos())
                } else {
                    ((PI * t * (1.0 - A)).sin() + 4.0 * A * t * (PI * t * (1.0 + A)).cos()) / (PI * t * (1.0 - 16.0 * A * A * t * t))
                }
            })
            .collect()
    };
    let mut base = vec![(0.0f64, 0.0f64); dibits.len() * SPS + pulse.len()];
    let mut phase = 0.0f64;
    for (n, &d) in dibits.iter().enumerate() {
        phase += PI / 4.0 + [0usize, 1, 3, 2][d as usize] as f64 * PI / 2.0;
        let (s, c) = phase.sin_cos();
        for (j, &h) in pulse.iter().enumerate() {
            base[n * SPS + j].0 += c * h;
            base[n * SPS + j].1 += s * h;
        }
    }
    (0..samples)
        .map(|i| {
            let k = i + timing + pulse.len() / 2;
            let (re, im) = if k < base.len() { base[k] } else { (0.0, 0.0) };
            let w = 2.0 * PI * offset_hz * i as f64 / rate;
            let (s, c) = w.sin_cos();
            (amplitude * (re * c - im * s), amplitude * (re * s + im * c))
        })
        .collect()
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let out = Path::new(&args[1]);
    fs::create_dir_all(out).unwrap();
    if let Some(table) = args.get(2) {
        jis_table(Path::new(table));
    }
    // 生成した環境(C++ のテストはこれが一致するときだけ浮動小数に依存する結果のビット一致を要求する: tests/platform.hpp)
    fs::write(out.join("platform"), format!("{}-{}\n", std::env::consts::OS, std::env::consts::ARCH)).unwrap();
    let gf = Gf::new();
    let mut rng = Rng(0x2468_ace1);

    // ---- 1. 音声フレームの復号(原本 voice_frame::decode)----
    let mut v = String::new();
    for n in 0..400 {
        let payload = random_payload(&mut rng);
        let clean = voice_frame(&gf, &payload, (n % 2) as u8);
        let errors = match n % 8 {
            0 => 0,
            1 => 1,
            2 => 2,
            3 => 4,
            4 => 6,
            5 => 9,
            6 => 14,
            _ => 30,
        };
        let e = corrupt(&mut rng, &clean, errors);
        let r = receiver_core::voice_frame::decode(&e);
        match r {
            Some(s) => writeln!(v, "{} {} {}", hex(&e), hex(&s.payload), s.corrected_bits).unwrap(),
            None => writeln!(v, "{} - -", hex(&e)).unwrap(),
        }
    }
    // 乱数の包絡(ほぼすべて棄却)とデータフレーム(音声ではない)
    for _ in 0..100 {
        let mut e = [0u8; 48];
        for b in e.iter_mut() {
            *b = rng.byte();
        }
        let r = receiver_core::voice_frame::decode(&e);
        match r {
            Some(s) => writeln!(v, "{} {} {}", hex(&e), hex(&s.payload), s.corrected_bits).unwrap(),
            None => writeln!(v, "{} - -", hex(&e)).unwrap(),
        }
    }
    fs::write(out.join("voice_frames.txt"), v).unwrap();

    // ---- 2. データフレーム単体(decode_frame: RS(42,30) + CRC)----
    let mut d = String::new();
    for n in 0..300 {
        let mut info = [0u8; 28];
        for b in info.iter_mut() {
            *b = rng.byte();
        }
        let clean = data_frame(&gf, &info);
        let e = corrupt(&mut rng, &clean, [0, 1, 3, 6, 7, 10][n % 6]);
        match decode_frame(&e) {
            Some((w, c)) => writeln!(d, "{} {} {}", hex(&e), hex(&w), c).unwrap(),
            None => writeln!(d, "{} - -", hex(&e)).unwrap(),
        }
    }
    fs::write(out.join("data_frames.txt"), d).unwrap();

    // ---- 3. データ電文(Decoder: パケット組立・電文解析)----
    let mut cases = String::new();
    // 3a. Slotted: 各種電文 + チャネル状況 + 呼設定
    {
        let mut frames = Vec::new();
        let mut info = [0u8; 28];
        info[0] = 0x80;
        info[1] = 0x20;
        info[2..4].copy_from_slice(&[0x12, 0x34]);
        info[4..15].copy_from_slice(&[0x12, 0x01, 0x05, 0x01, 0x12, 0x34, 0x03, 0x02, 0x81, 0x23, 0x80]);
        info[15..26].copy_from_slice(&[0x07, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00]);
        frames.push((0u64, data_frame(&gf, &info)));
        let mut call = [0u8; 28];
        call[0] = 0xa0;
        call[1..4].copy_from_slice(&[0x45, 0x67, 0x01]);
        call[4..7].copy_from_slice(&[0x12, 0x34, 0x02]);
        call[7] = 0x03;
        call[8] = 0x20;
        call[9] = 3;
        call[10..12].copy_from_slice(&[0x01, 0x23]);
        call[12..14].copy_from_slice(&[0xff, 0x02]);
        frames.push((384, data_frame(&gf, &call)));
        let jis_text: Vec<u8> = vec![0x3b, 0x6e, 0x38, 0x33, 0x0d, 0x0a, 0x21, 0x41, 0x24, 0x22, 0x00, 0x7f, 0x30, 0x21, 0xb0, 0xa2];
        let packets = vec![presence(), registration(), transfer(), notice(&jis_text), vec![0x77, 3, 1, 2, 3]];
        frames.extend(frames_for(&gf, Format::Slotted, &packets, 384 * 4 + 384 * 3, 3));
        cases += &data_case(&gf, &mut rng, "slotted_messages", Format::Slotted, &frames);
        // 同じ列に誤りを入れたもの(訂正 / 棄却 / 組立のやり直し)
        let mut damaged = frames.clone();
        for (i, f) in damaged.iter_mut().enumerate() {
            let n = [0, 2, 5, 0, 7, 1, 0, 13][i % 8];
            f.1 = corrupt(&mut rng, &f.1, n);
        }
        cases += &data_case(&gf, &mut rng, "slotted_damaged", Format::Slotted, &damaged);
    }
    // 3b. Common: 一覧 / 旅客案内(非圧縮・LH5)/ 運行把握(LH5 定数ブロック・ハフマン)
    {
        let content = service_content();
        let mut packets = vec![directory(0), directory(13)];
        let mut p = common_packets(0x21, 0x46, &service_body(&content, false), 1);
        packets.append(&mut p);
        let mut p = common_packets(0x21, 0xc6, &service_body(&content, true), 2);
        packets.append(&mut p);
        let mut p = common_packets(0x22, 0xc9, &operation_body(&operation_table(0), false), 0x1234);
        packets.append(&mut p);
        let mut p = common_packets(0x22, 0xc9, &operation_body(&operation_table(1), true), 0x1235);
        packets.append(&mut p);
        packets.push(vec![0x23, 2, 9, 9]);
        let frames = frames_for(&gf, Format::Common, &packets, 0, 0);
        cases += &data_case(&gf, &mut rng, "common_messages", Format::Common, &frames);
        let mut damaged = frames.clone();
        for (i, f) in damaged.iter_mut().enumerate() {
            let n = [0, 0, 3, 0, 8, 0, 1, 0, 0, 20][i % 10];
            f.1 = corrupt(&mut rng, &f.1, n);
        }
        cases += &data_case(&gf, &mut rng, "common_damaged", Format::Common, &damaged);
    }
    fs::write(out.join("data_cases.txt"), cases).unwrap();

    // ---- 4. LH5 と文字列(原本の非公開関数)----
    let mut t = String::new();
    let table = operation_table(2);
    for a in [archive(&table), archive_huffman(&table), archive_huffman(&service_content())] {
        for cut in [a.len(), a.len() - 1, 30, 26] {
            let input = &a[..cut];
            match shim::lha::expand(input) {
                Ok(o) => writeln!(t, "lha {} ok {}", hex(input), hex(&o)).unwrap(),
                Err(e) => writeln!(t, "lha {} err {e}", hex(input)).unwrap(),
            }
        }
        let mut bad = a.clone();
        let k = bad.len() - 3;
        bad[k] ^= 0x10;
        match shim::lha::expand(&bad) {
            Ok(o) => writeln!(t, "lha {} ok {}", hex(&bad), hex(&o)).unwrap(),
            Err(e) => writeln!(t, "lha {} err {e}", hex(&bad)).unwrap(),
        }
    }
    for _ in 0..60 {
        let n = 1 + rng.below(40) as usize;
        let bytes: Vec<u8> = (0..n)
            .map(|_| match rng.below(5) {
                0 => 0x21 + rng.below(94) as u8,
                1 => 0xa1 + rng.below(94) as u8,
                2 => rng.byte(),
                3 => [0, 13, 10, 0x21][rng.below(4) as usize],
                _ => 0x30 + rng.below(0x40) as u8,
            })
            .collect();
        for trim in [false, true] {
            writeln!(t, "notice {} {} {}", u8::from(trim), hex(&bytes), hex(shim::text::notice(&bytes, trim).as_bytes())).unwrap();
        }
    }
    for row in [0x21u8, 0x22, 0x24, 0x2d, 0x30, 0x4f, 0x50, 0x74, 0x75, 0x79, 0x7c, 0x7e] {
        for cell in [0x21u8, 0x41, 0x42, 0x5d, 0x71, 0x72, 0x4c, 0x7e] {
            match shim::jis(row, cell) {
                Ok(s) => writeln!(t, "jis {row:02x}{cell:02x} ok {}", hex(s.as_bytes())).unwrap(),
                Err(e) => writeln!(t, "jis {row:02x}{cell:02x} err {e}").unwrap(),
            }
        }
    }
    let content = service_content();
    for cut in [content.len(), content.len() - 1, 12] {
        match shim::text::service(&content[..cut]) {
            Ok(f) => writeln!(t, "service {} ok {}", hex(&content[..cut]), serde_json::to_string(&f).unwrap()).unwrap(),
            Err(e) => writeln!(t, "service {} err {e}", hex(&content[..cut])).unwrap(),
        }
    }
    fs::write(out.join("text_cases.txt"), t).unwrap();
    let _: Option<Field> = None;

    // ---- 5. パイプライン(フレーム → 音声フレーム復号 → 音声合成 / データ電文。原本 Pipeline::frame_observed)----
    // Slotted のチャネル: 音声 40 フレーム → 欠落 3 → 音声 20 → 無音(データ)→ 再同期(bit_offset が飛ぶ)→ 音声 12
    let mut pipe_frames: Vec<(u64, [u8; 48])> = Vec::new();
    let mut off = 384 * 10;
    for i in 0..40 {
        pipe_frames.push((off, voice_frame(&gf, &random_payload(&mut rng), (i % 2) as u8)));
        off += 384;
    }
    off += 384 * 3;
    for i in 0..20 {
        let clean = voice_frame(&gf, &random_payload(&mut rng), (i % 2) as u8);
        pipe_frames.push((off, corrupt(&mut rng, &clean, [0, 1, 3, 0, 5][i % 5])));
        off += 384;
    }
    let packets = vec![presence(), registration()];
    let data = frames_for(&gf, Format::Slotted, &packets, off + 384 * 2, 3);
    off = data.last().unwrap().0 + 384 * 5;
    pipe_frames.extend(data);
    off += 100; // 再同期(384 の倍数でない)
    for i in 0..12 {
        pipe_frames.push((off, voice_frame(&gf, &random_payload(&mut rng), (i % 2) as u8)));
        off += 384;
    }
    let mut pp = String::new();
    for (name, freq, format, frames) in [
        ("slotted", 352_600_000.0, Format::Slotted, pipe_frames.clone()),
        (
            "common",
            352_625_000.0,
            Format::Common,
            frames_for(&gf, Format::Common, &common_packets(0x22, 0xc9, &operation_body(&operation_table(3), true), 7), 0, 0),
        ),
    ] {
        writeln!(pp, "case {name} {}", if format == Format::Common { "common" } else { "slotted" }).unwrap();
        let config = ChannelConfig { id: format!("{}", freq as u64), label: name.into(), frequency_hz: freq, format };
        let mut pipe = Pipeline::new(config, 240000.0, 352603125.0).unwrap();
        let mut events = String::new();
        for (offset, e) in &frames {
            writeln!(pp, "f {offset} {}", hex(e)).unwrap();
            let frame = Frame { bit_offset: *offset, sync_matched: true, marker_errors: 0, received: *e };
            let audio = pipe
                .frame_observed(&frame, &mut |ev| {
                    let ms: Vec<String> = ev.messages.iter().map(|m| serde_json::to_string(m).unwrap()).collect();
                    writeln!(
                        events,
                        "e {} {} {} {}",
                        ev.epoch,
                        ev.index,
                        ev.corrected_bits.map_or("-".to_string(), |c| c.to_string()),
                        ms.len()
                    )
                    .unwrap();
                    for m in ms {
                        writeln!(events, "m {m}").unwrap();
                    }
                })
                .unwrap();
            pp += &events;
            events.clear();
            for a in audio {
                let pcm: Vec<u8> = a.pcm.iter().flat_map(|s| s.to_le_bytes()).collect();
                writeln!(pp, "a {} {} {}", a.epoch, a.index, hex(&pcm)).unwrap();
            }
        }
        for a in pipe.flush().unwrap() {
            let pcm: Vec<u8> = a.pcm.iter().flat_map(|s| s.to_le_bytes()).collect();
            writeln!(pp, "a {} {} {}", a.epoch, a.index, hex(&pcm)).unwrap();
        }
        let st = pipe.snapshot();
        writeln!(
            pp,
            "status {} {} {} {} {} {} {} {}",
            st.frames, st.synchronized_frames, st.voice_frames, st.rejected_voice, st.corrected_bits, st.audio_frames, st.epoch, st.data.valid_frames
        )
        .unwrap();
        for r in &st.messages {
            writeln!(pp, "h {} {}", r.receptions, serde_json::to_string(&r.message).unwrap()).unwrap();
        }
        if let Some(op) = &st.operation_monitor {
            writeln!(pp, "op {} {}", op.receptions, serde_json::to_string(&op.summary).unwrap()).unwrap();
        }
    }
    fs::write(out.join("pipeline.txt"), pp).unwrap();

    // ---- 6. 通し(帯域 IQ → 原本の Pipeline::process)----
    // 4 チャネル: 音声(Slotted)/ データ(Slotted)/ 共用波(運行把握・旅客案内)/ 空き
    const RATE: f64 = 240_000.0;
    const CENTER: f64 = 352_603_125.0;
    const SECONDS: f64 = 2.4;
    let samples = (RATE * SECONDS) as usize;
    let nframes = (SECONDS / 0.04) as usize + 2;
    // 音声: 連続 25 フレーム、5 フレームの無信号(乱数)、さらに 20
    let voice: Vec<[u8; 48]> = (0..nframes)
        .map(|i| {
            if (25..30).contains(&i) {
                let mut e = [0u8; 48];
                for b in e.iter_mut() {
                    *b = rng.byte();
                }
                e
            } else {
                voice_frame(&gf, &random_payload(&mut rng), (i % 2) as u8)
            }
        })
        .collect();
    // データ(Slotted): パケットを 16 スロット周期の位置に
    let data_infos = {
        let mut v = vec![[0u8; 48]; nframes];
        let mut fill = [0u8; 28];
        fill[0] = 0x10;
        for slot in v.iter_mut() {
            *slot = data_frame(&gf, &fill);
        }
        let packets = vec![presence(), registration(), transfer()];
        let mut start = 4usize;
        for p in &packets {
            for (o, info) in packet_infos(Format::Slotted, p, (start * 384) as u64, (start % 16) as u8) {
                let k = (o / 384) as usize;
                if k < v.len() {
                    v[k] = data_frame(&gf, &info);
                }
            }
            start += 16;
            while start % 16 != 3 {
                start += 1;
            }
        }
        v
    };
    let common_frames = {
        let mut v = vec![[0u8; 48]; nframes];
        let mut idle = [0u8; 28];
        idle[0] = 0x3f; // 不完全な続きスロット(棄却されるが CRC は正しい)
        for slot in v.iter_mut() {
            *slot = data_frame(&gf, &idle);
        }
        let mut packets = common_packets(0x22, 0xc9, &operation_body(&operation_table(4), true), 0x4242);
        packets.extend(common_packets(0x21, 0x46, &service_body(&service_content(), false), 3));
        let mut k = 3usize;
        for p in &packets {
            for (_, info) in packet_infos(Format::Common, p, 0, 0) {
                if k < v.len() {
                    v[k] = data_frame(&gf, &info);
                }
                k += 1;
            }
            k += 2;
        }
        v
    };
    let chans = [
        (352_587_500.0, Format::Slotted, voice, 0.20, 80.0, 7usize),
        (352_637_500.0, Format::Slotted, data_infos, 0.15, -120.0, 23),
        (352_625_000.0, Format::Common, common_frames, 0.18, 30.0, 41),
    ];
    let mut band = vec![(0.0f64, 0.0f64); samples];
    for (freq, _, frames, amp, err, timing) in &chans {
        let sig = modulate(frames, *amp, freq - CENTER + err, RATE, samples, *timing, &mut rng);
        for (b, s) in band.iter_mut().zip(sig) {
            b.0 += s.0;
            b.1 += s.1;
        }
    }
    let mut iq16 = Vec::with_capacity(samples * 4);
    let mut iq = Vec::with_capacity(samples);
    for b in &band {
        let mut n = [0.0f64; 2];
        for x in n.iter_mut() {
            *x = ((0..4).map(|_| rng.next() as f64 / 4294967296.0).sum::<f64>() - 2.0) * 0.003;
        }
        let re = ((b.0 + n[0]) * 32768.0).round().clamp(-32768.0, 32767.0) as i16;
        let im = ((b.1 + n[1]) * 32768.0).round().clamp(-32768.0, 32767.0) as i16;
        iq16.extend_from_slice(&re.to_le_bytes());
        iq16.extend_from_slice(&im.to_le_bytes());
        iq.push(Complex32 { real: re as f32 / 32768.0, imaginary: im as f32 / 32768.0 });
    }
    fs::write(out.join("e2e_240k.sc16"), &iq16).unwrap();
    let mut e2e = String::new();
    writeln!(e2e, "rate {RATE}\ncenter {CENTER}\nsamples {samples}").unwrap();
    for (freq, format, _, _, _, _) in &chans {
        let config = ChannelConfig { id: format!("{}", *freq as u64), label: "e2e".into(), frequency_hz: *freq, format: *format };
        writeln!(e2e, "case {} {}", *freq as u64, if *format == Format::Common { "common" } else { "slotted" }).unwrap();
        let mut pipe = Pipeline::new(config, RATE, CENTER).unwrap();
        let mut sync = Synchronizer::default();
        let _ = &mut sync;
        for chunk in iq.chunks(4096) {
            let mut events = Vec::new();
            for a in pipe
                .process_observed(chunk, |ev| {
                    events.push(format!(
                        "e {} {} {} {} {}",
                        ev.epoch,
                        ev.index,
                        ev.frame.bit_offset,
                        ev.corrected_bits.map_or("-".to_string(), |c| c.to_string()),
                        ev.messages.len()
                    ));
                    for m in &ev.messages {
                        events.push(format!("m {}", serde_json::to_string(m).unwrap()));
                    }
                })
                .unwrap()
            {
                let pcm: Vec<u8> = a.pcm.iter().flat_map(|s| s.to_le_bytes()).collect();
                events.push(format!("a {} {} {}", a.epoch, a.index, hex(&pcm)));
            }
            for e in events {
                writeln!(e2e, "{e}").unwrap();
            }
        }
        for a in pipe.flush().unwrap() {
            let pcm: Vec<u8> = a.pcm.iter().flat_map(|s| s.to_le_bytes()).collect();
            writeln!(e2e, "a {} {} {}", a.epoch, a.index, hex(&pcm)).unwrap();
        }
        let st = pipe.snapshot();
        writeln!(
            e2e,
            "status {} {} {} {} {} {} {} {}",
            st.frames, st.synchronized_frames, st.voice_frames, st.rejected_voice, st.corrected_bits, st.audio_frames, st.epoch, st.data.valid_frames
        )
        .unwrap();
        eprintln!(
            "e2e {}: frames {} synced {} voice {} rejected {} audio {} data valid {} messages {}",
            freq, st.frames, st.synchronized_frames, st.voice_frames, st.rejected_voice, st.audio_frames, st.data.valid_frames, st.messages.len()
        );
    }
    fs::write(out.join("e2e.txt"), e2e).unwrap();
}

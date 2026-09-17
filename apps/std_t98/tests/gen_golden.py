#!/usr/bin/env python3
# std-t98-tools の Python 実装を真値として、ランダムシンボル列と期待デコード結果を
# C++ ヘッダ (golden_vectors.hpp) に吐く。C++ 移植の等価性検証に使う。
# 使い方: PYTHONPATH=../std-t98-tools python3 gen_golden.py > golden_vectors.hpp
import os, sys, random
sys.path.insert(0, os.path.expanduser("~/Documents/std-t98-tools"))
from core.protocol.dewhiten import dewhiten
from core.protocol.frame_layout import parse_frame_symbols
from core.protocol.rich import decode_rich
from core.protocol.sacch import decode_sacch
from core.protocol.pich import decode_pich
import numpy as np

random.seed(20260917)
LEVELS = [-3, -1, 1, 3]
N = 40

def cpp_bool(b): return "true" if b else "false"

print("// 自動生成: apps/std_t98/tests/gen_golden.py。手で編集しない。")
print("// std-t98-tools の Python 実装を真値とした golden vector。")
print("#pragma once")
print("#include <cstdint>")
print("#include <vector>")
print("#include <string>")
print("namespace spear::std_t98::golden {")
print("struct Case {")
print("  std::vector<int8_t> symbols;")
print("  int rich_f, rich_m, rich_d, rich_parity; bool rich_parity_ok;")
print("  int sacch_msg_type, sacch_user, sacch_maker, sacch_call, sacch_errors; bool sacch_crc_ok; std::string sacch_crc_recv;")
print("  std::string pich_csm; int pich_errors; bool pich_crc_ok; std::string pich_crc_recv;")
print("};")
print("inline const std::vector<Case>& cases() { static const std::vector<Case> c = {")

for _ in range(N):
    syms = [random.choice(LEVELS) for _ in range(192)]
    dw = dewhiten(np.array(syms, dtype=np.int8))
    fields = parse_frame_symbols(dw)
    r = decode_rich(fields["RICH"])
    s = decode_sacch(fields["SACCH"])
    p = decode_pich(fields["TCH1"])
    symlist = ",".join(str(x) for x in syms)
    print("  {{" + symlist + "},")
    print("   %d,%d,%d,%d,%s," % (r['F'], r['M'], r['D'], r['Parity'], cpp_bool(r['Parity_OK'])))
    print("   %d,%d,%d,%d,%d,%s,\"%s\"," % (s['MsgType'], s['UserCode'], s['MakerCode'], s['CallStat'], s['BitErrors'], cpp_bool(s['CRC_OK']), s['CRC_Recv']))
    print("   \"%s\",%d,%s,\"%s\"}," % (p['CSM'], p['BitErrors'], cpp_bool(p['CRC_OK']), p['CRC_Recv']))

print("  }; return c; }")
print("}")

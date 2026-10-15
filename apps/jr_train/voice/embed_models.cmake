# 学習済みモデル(models/*.bin)を C++ のバイト配列にする(cmake -P で実行。voice/CMakeLists.txt の add_custom_command)。
#   -DOUTPUT=<生成する .cpp> -DINPUTS=<.bin;.bin;...>
# 移植性のため .incbin(ELF 専用)ではなく配列で埋め込む。名前は拡張子を除いたファイル名(embedded_model("predictor") など)。
cmake_minimum_required(VERSION 3.25)
set(_out "// 自動生成: apps/jr_train/voice/embed_models.cmake。手で編集しない。\n#include \"models.hpp\"\n#include <cstring>\n\nnamespace spear::jr_train::voice {\nnamespace {\n")
set(_table "")
foreach(_in ${INPUTS})
  get_filename_component(_name "${_in}" NAME_WE)
  file(READ "${_in}" _hex HEX)
  string(LENGTH "${_hex}" _len)
  math(EXPR _bytes "${_len} / 2")
  # 2 桁ずつ 0xNN, に。1 行 32 byte で改行する
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _body "${_hex}")
  string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" _body "${_body}")
  string(APPEND _out "const unsigned char k_${_name}[${_bytes}] = {\n${_body}\n};\n")
  string(APPEND _table "    if (std::strcmp(name, \"${_name}\") == 0) return {k_${_name}, sizeof k_${_name}};\n")
endforeach()
string(APPEND _out "} // namespace\n\nBlob embedded_model(const char* name) {\n${_table}    return {};\n}\n\n} // namespace spear::jr_train::voice\n")
file(WRITE "${OUTPUT}.tmp" "${_out}")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")

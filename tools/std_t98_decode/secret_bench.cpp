// FEC 後の同一実録音窓を使い、線形ソルバーと旧 AI 単独経路を比較する。
// 入力は spear-std-t98-decode --raw2450 の出力。鍵・キャッシュは各試行で消す。
#include "secret/cracker.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
using namespace spear::std_t98::secret;
using Clock = std::chrono::steady_clock;
struct Call { std::vector<Burst> bursts; std::vector<uint64_t> symbols; };
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <raw2450.txt> <channel 0-based> [repeats=5]\n", argv[0]);
        return 1;
    }
    const int channel = std::stoi(argv[2]);
    const int repeats = argc > 3 ? std::stoi(argv[3]) : 5;
    if (repeats < 1) return 1;
    std::ifstream input(argv[1]);
    if (!input) return 1;
    std::vector<Call> calls;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream in(line);
        int ch; uint64_t symbol;
        if (!(in >> ch >> symbol) || ch != channel) continue;
        Burst burst{};
        for (auto& frame : burst) {
            std::string hex;
            if (!(in >> hex) || hex.size() != 14) { std::cerr << "invalid frame\n"; return 1; }
            std::array<uint8_t,7> bytes{};
            for (std::size_t i=0; i<bytes.size(); ++i)
                bytes[i] = static_cast<uint8_t>(std::stoul(hex.substr(i*2,2), nullptr, 16));
            frame = unpack_frame49(bytes);
        }
        // この録音は PTT 間に 4 TCH より長い空白がある。短い欠落は分割しない。
        if (calls.empty() || symbol > calls.back().symbols.back()+4*192) calls.push_back({});
        calls.back().bursts.push_back(burst);
        calls.back().symbols.push_back(symbol);
    }
    if (calls.empty()) return 1;
    Ffnn ffnn;
    Hybrid hybrid;
    std::string error;
    auto start = Clock::now();
    if (!ffnn.load_embedded(&error) || !hybrid.load_embedded(&error)) { std::cerr << error << '\n'; return 1; }
    std::cerr << "models_load_ms=" << std::chrono::duration<double,std::milli>(Clock::now()-start).count() << '\n';
    start = Clock::now();
    Cracker solver(ffnn, hybrid);
    std::cerr << "solver_init_ms=" << std::chrono::duration<double,std::milli>(Clock::now()-start).count() << '\n';
    start = Clock::now();
    Cracker ai(ffnn, hybrid, {.ai_fallback=true, .analytic=false});
    std::cerr << "ai_init_ms=" << std::chrono::duration<double,std::milli>(Clock::now()-start).count() << '\n';
    // 実運用と同じプロセス内再利用。計測前に双方を一度呼んで初回の影響を除く。
    if (calls[0].bursts.size() >= 5) {
        const auto warm = std::span<const Burst>(calls[0].bursts.data(),5);
        solver.resolve(0,warm); ai.resolve(0,warm);
    }
    std::cout << "ptt,received_bursts,window_bursts,collection_ms,method,repeat,key,source,wall_ms\n";
    for (std::size_t ci=0; ci<calls.size(); ++ci) {
        const auto& call = calls[ci];
        std::cerr << "PTT " << ci+1 << ": " << call.bursts.size() << " bursts\n";
        for (std::size_t n=5; n<=call.bursts.size(); n+=5) {
            const auto count = std::min<std::size_t>(n,10);
            const auto window = std::span<const Burst>(call.bursts.data()+n-count,count);
            // 1 TCH は 192 symbols / 2400 symbols/s。最初の TCH の開始から末尾まで。
            const double collection = static_cast<double>(call.symbols[n-1]-call.symbols[0]+192)/2.4;
            for (int r=0; r<repeats; ++r) for (int order=0; order<2; ++order) {
                const bool use_ai = ((r+order)%2)==1;
                auto& cracker = use_ai ? ai : solver;
                cracker.clear_cache();
                const auto t0 = Clock::now();
                const auto result = cracker.resolve(0,window);
                const double elapsed = std::chrono::duration<double,std::milli>(Clock::now()-t0).count();
                std::cout << ci+1 << ',' << n << ',' << count << ',' << collection << ','
                          << (use_ai ? "ai" : "solver") << ',' << r+1 << ',' << result.key << ','
                          << to_string(result.source) << ',' << elapsed << '\n' << std::flush;
            }
        }
    }
}

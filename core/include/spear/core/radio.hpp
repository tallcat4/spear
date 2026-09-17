// S.P.E.A.R. core — Radio (要件 §2, §17)
//
// B210 を所有し UHD を扱う唯一の主体。App はこれに触れない(App が触るのは Source/Stream のみ)。
// SoapySDR 等の抽象化層は挟まない (§17.1): 異常報告の情報量を最大化する。
//
// 起動シーケンス (§17.2) の各段を区別して報告する:
//   1. device 列挙 → serial 確認   2. FPGA/FW version 確認   3. clock source → ref_locked
//   4. tune → lo_locked (timeout 付き)   5. stream 開始 → settling   6. 定常監視
#pragma once

#include "event.hpp"
#include "types.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <uhd/usrp/multi_usrp.hpp>

namespace spear {

struct RadioOptions {
    std::string expected_serial;         // 空なら任意の B2xx を受け入れる(Appliance では固定すること)
    std::string device_args;             // 追加 UHD args。例 "num_recv_frames=128" (M-1 で overflow が出たら最初に触る)
    double      lock_timeout_s = 3.0;    // ref_locked / lo_locked 待ち
    std::size_t settle_blocks  = 8;      // stream 開始直後に捨てる block 数 (§17.2 手順5)
    // 期待する FW / FPGA version(空なら照合しない)。不一致は Warning(動作は続ける)
    std::string expected_fw_version;
    std::string expected_fpga_version;
    // USB link 帯域に対する要求 rate の上限比。超えたら Fault(維持できない構成を黙って流さない)
    double      link_budget_fault = 0.95;
    double      link_budget_warn  = 0.80;
    // tune 後の coercion 許容
    double      rate_tolerance_rel = 1e-6;
    double      freq_tolerance_hz  = 1.0;
    double      gain_tolerance_db  = 0.5;
};

// 起動時の最小自己診断 (§12.1) の結果。stage 0。
struct SelfCheck {
    bool ok = true;                      // false = 装置を開く前に分かる致命的欠陥(image 欠落等)
    std::string uhd_version;
    std::string fw_image, fpga_image;    // 見つかった image の path(空 = 見つからない)
    long rtprio_limit = 0;               // RLIMIT_RTPRIO (0 = SCHED_FIFO 不可)
    long memlock_limit_kb = 0;           // -1 = unlimited
    std::vector<std::string> notes;
};

struct RadioInfo {
    std::string serial, product, name, fpga_version, fw_version, pp_string;
    int usb_version = 0;       // /mboards/0/usb_version (2 or 3)
};

// probe() の結果を spear の状態へ写したもの(副作用なし。uhd::usrp::b2xx::probe 由来)
struct ProbeSummary {
    DeviceState state = DeviceState::Disconnected;
    std::string evidence;
    std::string serial;
    std::string fx3_state;
    bool attempt_open = false; // この状態なら open を試みてよい(NoFirmware / FX3 running・unconfigured・fpga_ready)
};

class Radio {
public:
    Radio(EventBus* events, RadioOptions opt);
    ~Radio();
    Radio(const Radio&) = delete;
    Radio& operator=(const Radio&) = delete;

    // 手順 1〜3。失敗段を err と StartupStage/Error event で区別して返す。
    bool open(const RfConfig& cfg, std::string* err = nullptr);
    void close();
    bool is_open() const;

    // 手順 4。rate/freq/gain/antenna/bandwidth を適用し lo_locked を確認する。
    bool tune_rx(const RfConfig& cfg, std::string* err = nullptr);

    // timed retune (§17.1): at(hardware time) から新周波数。実際の周波数を返す。
    // 呼び出しは B210LiveSource の RX thread からのみ。
    double retune_rx_at(double freq, const uhd::time_spec_t& at);

    uhd::rx_streamer::sptr make_rx_streamer(std::string* err = nullptr);
    uhd::usrp::multi_usrp::sptr usrp() const { return usrp_; }
    uhd::time_spec_t now() const;

    // stage 0: 装置に触らずに分かること(UHD version / image files / rlimit)。起動時に 1 回。
    SelfCheck self_check(const std::string& product_hint = "B210");

    // 受信中に安全に読めるセンサ(実測: temp/rssi/lo_locked/ref_locked/time は転送を止めない)
    LiveSensors read_live_sensors();
    const TimeReference& time_reference() const { return time_ref_; }
    void set_time_reference(const TimeReference& t);

    // ---- 装置状態 (§12.1) ----
    // 副作用なしで USB 上の B2xx を調べ、状態を更新して返す(device を open していない間に呼ぶ)。
    ProbeSummary probe();
    // open 中: FX3 状態レジスタを tree (/mboards/0/fx3_state_code) から読む。running でなければ false。
    bool check_live_fx3(std::string* state_str = nullptr);
    DeviceStatus status() const;
    uint64_t state_version() const { return state_version_.load(); }
    // 状態遷移。evidence は一次ソースの値をそのまま書く。変化時に DeviceState event を出す。
    void set_state(DeviceState st, std::string evidence, int stage = -1);
    void set_generation(uint64_t g);

    const RadioInfo& info() const { return info_; }
    const RadioOptions& options() const { return opt_; }
    double actual_rx_rate() const { return actual_rate_; }
    double actual_rx_freq() const { return actual_freq_; }

private:
    bool wait_sensor(const std::string& sensor, bool mboard, std::string* err);
    void stage(int n, const std::string& detail, bool ok);

    void install_uhd_log_hook();

    EventBus*    events_;
    RadioOptions opt_;
    uhd::usrp::multi_usrp::sptr usrp_;
    RadioInfo info_;
    double actual_rate_ = 0, actual_freq_ = 0;
    mutable std::mutex st_mu_;
    DeviceStatus st_;
    std::atomic<uint64_t> state_version_{0};
    TimeReference time_ref_;
    std::string clock_source_ = "internal";
};

} // namespace spear

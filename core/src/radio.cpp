#include "spear/core/radio.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>

#include <uhd/device.hpp>
#include <uhd/exception.hpp>
#include <uhd/property_tree.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/usrp/b2xx_probe.hpp>
#include <uhd/utils/log.hpp>
#include <uhd/utils/log_add.hpp>
#include <uhd/utils/paths.hpp>
#include <uhd/version.hpp>

#include <sys/resource.h>

namespace spear {

namespace {
// UHD のログを EventBus へ流す(プロセスで 1 回)。B200 コンポーネントの info(FW/FPGA ロード等)と
// 全コンポーネントの warning 以上を拾う。console への出力は warning 以上に絞る。
std::mutex g_hook_mu;
EventBus* g_hook_events = nullptr;
Radio* g_hook_radio = nullptr;     // UHD のロード報告を状態へ反映する先(プロセスに Radio は 1 つ)
bool g_hook_installed = false;
} // namespace

void Radio::install_uhd_log_hook() {
    std::lock_guard lk(g_hook_mu);
    g_hook_events = events_;
    g_hook_radio = this;
    if (g_hook_installed) return;
    g_hook_installed = true;
    uhd::log::add_logger("spear", [](const uhd::log::logging_info& li) {
        EventBus* ev;
        Radio* radio;
        {
            std::lock_guard lk2(g_hook_mu);
            ev = g_hook_events;
            radio = g_hook_radio;
        }
        if (!ev) return;
        const bool b200_info = (li.component == "B200" || li.component == "GPS") && li.verbosity >= uhd::log::info;
        const bool fpga_progress = li.component == "B200" && li.message.rfind("FPGA load:", 0) == 0; // debug level
        if (li.verbosity < uhd::log::warning && !b200_info && !fpga_progress) return;
        ev->emit(EventKind::UhdLog, "uhd/" + li.component, li.message, {}, static_cast<int64_t>(li.verbosity));
        // UHD 自身の「書き込み中」報告 → STANDBY(FPGA, 進捗つき)/ NO_FIRMWARE(FW)。open() 完了時に上書きされる。
        if (radio && li.component == "B200") {
            if (li.message.rfind("Loading FPGA image", 0) == 0 || fpga_progress) {
                radio->set_state(DeviceState::Standby, "uhd: " + li.message, 1);
                if (fpga_progress) radio->set_progress(std::atoi(li.message.c_str() + std::strlen("FPGA load:")));   // "FPGA load:  45%"
            } else if (li.message.rfind("Loading firmware image", 0) == 0)
                radio->set_state(DeviceState::NoFirmware, "uhd: " + li.message, 1);
        }
    });
    uhd::log::set_log_level(uhd::log::debug);      // FPGA 進捗(debug)を logger まで届かせる
    uhd::log::set_logger_level("spear", uhd::log::debug);
    uhd::log::set_console_level(uhd::log::warning);
    uhd::log::set_file_level(uhd::log::off);
}

Radio::Radio(EventBus* events, RadioOptions opt) : events_(events), opt_(std::move(opt)) {
    install_uhd_log_hook();
}

SelfCheck Radio::self_check(const std::string& product_hint) {
    SelfCheck sc;
    sc.uhd_version = uhd::get_version_string();
    auto find_img = [&](const std::string& name, std::string& out) {
        try { out = uhd::find_image_path(name); return true; }
        catch (const std::exception& ex) { sc.notes.push_back(name + ": " + ex.what()); return false; }
    };
    const std::string fpga_name = product_hint == "B200" ? "usrp_b200_fpga.bin"
                                : product_hint == "B200mini" ? "usrp_b200mini_fpga.bin"
                                : product_hint == "B205mini" ? "usrp_b205mini_fpga.bin" : "usrp_b210_fpga.bin";
    if (!find_img("usrp_b200_fw.hex", sc.fw_image)) sc.ok = false;
    if (!find_img(fpga_name, sc.fpga_image)) sc.ok = false;
    struct rlimit rl{};
    if (getrlimit(RLIMIT_RTPRIO, &rl) == 0) sc.rtprio_limit = static_cast<long>(rl.rlim_cur);
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0)
        sc.memlock_limit_kb = rl.rlim_cur == RLIM_INFINITY ? -1 : static_cast<long>(rl.rlim_cur / 1024);
    if (sc.rtprio_limit == 0) sc.notes.push_back("RLIMIT_RTPRIO=0: SCHED_FIFO unavailable (limits.d)");
    std::string summary = "uhd=" + sc.uhd_version + " fw_image=" + (sc.fw_image.empty() ? "MISSING" : "ok") +
                          " fpga_image=" + (sc.fpga_image.empty() ? "MISSING" : "ok") +
                          " rtprio=" + std::to_string(sc.rtprio_limit) +
                          " memlock_kb=" + std::to_string(sc.memlock_limit_kb);
    for (const auto& n : sc.notes) summary += "; " + n;
    stage(0, summary, sc.ok);
    { std::lock_guard lk(st_mu_); self_check_ = sc; }
    if (!sc.ok) set_state(DeviceState::Fault, "self-check: " + summary, 0);
    return sc;
}

LiveSensors Radio::read_live_sensors() {
    LiveSensors ls;
    if (!usrp_) return ls;
    try {
        ls.lo_locked = usrp_->get_rx_sensor("lo_locked").to_bool();
        if (clock_source_ != "internal") ls.ref_locked = usrp_->get_mboard_sensor("ref_locked").to_bool();
        ls.rx_temp_c = usrp_->get_rx_sensor("temp").to_real();
        ls.rssi_db = usrp_->get_rx_sensor("rssi").to_real();
        const uint64_t t0 = host_now_ns();
        const auto hw = usrp_->get_time_now();
        const uint64_t t1 = host_now_ns();
        ls.host_ns = t0 + (t1 - t0) / 2;   // 往復の中点
        ls.hw_time_s = hw.get_real_secs();
        if (time_ref_.valid) {
            const double host_dt = static_cast<double>(ls.host_ns - time_ref_.host_mono_ns) * 1e-9;
            const double hw_dt = ls.hw_time_s - time_ref_.hw_time_s;
            if (host_dt > 1.0) {
                ls.drift_ppm = (hw_dt - host_dt) / host_dt * 1e6;
                ls.drift_uncertainty_ppm = static_cast<double>((t1 - t0) + time_ref_.round_trip_ns) / 2.0 * 1e-9 / host_dt * 1e6;
            }
        }
        ls.sampled_ns = ls.host_ns;
        ls.valid = true;
    } catch (const std::exception& ex) {
        ls.valid = false;
    }
    {
        std::lock_guard lk(st_mu_);
        st_.sensors = ls;
    }
    return ls;
}

void Radio::set_time_reference(const TimeReference& t) {
    time_ref_ = t;
    if (events_) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "hw=%.9f s mono=%llu ns utc=%llu ns rt=%llu ns (%s, %s)", t.hw_time_s,
                      (unsigned long long)t.host_mono_ns, (unsigned long long)t.utc_ns, (unsigned long long)t.round_trip_ns,
                      t.reference.c_str(), t.accuracy.c_str());
        events_->emit(EventKind::TimeReference, "radio", buf, {t.generation, t.hw_sample_index, t.hw_sample_index},
                      static_cast<int64_t>(t.utc_ns));
    }
}

DeviceStatus Radio::status() const {
    std::lock_guard lk(st_mu_);
    return st_;
}

void Radio::set_state(DeviceState st, std::string evidence, int stage) {
    bool changed;
    {
        std::lock_guard lk(st_mu_);
        changed = (st != st_.state) || (evidence != st_.evidence);
        if (st != st_.state) st_.since_ns = host_now_ns();
        st_.state = st;
        st_.evidence = evidence;
        if (stage >= 0) st_.stage = stage;
        if (st != DeviceState::Standby) st_.progress_pct = -1;
        st_.serial = info_.serial;
    }
    if (changed) state_version_++;
    if (changed && events_)
        events_->emit(EventKind::DeviceState, "radio", std::string(to_string(st)) + ": " + evidence, {}, static_cast<int64_t>(st));
}

void Radio::set_progress(int pct) {
    bool changed;
    {
        std::lock_guard lk(st_mu_);
        changed = st_.progress_pct != pct;
        st_.progress_pct = pct;
    }
    if (changed) state_version_++;
}

void Radio::set_generation(uint64_t g) {
    std::lock_guard lk(st_mu_);
    st_.generation = g;
}

ProbeSummary Radio::probe() {
    namespace b2 = uhd::usrp::b2xx;
    ProbeSummary ps;
    std::vector<b2::probe_result> found;
    try {
        found = b2::probe();
    } catch (const std::exception& ex) {
        ps.state = DeviceState::Fault;
        ps.evidence = std::string("probe failed: ") + ex.what();
        set_state(ps.state, ps.evidence, 0);
        return ps;
    }
    // 期待 serial があればそれに絞る
    const b2::probe_result* dev = nullptr;
    for (const auto& r : found)
        if (opt_.expected_serial.empty() || r.serial == opt_.expected_serial) { dev = &r; break; }
    if (!dev) {
        if (found.empty()) {
            ps.state = DeviceState::Disconnected;
            ps.evidence = "libusb: no B2xx device (vid 2500/3923)";
        } else {
            // 別個体が刺さっている: 不在ではなく Fault(現場で「機材が違う」と分かるように)
            ps.state = DeviceState::Fault;
            ps.evidence = "wrong device: serial " + found.front().serial + " present, expected " + opt_.expected_serial;
        }
        set_state(ps.state, ps.evidence, 0);
        return ps;
    }
    ps.serial = dev->serial;
    {
        std::lock_guard lk(st_mu_);
        st_.serial = dev->serial;
        st_.fx3_state = dev->queried ? b2::to_string(dev->state) : "";
    }
    if (!dev->firmware_loaded) {
        ps.state = DeviceState::NoFirmware;
        ps.evidence = "usb descriptor manufacturer=\"" + dev->manufacturer + "\" (FX3 bootloader); UHD will load firmware on open";
        ps.attempt_open = true;
    } else if (dev->in_use) {
        ps.state = DeviceState::Fault;
        if (dev->error.find("ACCESS") != std::string::npos || dev->error.find("ermission") != std::string::npos)
            ps.evidence = "permission denied opening USB device (udev rules / group?): " + dev->error;
        else
            ps.evidence = "interface 0 claimed by another handle (another process?): " + dev->error;
    } else if (!dev->queried) {
        ps.state = DeviceState::Fault;
        ps.evidence = "FX3 query failed: " + dev->error;
    } else {
        ps.fx3_state = b2::to_string(dev->state);
        char buf[96];
        std::snprintf(buf, sizeof buf, "fx3_state=%s compat=%u.%u usb=%u", ps.fx3_state.c_str(),
                      dev->compat_num >> 8, dev->compat_num & 0xff, dev->usb_speed);
        ps.evidence = buf;
        switch (dev->state) {
        case b2::fx3_state::running:
            ps.state = DeviceState::Ready;        // FPGA 構成済み、すぐ使える(open へ)
            ps.attempt_open = true;
            break;
        case b2::fx3_state::unconfigured:
        case b2::fx3_state::fpga_ready:
            ps.state = DeviceState::Standby;      // FPGA 未構成。open(make) が書き込む
            ps.attempt_open = true;
            break;
        case b2::fx3_state::configuring_fpga:
        case b2::fx3_state::busy:
            ps.state = DeviceState::Standby;      // 誰かが構成中。待つ
            break;
        case b2::fx3_state::error:
        default:
            ps.state = DeviceState::Fault;
            break;
        }
    }
    set_state(ps.state, ps.evidence, 0);
    return ps;
}

bool Radio::check_live_fx3(std::string* state_str) {
    if (!usrp_) return false;
    try {
        auto tree = usrp_->get_tree();
        if (!tree->exists("/mboards/0/fx3_state_code")) { if (state_str) *state_str = "n/a"; return true; }
        const int code = tree->access<int>("/mboards/0/fx3_state_code").get();
        const auto st = uhd::usrp::b2xx::fx3_state_from_code(static_cast<uint8_t>(code));
        const std::string name = uhd::usrp::b2xx::to_string(st);
        if (state_str) *state_str = name;
        {
            std::lock_guard lk(st_mu_);
            st_.fx3_state = name;
        }
        return st == uhd::usrp::b2xx::fx3_state::running;
    } catch (const std::exception& ex) {
        if (state_str) *state_str = std::string("query failed: ") + ex.what();
        return false;
    }
}

Radio::~Radio() {
    close();
    std::lock_guard lk(g_hook_mu);
    if (g_hook_radio == this) g_hook_radio = nullptr;
    if (g_hook_events == events_) g_hook_events = nullptr;
}

void Radio::stage(int n, const std::string& detail, bool ok) {
    if (!events_) return;
    events_->emit(ok ? EventKind::StartupStage : EventKind::Error, "radio",
                  "stage " + std::to_string(n) + ": " + detail, {}, n);
}

bool Radio::is_open() const { return static_cast<bool>(usrp_); }

void Radio::close() { usrp_.reset(); }

bool Radio::wait_sensor(const std::string& sensor, bool mboard, std::string* err) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(opt_.lock_timeout_s));
    try {
        const auto names = mboard ? usrp_->get_mboard_sensor_names() : usrp_->get_rx_sensor_names();
        if (std::find(names.begin(), names.end(), sensor) == names.end()) return true; // sensor 無し = 確認不能だが失敗ではない
        while (clock::now() < deadline) {
            const auto v = mboard ? usrp_->get_mboard_sensor(sensor) : usrp_->get_rx_sensor(sensor);
            if (v.to_bool()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    } catch (const uhd::exception& e) {
        if (err) *err = sensor + ": " + e.what();
        return false;
    }
    if (err) *err = sensor + " timeout";
    return false;
}

bool Radio::open(const RfConfig& cfg, std::string* err) {
    close();
    std::string e;
    int failed_stage = 1;
    try {
        // NoFirmware なら find() が FW を書き込み、FPGA 未構成なら make() が FPGA を書き込む。
        // その間の状態は probe() 済みの一次ソース + UHD 自身のログ(UhdLog event)で追える。
        // ---- 1. device 列挙 → serial 確認 ----
        uhd::device_addr_t hint("type=b200");
        if (!opt_.expected_serial.empty()) hint["serial"] = opt_.expected_serial;
        const auto found = uhd::device::find(hint);
        if (found.empty()) {
            e = "no B2xx device found" + (opt_.expected_serial.empty() ? std::string() : " (serial=" + opt_.expected_serial + ")");
            stage(1, e, false);
            set_state(DeviceState::Disconnected, "uhd::device::find returned nothing", 0);
            if (err) *err = e;
            return false;
        }
        set_state(DeviceState::Initializing, "uhd::device::find ok; multi_usrp::make (loads FPGA if FX3 unconfigured)", 1);
        uhd::device_addr_t args = found.front();
        if (!opt_.device_args.empty()) {
            uhd::device_addr_t extra(opt_.device_args);
            for (const auto& k : extra.keys()) args[k] = extra[k];
        }
        // 注: uhd::device::make は発見 address のハッシュで生存中の device object を再利用する。
        // 消失した device の object は close() で確実に破棄しなければ再接続できない
        // (素の UHD 4.9 では破棄時に terminate する → packaging/libuhd のパッチが前提)。
        usrp_ = uhd::usrp::multi_usrp::make(args);
        info_.serial  = args.has_key("serial") ? args["serial"] : "?";
        info_.product = args.has_key("product") ? args["product"] : "?";
        info_.name    = args.has_key("name") ? args["name"] : "";
        stage(1, "device found serial=" + info_.serial + " product=" + info_.product +
                 (info_.name.empty() ? "" : " name=" + info_.name), true);

        // ---- 2. FPGA image / FW version 確認 ----
        failed_stage = 2;
        auto tree = usrp_->get_tree();
        if (tree->exists("/mboards/0/fpga_version")) info_.fpga_version = tree->access<std::string>("/mboards/0/fpga_version").get();
        if (tree->exists("/mboards/0/fw_version"))   info_.fw_version   = tree->access<std::string>("/mboards/0/fw_version").get();
        info_.pp_string = usrp_->get_pp_string();
        if (tree->exists("/mboards/0/usb_version")) info_.usb_version = tree->access<int>("/mboards/0/usb_version").get();
        std::string fx3;
        if (!check_live_fx3(&fx3)) {
            e = "FX3 not running after make: fx3_state=" + fx3;
            stage(2, e, false);
            set_state(DeviceState::Fault, e, 2);
            if (err) *err = e;
            usrp_.reset();
            return false;
        }
        stage(2, "fpga=" + info_.fpga_version + " fw=" + info_.fw_version + " usb=" + std::to_string(info_.usb_version) + " fx3=" + fx3, true);
        if (!opt_.expected_fw_version.empty() && opt_.expected_fw_version != info_.fw_version && events_)
            events_->emit(EventKind::Warning, "radio", "fw version " + info_.fw_version + " != expected " + opt_.expected_fw_version);
        if (!opt_.expected_fpga_version.empty() && opt_.expected_fpga_version != info_.fpga_version && events_)
            events_->emit(EventKind::Warning, "radio", "fpga version " + info_.fpga_version + " != expected " + opt_.expected_fpga_version);
        // USB link 帯域と要求 rate の照合(sc16 = 4 B/sample)。維持できない構成は開始前に止める。
        if (tree->exists("/mboards/0/link_max_rate")) {
            const double link = tree->access<double>("/mboards/0/link_max_rate").get();
            const double need = cfg.sample_rate * 4.0;
            char lb[160];
            std::snprintf(lb, sizeof lb, "link budget: %.1f MB/s of %.1f MB/s (USB %d) = %.0f%%", need / 1e6, link / 1e6,
                          info_.usb_version, 100.0 * need / link);
            if (need > opt_.link_budget_fault * link) {
                e = lb;
                stage(2, e, false);
                set_state(DeviceState::Fault, e, 2);
                if (err) *err = e;
                usrp_.reset();
                return false;
            }
            if (events_) events_->emit(need > opt_.link_budget_warn * link ? EventKind::Warning : EventKind::Info, "radio", lb);
        }
        set_state(DeviceState::Initializing, "make ok; fx3_state=" + fx3 + " fpga=" + info_.fpga_version + " fw=" + info_.fw_version, 2);

        // ---- 3. clock source → ref_locked ----
        failed_stage = 3;
        clock_source_ = cfg.clock_source;
        usrp_->set_clock_source(cfg.clock_source);
        // B2xx の ref_locked は外部 10 MHz 基準 PLL (ADF4001) のロック状態。internal では unlocked が正常。
        // external / gpsdo のときだけ必須条件にし、internal では値を報告するに留める。
        if (cfg.clock_source == "internal") {
            std::string ref = "n/a";
            try { ref = usrp_->get_mboard_sensor("ref_locked").to_pp_string(); } catch (...) {}
            stage(3, "clock_source=internal (TCXO, ref_locked not applicable: " + ref + ")", true);
            return true;
        }
        if (!wait_sensor("ref_locked", true, &e)) {
            stage(3, e, false);
            set_state(DeviceState::Fault, "sensor ref_locked: " + e, 3);
            if (err) *err = e;
            return false;
        }
        stage(3, "clock_source=" + cfg.clock_source + " ref_locked", true);
        return true;
    } catch (const uhd::exception& ex) {
        e = std::string("uhd: ") + ex.what();
    } catch (const std::exception& ex) {
        e = ex.what();
    }
    stage(failed_stage, e, false);
    set_state(DeviceState::Fault, "stage " + std::to_string(failed_stage) + ": " + e, failed_stage);
    if (err) *err = e;
    usrp_.reset();
    return false;
}

bool Radio::tune_rx(const RfConfig& cfg, std::string* err) {
    if (!usrp_) { if (err) *err = "not open"; return false; }
    std::string e;
    const LoCorrection corr = lo_correction();   // 個体の LO 誤差: 要求は装置の目盛りへ、読み値は真の周波数へ
    try {
        // ---- 宣言 (§8.1) を装置の能力範囲と照合。範囲外は「適用不能」として失敗させる(黙って丸めない)----
        {
            const auto fr = usrp_->get_rx_freq_range();
            const auto gr = usrp_->get_rx_gain_range();
            const auto rr = usrp_->get_rx_rates();
            const auto ants = usrp_->get_rx_antennas();
            char rb[200];
            if (corr.to_device(cfg.center_freq) < fr.start() || corr.to_device(cfg.center_freq) > fr.stop()) {
                std::snprintf(rb, sizeof rb, "center_freq %.6f MHz outside [%.3f, %.3f] MHz", cfg.center_freq / 1e6, fr.start() / 1e6, fr.stop() / 1e6);
                throw uhd::value_error(rb);
            }
            if (cfg.gain < gr.start() || cfg.gain > gr.stop()) {
                std::snprintf(rb, sizeof rb, "gain %.1f dB outside [%.1f, %.1f] dB", cfg.gain, gr.start(), gr.stop());
                throw uhd::value_error(rb);
            }
            if (cfg.sample_rate < rr.start() || cfg.sample_rate > rr.stop()) {
                std::snprintf(rb, sizeof rb, "sample_rate %.6g outside [%.6g, %.6g]", cfg.sample_rate, rr.start(), rr.stop());
                throw uhd::value_error(rb);
            }
            if (!cfg.antenna.empty() && std::find(ants.begin(), ants.end(), cfg.antenna) == ants.end()) {
                std::string list;
                for (const auto& a : ants) list += (list.empty() ? "" : ",") + a;
                throw uhd::value_error("antenna " + cfg.antenna + " not in {" + list + "}");
            }
        }
        usrp_->set_rx_rate(cfg.sample_rate);
        actual_rate_ = usrp_->get_rx_rate();
        const auto res = usrp_->set_rx_freq(uhd::tune_request_t(corr.to_device(cfg.center_freq)));
        actual_freq_ = corr.to_true(usrp_->get_rx_freq());
        usrp_->set_rx_gain(cfg.gain);
        usrp_->set_rx_agc(cfg.agc);   // AD9361 の AGC。ON なら gain は装置が決める(以後 get_rx_gain は実測値)
        const double actual_gain = usrp_->get_rx_gain();
        if (!cfg.antenna.empty()) usrp_->set_rx_antenna(cfg.antenna);
        usrp_->set_rx_bandwidth(cfg.bandwidth > 0 ? cfg.bandwidth : cfg.sample_rate);
        const double actual_bw = usrp_->get_rx_bandwidth();
        // ---- coercion 検出: 要求と実際の差が許容を超えたら失敗(provenance の前提が崩れる)----
        {
            char cb[200];
            if (std::abs(actual_rate_ - cfg.sample_rate) > opt_.rate_tolerance_rel * cfg.sample_rate) {
                std::snprintf(cb, sizeof cb, "sample_rate coerced: requested %.6f, actual %.6f Msps", cfg.sample_rate / 1e6, actual_rate_ / 1e6);
                throw uhd::value_error(cb);
            }
            if (std::abs(actual_freq_ - cfg.center_freq) > opt_.freq_tolerance_hz) {
                std::snprintf(cb, sizeof cb, "center_freq coerced: requested %.6f, actual %.6f MHz", cfg.center_freq / 1e6, actual_freq_ / 1e6);
                throw uhd::value_error(cb);
            }
            if (!cfg.agc && std::abs(actual_gain - cfg.gain) > opt_.gain_tolerance_db) {
                std::snprintf(cb, sizeof cb, "gain coerced: requested %.1f, actual %.1f dB", cfg.gain, actual_gain);
                throw uhd::value_error(cb);
            }
            const double want_bw = cfg.bandwidth > 0 ? cfg.bandwidth : cfg.sample_rate;
            if (std::abs(actual_bw - want_bw) > 0.25 * want_bw && events_) {
                std::snprintf(cb, sizeof cb, "bandwidth coerced: requested %.3f, actual %.3f MHz", want_bw / 1e6, actual_bw / 1e6);
                events_->emit(EventKind::Warning, "radio", cb);
            }
        }
        if (!wait_sensor("lo_locked", false, &e)) {
            stage(4, e, false);
            set_state(DeviceState::Fault, "sensor lo_locked: " + e, 4);
            if (err) *err = e;
            return false;
        }
        char buf[200];
        std::snprintf(buf, sizeof buf, "rate=%.6g (mcr=%.6g) freq=%.6f MHz (rf=%.6f dsp=%.1f corr=%+.2fppm/%+.0fHz) gain=%.1f%s ant=%s lo_locked",
                      actual_rate_, usrp_->get_master_clock_rate(), actual_freq_ / 1e6, res.actual_rf_freq / 1e6,
                      res.actual_dsp_freq, corr.ppm, corr.error_hz(cfg.center_freq), usrp_->get_rx_gain(), cfg.agc ? " (AGC)" : "", usrp_->get_rx_antenna().c_str());
        stage(4, buf, true);
        set_state(DeviceState::Ready, std::string("sensors: lo_locked=true; ") + buf, 4);
        return true;
    } catch (const uhd::exception& ex) {
        e = std::string("uhd: ") + ex.what();
    } catch (const std::exception& ex) {
        e = ex.what();
    }
    stage(4, e, false);
    set_state(DeviceState::Fault, "stage 4: " + e, 4);
    if (err) *err = e;
    return false;
}

double Radio::retune_rx_at(double freq, const uhd::time_spec_t& at) {
    const LoCorrection corr = lo_correction();
    usrp_->set_command_time(at);
    usrp_->set_rx_freq(uhd::tune_request_t(corr.to_device(freq)));
    usrp_->clear_command_time();
    actual_freq_ = corr.to_true(usrp_->get_rx_freq());
    return actual_freq_;
}

uhd::rx_streamer::sptr Radio::make_rx_streamer(std::string* err) {
    try {
        uhd::stream_args_t sa("sc16", "sc16"); // native wire format (§1.1)
        sa.channels = {0};
        return usrp_->get_rx_stream(sa);
    } catch (const uhd::exception& ex) {
        if (err) *err = std::string("uhd: ") + ex.what();
        return nullptr;
    }
}

uhd::time_spec_t Radio::now() const { return usrp_->get_time_now(); }

} // namespace spear

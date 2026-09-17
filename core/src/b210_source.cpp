#include "spear/core/b210_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <uhd/exception.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/utils/thread.hpp>

namespace spear {

B210LiveSource::B210LiveSource(EventBus* events, B210SourceOptions opt)
    : events_(events), opt_(std::move(opt)), radio_(events, opt_.radio),
      pool_(opt_.block_samples * sizeof(sc16), opt_.pool_blocks) {
    StreamMeta m;
    m.id = kRadioRxStreamId;
    m.dtype = DataType::ComplexInt16;
    m.unit = "FS";
    m.direction = Direction::RX;
    stream_ = std::make_unique<Stream<sc16>>(m, events_);
}

B210LiveSource::~B210LiveSource() { stop(); }

bool B210LiveSource::configure(const RfConfig& cfg, std::string* err) {
    if (running_) { if (err) *err = "cannot configure while running"; return false; }
    if (cfg.sample_rate < 200e3 || cfg.sample_rate > 56e6) { if (err) *err = "sample_rate out of range"; return false; }
    {
        std::lock_guard lk(cfg_mu_);
        cfg_ = cfg;
    }
    state_version_++;
    StreamMeta m = stream_->meta();
    m.sample_rate = cfg.sample_rate;
    m.center_freq = cfg.center_freq;
    m.bandwidth   = cfg.bandwidth > 0 ? cfg.bandwidth : cfg.sample_rate;
    if (stream_->consumer_count() == 0) stream_ = std::make_unique<Stream<sc16>>(m, events_);
    return true;
}

void B210LiveSource::start() {
    if (running_.exchange(true)) return;
    // 立ち上げが進行中なら完了を待つ(open の途中で RX thread が同じ Radio を触らないように)
    warm_cancel_ = true;
    if (warm_th_.joinable()) warm_th_.join();
    stop_req_ = false;
    th_ = std::thread([this] { run(); });
}

void B210LiveSource::stop() {
    stop_req_ = true;
    warm_cancel_ = true;
    if (warm_th_.joinable()) warm_th_.join();
    if (th_.joinable()) th_.join();
    running_ = false;
}

void B210LiveSource::report(const std::string& line) {
    { std::lock_guard lk(mu_); report_.push_back(line); }
    if (events_) events_->emit(EventKind::Info, "radio", "warm-up: " + line);
}

void B210LiveSource::warm_up() {
    if (running_) return;
    if (warming_.exchange(true)) return;
    if (warm_th_.joinable()) warm_th_.join();
    warm_cancel_ = false;
    { std::lock_guard lk(mu_); report_.clear(); }
    warm_th_ = std::thread([this] { warm_run(); warming_ = false; });
}

// 立ち上げ: App を始める前に装置を「すぐ使える」状態にする。
// UHD は multi_usrp::make() の中で FPGA を書き込む(ストリームを始める必要はない)ので、open → close で FPGA は構成済みのまま残り、
// 次の open(App 起動)はハッシュ照合で書き込みを飛ばして数秒で済む。
void B210LiveSource::warm_run() {
    auto sleep_s = [&](double sec) {
        for (int i = 0; i < 20 && !warm_cancel_ && !stop_req_; ++i)
            std::this_thread::sleep_for(std::chrono::duration<double>(sec / 20));
    };
    // stage 0: 装置に触らない自己診断
    const auto sc = radio_.self_check();
    report("UHD " + sc.uhd_version);
    report(std::string("FW image: ") + (sc.fw_image.empty() ? "MISSING" : sc.fw_image));
    report(std::string("FPGA image: ") + (sc.fpga_image.empty() ? "MISSING" : sc.fpga_image));
    report("RLIMIT_RTPRIO=" + std::to_string(sc.rtprio_limit) + (sc.rtprio_limit == 0 ? " (SCHED_FIFO unavailable)" : "") +
           "  RLIMIT_MEMLOCK=" + (sc.memlock_limit_kb < 0 ? std::string("unlimited") : std::to_string(sc.memlock_limit_kb) + " kB"));
    for (const auto& n : sc.notes) if (n.rfind("RLIMIT_RTPRIO", 0) != 0) report("note: " + n);   // rtprio は上の行に出ている
    if (!sc.ok) { report("self-check FAILED: cannot open the device"); return; }
    report("self-check ok");
    // 装置の出現待ち(副作用なしの probe)
    bool announced = false;
    ProbeSummary ps;
    while (!warm_cancel_ && !stop_req_) {
        ps = radio_.probe();
        if (ps.attempt_open) break;
        if (!announced) { report("waiting for device: " + ps.evidence); announced = true; }
        sleep_s(ps.state == DeviceState::Fault ? opt_.fault_retry_s : opt_.probe_period_s);
    }
    if (warm_cancel_ || stop_req_) { report("skipped (device wait cancelled)"); return; }
    report("device present: serial=" + ps.serial + "  " + ps.evidence);
    // open: NoFirmware なら FW、FPGA 未構成なら FPGA を UHD が書き込む(進捗は DeviceStatus::progress_pct)
    const RfConfig cfg = config();
    report(ps.state == DeviceState::Ready ? "opening device (FPGA already configured)" : "opening device: loading FPGA (about 70 s on USB 2.0) ...");
    std::string err;
    if (!radio_.open(cfg, &err)) { report("open FAILED: " + err); radio_.close(); return; }
    const auto& info = radio_.info();
    report("open ok: " + info.product + " serial=" + info.serial + "  fw=" + info.fw_version + "  fpga=" + info.fpga_version + "  USB " + std::to_string(info.usb_version));
    // tune 確認(LO ロック)。運転の RF は App が決めるので、ここは草案での動作確認だけ
    char buf[128];
    std::snprintf(buf, sizeof buf, "tune check: %.6f MHz @ %.3f Msps gain %.1f dB", cfg.center_freq / 1e6, cfg.sample_rate / 1e6, cfg.gain);
    if (!radio_.tune_rx(cfg, &err)) report(std::string(buf) + " FAILED: " + err);
    else report(std::string(buf) + " lo_locked");
    radio_.close();
    report("closed; FPGA stays configured. ready");
    radio_.probe();   // 状態は一次ソース(FX3 レジスタ)から: running → READY
}

void B210LiveSource::request_retune(double freq_hz) {
    {
        std::lock_guard lk(cfg_mu_);
        cfg_.center_freq = freq_hz;     // 宣言は即時。適用は RX thread が Retune event で報告
    }
    state_version_++;
    retune_req_.store(freq_hz);
}

B210RxStats B210LiveSource::stats() const {
    std::lock_guard lk(mu_);
    auto s = st_;
    s.generation = generation_.load();
    return s;
}

bool B210LiveSource::bring_up() {
    std::string err;
    auto set_stage = [&](int s, const std::string& e) {
        std::lock_guard lk(mu_);
        st_.startup_stage = s;
        if (!e.empty()) st_.last_error = e;
    };
    const RfConfig cfg = config();
    if (!radio_.open(cfg, &err)) { set_stage(0, err); return false; }
    set_stage(3, "");
    if (!radio_.tune_rx(cfg, &err)) { set_stage(3, err); return false; }
    set_stage(4, "");
    rx_ = radio_.make_rx_streamer(&err);
    if (!rx_) { set_stage(4, err); return false; }
    try {
        // stream metadata は実際に適用された rate / freq で確定する
        if (stream_->consumer_count() == 0) {
            StreamMeta m = stream_->meta();
            m.sample_rate = radio_.actual_rx_rate();
            m.center_freq = radio_.actual_rx_freq();
            stream_ = std::make_unique<Stream<sc16>>(m, events_);
        }
        uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        cmd.stream_now = true;
        rx_->issue_stream_cmd(cmd);
    } catch (const uhd::exception& ex) {
        set_stage(4, std::string("uhd: ") + ex.what());
        if (events_) events_->emit(EventKind::Error, "radio", std::string("stage 5: ") + ex.what(), {}, 5);
        return false;
    }
    set_stage(5, "");
    if (events_) events_->emit(EventKind::StartupStage, "radio", "stage 5: streaming, settling " + std::to_string(opt_.radio.settle_blocks) + " blocks", {}, 5);
    return true;
}

void B210LiveSource::tear_down(bool device_lost) {
    const bool anything = rx_ || radio_.is_open();
    auto note = [&](const char* what) { if (events_ && anything) events_->emit(EventKind::Info, "radio", std::string("tear_down: ") + what); };
    if (!device_lost) {
        // 正常停止: stream を止めてから破棄
        try {
            if (rx_) rx_->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
        } catch (const std::exception& ex) {
            if (events_) events_->emit(EventKind::Warning, "radio", std::string("tear_down: stop cmd failed: ") + ex.what());
        }
    } else {
        // device 消失: USB へ何か送ろうとする操作は全て失敗する。stop cmd は送らない。
        note("device lost, skipping stop cmd");
    }
    note("resetting rx streamer");
    try { rx_.reset(); } catch (const std::exception& ex) {
        if (events_) events_->emit(EventKind::Warning, "radio", std::string("tear_down: streamer reset threw: ") + ex.what());
    }
    note("closing radio");
    radio_.close();   // device 消失時: パッチ済み libuhd なら timeout で抜ける(packaging/libuhd)
    note("done");
    std::lock_guard lk(mu_);
    st_.streaming = false;
}

void B210LiveSource::run() {
    if (opt_.realtime_priority && !uhd::set_thread_priority_safe(1.0, true) && events_)
        events_->emit(EventKind::Warning, "radio", "set_thread_priority_safe failed (rtprio not permitted?)");

    bool first_up = true;
    radio_.self_check();   // stage 0: image files / rlimit / UHD version(装置に触らない)
    auto sleep_s = [&](double sec) {
        for (int i = 0; i < 20 && !stop_req_; ++i)
            std::this_thread::sleep_for(std::chrono::duration<double>(sec / 20));
    };
    while (!stop_req_) {
        // ---- 待機: 副作用なしの probe で「open を試みてよい状態」になるまで待つ ----
        // (uhd::device::find は FW 書き込みの副作用があるので存在確認には使わない)
        {
            const auto ps = radio_.probe();
            if (!ps.attempt_open) {
                sleep_s(ps.state == DeviceState::Fault ? opt_.fault_retry_s : opt_.probe_period_s);
                continue;
            }
        }
        // ---- 起動 / 再接続 ----
        if (!bring_up()) {
            // 起動失敗(lock timeout 等)は device 消失ではない → 通常の close。状態は open() 内で Fault 済み
            tear_down(false);
            if (!first_up && events_) events_->emit(EventKind::Disconnected, "radio", "reconnect failed, retrying");
            sleep_s(opt_.reconnect_backoff_s);
            continue;
        }
        if (!first_up) {
            // 再接続成功: 時間軸が切れたので generation を進める (§4.4.1)
            ++generation_;
            std::lock_guard lk(mu_);
            st_.reconnects++;
        }
        if (!first_up && events_)
            events_->emit(EventKind::Reconnected, "radio", "generation advanced", {}, static_cast<int64_t>(generation_.load()));
        radio_.set_generation(generation_.load());
        first_up = false;
        sensor_stop_ = false;
        sensor_th_ = std::thread([this] {
            bool lo_was_locked = true;
            while (!sensor_stop_) {
                for (int i = 0; i < 10 && !sensor_stop_; ++i)
                    std::this_thread::sleep_for(std::chrono::duration<double>(opt_.sensor_period_s / 10));
                if (sensor_stop_) break;
                const auto ls = radio_.read_live_sensors();
                if (!ls.valid) continue;
                if (ls.lo_locked != lo_was_locked) {
                    lo_was_locked = ls.lo_locked;
                    if (events_) events_->emit(ls.lo_locked ? EventKind::LoUnlock : EventKind::LoUnlock, "radio",
                                               ls.lo_locked ? "lo_locked restored" : "LO UNLOCKED while streaming (data invalid)",
                                               {}, ls.lo_locked ? 1 : 0);
                }
            }
        });


        const double rate = radio_.actual_rx_rate();
        // time_spec は FPGA の master clock tick から作られる double。sample rate で直接 to_ticks() すると
        // 丸め境界で ±1 sample のジッタが出て偽の discontinuity になる。master clock の tick(正確な整数)で
        // 取り、decimation 比で割って sample index にする。
        double mcr = rate;
        try { mcr = radio_.usrp()->get_master_clock_rate(); } catch (...) {}
        const int64_t decim = std::max<int64_t>(1, std::llround(mcr / rate));
        const bool exact_ratio = std::abs(mcr / rate - static_cast<double>(decim)) < 1e-6;
        if (!exact_ratio && events_)
            events_->emit(EventKind::Warning, "radio", "master clock / sample rate is not integer; sample index may jitter", {}, decim);
        auto sample_ticks = [&](const uhd::time_spec_t& ts) -> int64_t {
            const int64_t t = ts.to_ticks(mcr);
            return exact_ratio ? t : std::llround(static_cast<double>(t) / static_cast<double>(decim));
        };
        auto ticks_to_index = [&](int64_t t) -> uint64_t {
            return exact_ratio ? static_cast<uint64_t>(t / decim) : static_cast<uint64_t>(t);
        };
        const std::size_t block_n = opt_.block_samples;
        uhd::rx_metadata_t md;
        std::size_t settle = opt_.radio.settle_blocks;
        int consecutive_timeouts = 0;
        bool device_lost = false;
        bool have_base = false;
        int64_t base_ticks = 0;         // generation 原点 (time_spec tick)
        uint64_t expected_index = 0;    // 次 block の先頭 index (連続なら)
        Flags pending;                  // 次に配送する block へ合成する flag
        std::optional<int64_t> retune_tick; double retune_freq = 0;
        bool stage6 = false;
        {
            std::lock_guard lk(mu_);
            st_.streaming = true;
        }

        // ---- 定常受信 ----
        while (!stop_req_) {
            // retune 要求: 前の境界が確定してから取り出す(取り出せない間は要求を保持 = 失われない)。
            // 少し先の時刻を指定し、その tick を境界とする。
            if (!retune_tick) {
                double f = retune_req_.load();
                if (f > 0 && retune_req_.compare_exchange_strong(f, 0.0)) {
                    try {
                        const auto at = radio_.now() + uhd::time_spec_t(0.05);
                        retune_freq = radio_.retune_rx_at(f, at);
                        retune_tick = sample_ticks(at);
                    } catch (const uhd::exception& ex) {
                        if (events_) events_->emit(EventKind::Error, "radio", std::string("retune: ") + ex.what());
                    }
                }
            }

            auto b = pool_.acquire<sc16>(block_n);
            std::size_t n = 0;
            try {
                n = rx_->recv(b.data<sc16>().data(), block_n, md, opt_.recv_timeout_s, false);
            } catch (const uhd::exception& ex) {
                // USB 切断等。App は落とさず再接続へ。
                {
                    std::lock_guard lk(mu_);
                    st_.last_error = ex.what();
                }
                if (events_) events_->emit(EventKind::Disconnected, "radio", ex.what(),
                                           {generation_.load(), expected_index, expected_index});
                radio_.set_state(DeviceState::Lost, std::string("recv threw: ") + ex.what(), 0);
                device_lost = true;
                break;
            }

            // ---- metadata → flags / events(区別したまま)----
            Flags f;
            SampleRange here{generation_.load(), expected_index, expected_index};
            bool reinit = false;
            {
                std::lock_guard lk(mu_);
                switch (md.error_code) {
                case uhd::rx_metadata_t::ERROR_CODE_NONE:
                    consecutive_timeouts = 0;
                    break;
                case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
                    st_.timeout++;
                    f.set(Flag::Timeout);
                    if (++consecutive_timeouts >= opt_.watchdog_timeouts) { st_.watchdog_resets++; reinit = true; }
                    break;
                case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
                    st_.overflow++;
                    f.set(Flag::Overflow).set(Flag::Discontinuity);
                    break;
                case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
                    st_.late_command++;
                    break;
                case uhd::rx_metadata_t::ERROR_CODE_BROKEN_CHAIN:
                    st_.broken_chain++;
                    f.set(Flag::Discontinuity);
                    break;
                case uhd::rx_metadata_t::ERROR_CODE_ALIGNMENT:
                    st_.alignment++;
                    f.set(Flag::Discontinuity);
                    break;
                case uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET:
                    st_.bad_packet++;
                    f.set(Flag::Discontinuity);
                    break;
                }
                if (md.out_of_sequence) {
                    st_.out_of_sequence++;
                    f.set(Flag::OutOfSequence).set(Flag::Discontinuity);
                }
            }
            std::string fx3_now;
            bool fx3_running = true;
            if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
                // データが来ない = 既に異常。ここで初めて FX3 状態レジスタ(一次ソース)を読んで原因を分類する
                fx3_running = radio_.check_live_fx3(&fx3_now);
            }
            if (events_) {
                if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
                    events_->emit(EventKind::Overflow, "radio", "host read too slowly (ERROR_CODE_OVERFLOW)", here);
                else if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT)
                    events_->emit(EventKind::Timeout, "radio", "recv timeout; fx3_state=" + fx3_now, here, consecutive_timeouts);
                else if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE)
                    events_->emit(EventKind::Error, "radio", "recv: " + md.strerror(), here, md.error_code);
                if (md.out_of_sequence)
                    events_->emit(EventKind::OutOfSequence, "radio", "transport packet loss (USB?)", here);
            }
            if (!fx3_running) {
                // FX3 が running でない(error / unconfigured 等): USB は生きているが装置側の異常
                radio_.set_state(DeviceState::Fault, "fx3_state=" + fx3_now + " while streaming (recv timeout)", 6);
                if (events_) events_->emit(EventKind::Error, "radio", "FX3 not running while streaming: " + fx3_now, here);
                device_lost = true;
                break;
            }
            if (reinit) {
                if (events_) events_->emit(EventKind::Disconnected, "radio", "watchdog: recv timeouts, reinitializing", here);
                radio_.set_state(DeviceState::Lost, "watchdog: " + std::to_string(consecutive_timeouts) +
                                 " consecutive recv timeouts; fx3_state=" + fx3_now, 0);
                device_lost = true;
                break;
            }
            pending |= f;
            if (n == 0) continue;

            // ---- settling: 最初の N block は捨てる(§17.2 手順5)----
            if (settle > 0) { --settle; continue; }
            if (!stage6) {
                stage6 = true;
                std::lock_guard lk(mu_);
                st_.startup_stage = 6;
                if (events_) events_->emit(EventKind::StartupStage, "radio", "stage 6: steady state", {}, 6);
                radio_.set_state(DeviceState::Streaming, "rx streamer delivering, settling done", 6);
                state_version_++;
            }

            // ---- 時間軸: time_spec tick → sample index ----
            auto& h = b.header();
            if (md.has_time_spec) {
                const int64_t ticks = sample_ticks(md.time_spec);
                if (!have_base) {
                    have_base = true;
                    base_ticks = ticks;
                    pending.set(Flag::StartOfBurst);
                    // 時刻基準 (§4.4): hardware time ↔ host monotonic ↔ UTC の対応を 1 点記録。
                    // block の time_spec は USB バッファ遅延分だけ host 受信時刻より古いので、
                    // get_time_now() の往復中点で対応付ける(1 回だけ、~1 ms の EP4 要求。転送は止めない)。
                    try {
                        TimeReference tr;
                        tr.generation = generation_.load();
                        const uint64_t t0 = host_now_ns();
                        const auto utc0 = std::chrono::system_clock::now();
                        const auto hw = radio_.now();
                        const uint64_t t1 = host_now_ns();
                        tr.round_trip_ns = t1 - t0;
                        tr.host_mono_ns = t0 + tr.round_trip_ns / 2;
                        tr.utc_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            utc0.time_since_epoch()).count()) + tr.round_trip_ns / 2;
                        tr.hw_time_s = hw.get_real_secs();
                        tr.hw_sample_index = ticks_to_index(sample_ticks(hw) - base_ticks);
                        const std::string clk = config().clock_source;
                        tr.accuracy = clk == "internal" ? "TCXO ~2ppm, no GPSDO" : "external reference: " + clk;
                        tr.valid = true;
                        radio_.set_time_reference(tr);
                    } catch (const std::exception& ex) {
                        if (events_) events_->emit(EventKind::Warning, "radio", std::string("time reference failed: ") + ex.what());
                    }
                } else if (ticks < base_ticks) {
                    // 時刻が巻き戻った = clock reset。generation を進める。
                    ++generation_;
                    base_ticks = ticks;
                    expected_index = 0;
                    pending.set(Flag::ClockReset).set(Flag::Discontinuity);
                    if (events_) events_->emit(EventKind::ClockReset, "radio", "time_spec went backwards", {}, static_cast<int64_t>(generation_.load()));
                }
                const uint64_t idx = ticks_to_index(ticks - base_ticks);
                if (idx != expected_index && expected_index != 0) {
                    // UHD が overflow を報告しなくても time_spec のギャップは欠落。無言にしない。
                    pending.set(Flag::Discontinuity);
                    {
                        std::lock_guard lk(mu_);
                        st_.discontinuities++;
                        if (idx > expected_index) st_.missing_samples += idx - expected_index;
                    }
                    if (events_) events_->emit(EventKind::Discontinuity, "radio", "sample index gap",
                                               {generation_.load(), expected_index, idx},
                                               static_cast<int64_t>(idx) - static_cast<int64_t>(expected_index));
                }
                h.sample_index = idx;
                h.hw_time = {md.time_spec.get_full_secs(), md.time_spec.get_frac_secs(), true};
            } else {
                h.sample_index = expected_index;
            }
            if (retune_tick && md.has_time_spec && sample_ticks(md.time_spec) + static_cast<int64_t>(n) * decim > *retune_tick) {
                pending.set(Flag::Retune);
                const uint64_t at_idx = ticks_to_index(std::max<int64_t>(*retune_tick - base_ticks, 0));
                if (events_) events_->emit(EventKind::Retune, "radio", "center frequency changed",
                                           {generation_.load(), at_idx, at_idx}, static_cast<int64_t>(std::llround(retune_freq)));
                retune_tick.reset();
                state_version_++;
            }
            h.generation = generation_.load();
            h.sequence = sequence_++;
            h.host_ns = host_now_ns();
            h.flags = pending;
            pending = {};
            expected_index = h.sample_index + n;
            {
                std::lock_guard lk(mu_);
                st_.blocks++;
                st_.samples += n;
            }
            stream_->publish(b.commit(static_cast<uint32_t>(n)));
        }
        sensor_stop_ = true;
        if (sensor_th_.joinable()) sensor_th_.join();
        tear_down(device_lost);
    }
    // stop: device は閉じたので、一次ソースで状態を取り直してから EOS
    radio_.probe();
    {
        auto b = pool_.acquire<sc16>(1);
        b.header().flags.set(Flag::EndOfStream);
        b.header().generation = generation_.load();
        b.header().sequence = sequence_++;
        stream_->publish(b.commit(0));
        stream_->end();
    }
    running_ = false;
}

} // namespace spear

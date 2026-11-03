#include "replay_app.hpp"

#include "spear/core/tap.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <filesystem>

namespace spear::apps {

using namespace std::chrono_literals;

namespace {
// 送信キュー(kDefaultTxQueueBlocks)+ 表示の LatestOnly(2)+ 書き込み中 1 + 余裕。足りなければ BlockPool は heap に落ちる(統計に出る)
constexpr std::size_t kPoolBlocks = kDefaultTxQueueBlocks + 8;
} // namespace

ReplayApp::ReplayApp(appfw::AppInfo info, QObject* parent)
    : appfw::GuiApp(std::move(info), parent), pool_(kDefaultBlockSamples * sizeof(sc16), kPoolBlocks) {
    // 送信ゲインは保存しない(起動のたびに 0 dB から。意図しない出力で始めない)
    persist({"file", "loop", "txPort", "view.dbMin", "view.dbMax", "view.manualRange", "view.averaging"});
}

ReplayApp::~ReplayApp() {
    stop_burst_ = true;
    join_burst();
}

void ReplayApp::configure(const QVariantMap& settings) {
    dirs_.clear();
    if (settings.contains("record_dir")) dirs_ << settings["record_dir"].toString();
    // 録音以外の置き場所(golden 等)。site.conf の replay.dirs=<dir>:<dir>
    if (settings.contains("replay.dirs"))
        for (const auto& d : settings["replay.dirs"].toString().split(':', Qt::SkipEmptyParts)) dirs_ << d;
    refreshRecordings();
}

void ReplayApp::refreshRecordings() {
    struct Row { QString base; QDateTime mtime; QVariantMap m; };
    std::vector<Row> rows;
    for (const auto& dir : dirs_) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(dir.toStdString(), ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file() || it->path().extension() != ".sigmf-meta") continue;
            std::string base = it->path().string();
            base.resize(base.size() - std::string(".sigmf-meta").size());
            sigmf::Meta meta;
            if (!sigmf::read(base, meta)) continue;
            std::error_code ec2;
            const auto bytes = std::filesystem::file_size(sigmf::data_path(base), ec2);
            const double rate = meta.sample_rate;
            const double secs = !ec2 && rate > 0 && dtype_size(meta.dtype) ? static_cast<double>(bytes / dtype_size(meta.dtype)) / rate : 0;   // bytes / 要素の大きさ = sample 数(整数)
            const double freq = meta.captures.empty() ? 0 : meta.captures.front().frequency;
            Row r;
            r.base = QString::fromStdString(base);
            r.mtime = QFileInfo(QString::fromStdString(it->path().string())).lastModified();
            r.m["label"] = QFileInfo(r.base).fileName();
            r.m["sublabel"] = QString("%1 MHz   %2 Msps   %3 s   %4   %5")
                                  .arg(freq / 1e6, 0, 'f', 6).arg(rate / 1e6, 0, 'f', 3).arg(secs, 0, 'f', 1)
                                  .arg(r.mtime.toString("yyyy-MM-dd HH:mm")).arg(QFileInfo(r.base).dir().dirName());
            r.m["value"] = r.base;
            rows.push_back(std::move(r));
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.mtime > b.mtime; });   // 新しい順
    recordings_.clear();
    for (auto& r : rows) recordings_.push_back(r.m);
    Q_EMIT recordingsChanged();
}

void ReplayApp::setFile(const QString& base) {
    if (base == file_ && file_valid_) return;
    file_ = base;
    load_file_info();
    Q_EMIT fileChanged();
}

void ReplayApp::load_file_info() {
    file_valid_ = false;
    file_error_.clear();
    file_freq_ = file_rate_ = 0;
    file_samples_ = 0;
    file_disc_ = file_retunes_ = 0;
    if (file_.isEmpty()) { file_error_ = "no recording selected"; return; }
    SigmfReader r;
    std::string err;
    if (!r.open(file_.toStdString(), &err)) { file_error_ = QString::fromStdString(err); return; }
    const auto& m = r.meta();
    if (m.sample_rate <= 0 || m.captures.empty()) { file_error_ = "recording has no sample rate / capture frequency"; return; }
    file_rate_ = m.sample_rate;
    file_freq_ = m.captures.front().frequency;
    file_samples_ = r.total_samples();
    file_disc_ = static_cast<int>(m.discontinuities.size());
    file_retunes_ = static_cast<int>(m.captures.size()) - 1;
    file_valid_ = file_samples_ > 0;
    if (!file_valid_) file_error_ = "recording is empty";
}

QString ReplayApp::txPort() const {
    const auto n = rf_port_name(tx_port_);
    return QString::fromUtf8(n.data(), static_cast<qsizetype>(n.size()));
}

void ReplayApp::setTxPort(const QString& name) {
    RfPort p;
    if (!parse_rf_port(name.toStdString(), &p) || !rf_port_can_tx(p) || p == tx_port_) return;   // 受信専用端子は受けない
    tx_port_ = p;
    Q_EMIT txPortChanged();
}

// 録音の center / rate で送信を宣言する。録音が無ければ草案のまま(TRANSMIT は押せない。burst の外では何も出ない)
RfConfig ReplayApp::declare_rf_config() const {
    RfConfig c = GuiApp::declare_rf_config();
    if (file_valid_) {
        c.center_freq = file_freq_;
        c.sample_rate = file_rate_;
        c.bandwidth = 0;
    }
    c.tx_gain = tx_gain_;
    c.tx_port = tx_port_;
    return c;
}

bool ReplayApp::configCurrent() const {
    if (!core() || !file_valid_) return false;
    const RfConfig c = core()->source().config();
    return c.direction == Direction::TX && c.center_freq == file_freq_ && c.sample_rate == file_rate_ && c.tx_port == tx_port_;
}

void ReplayApp::setTxGain(double db) {
    db = std::clamp(db, 0.0, kMaxTxGainDb);
    tx_gain_ = db;
    if (core()) core()->source().set_tx_gain(db);   // 運転中は Core の宣言を変える(送信 thread が適用し TxGain event)
    Q_EMIT statsChanged();
}

void ReplayApp::on_start(Core& core) {
    tx_ = &core.tx();
    tx_gain_ = core.source().config().tx_gain;
    position_ = 0;
    passes_ = 0;
    set_error({});
    StreamMeta m;
    m.id = "replay.tx";
    m.sample_rate = file_valid_ ? file_rate_ : core.source().config().sample_rate;
    m.center_freq = file_valid_ ? file_freq_ : core.source().config().center_freq;
    m.unit = "FS";
    m.direction = Direction::TX;
    view_stream_ = std::make_unique<Stream<sc16>>(m, &core.events());
    vp_ = std::make_unique<appfw::ViewProcessor>(*view_stream_, info().id + ".view");
    view_.setProcessor(vp_.get());
    vp_->start();
    timer_ = startTimer(100);
}

void ReplayApp::on_stop() {
    // Core::stop_app は Source を先に止める(TxStream が閉じて write が解ける)ので、ここで burst の thread を待てる
    stop_burst_ = true;
    join_burst();
    if (timer_) { killTimer(timer_); timer_ = 0; }
    view_.setProcessor(nullptr);
    vp_.reset();
    view_stream_.reset();
    tx_ = nullptr;
}

void ReplayApp::join_burst() {
    if (burst_th_.joinable()) burst_th_.join();
}

void ReplayApp::transmit() {
    if (!tx_ || sending_) return;
    if (!file_valid_) { set_error("no recording: " + file_error_); Q_EMIT statsChanged(); return; }
    if (!configCurrent()) { set_error("recording / port not applied to the radio yet"); Q_EMIT statsChanged(); return; }
    if (!core()->tx_armed()) { set_error("TX MASTER is SAFE: arm it in the main menu"); Q_EMIT statsChanged(); return; }
    join_burst();
    set_error({});
    stop_burst_ = false;
    sending_ = true;
    position_ = 0;
    passes_ = 0;
    burst_th_ = std::thread([this] { run_burst(); });
    Q_EMIT statsChanged();
}

void ReplayApp::stopTransmit() {
    stop_burst_ = true;
    Q_EMIT statsChanged();
}

// 1 つの burst: 録音を先頭から読み、Core の送信入口へ順に入れる。止める・終わる・拒否されたら EOB で閉じる。
// 書き込みは装置の速さで待たされる(TxStream は捨てない)。同じ block を表示用の stream にも配る(参照のみ、コピーなし)
void ReplayApp::run_burst() {
    SigmfReader r;
    std::string err;
    if (!r.open(file_.toStdString(), &err)) {
        set_error("cannot open recording: " + QString::fromStdString(err));
        sending_ = false;
        return;
    }
    bool first = true, refused = false;
    while (!stop_burst_) {
        auto b = pool_.acquire<sc16>(kDefaultBlockSamples);
        std::size_t n = r.read(b.data<sc16>().data(), kDefaultBlockSamples);
        if (n == 0) {
            if (!loop_ || !r.rewind()) break;   // 録音の終わり
            ++passes_;
            n = r.read(b.data<sc16>().data(), kDefaultBlockSamples);
            if (n == 0) break;
        }
        auto& h = b.header();
        h.sample_index = r.position() - n;   // 録音内の位置(表示・TAP 用。装置側の範囲は Core が送信済み数で付ける)
        h.host_ns = host_now_ns();
        if (first) h.flags.set(Flag::StartOfBurst);
        BlockRef ref = b.commit(static_cast<uint32_t>(n));
        TAP("replay.tx", ref.as<sc16>().data(), n);
        if (!tx_->write(ref)) { refused = true; break; }   // TX MASTER SAFE / App 停止(Source が閉じた)
        first = false;
        view_stream_->publish(ref);
        position_ = r.position();
    }
    if (refused && !tx_->closed()) set_error(tx_->armed() ? "transmission refused by the core" : "TX MASTER went SAFE: transmission stopped");
    if (!first && !refused) {
        auto eob = pool_.acquire<sc16>(1);
        eob.header().flags.set(Flag::EndOfBurst);
        tx_->write(eob.commit(0));
    }
    sending_ = false;
}

} // namespace spear::apps

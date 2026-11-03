// 1 送信(同期バーストから次の同期バーストまで)の受信情報と、選択呼出の着信判定、受信履歴。Qt 非依存。
// std-t99-receiver-sharp の Call.cs(CallInfo / Selective)と ReceiverWorker.cs の送信の区切りの移植。
//   着信判定: ARIB STD-T99 Annex 2 4.5、表 Annex 2-4-14(受信接続試験は表 Annex 2-7-6)
#pragma once

#include "session.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace spear::std_t99 {

enum class Standby { Monitor = 0, Individual = 1, Group = 2 };   // 全受信(モニター)/ 個別待受 / グループ待受

struct CallInfo {
    uint64_t number = 0;                 // 受信開始からの通し番号(1..)
    double started_utc = 0, last_utc = 0;   // 壁時計 [s](UNIX 時刻)
    uint64_t first_index = 0, last_index = 0;   // radio.rx の index(provenance)
    std::optional<std::string> caller, called;
    std::optional<uint32_t> extension;
    int comm_type = -1;                  // 1 グループ、4 個別
    int group = -1;                      // 0 = 一斉呼出番号
    int call_control = -1;               // 0 通常、1 緊急通信 1、2 緊急通信 2、3 緊急応答
    int voice_mode = -1;
    int kind = -1;                       // PICH2 のメッセージ種別(何のための呼か)
    std::string name;                    // 発信者名(UTF-8、CRC-32 一致後)
    bool name_failed = false, ended = false, voice = false;
    std::optional<Position> position;
    std::optional<GpsTime> gps_time;
    std::vector<int> requests;           // この呼で受けた遠隔送信要求 / 位置要求 / 機器 ID 要求(メッセージ種別)
    uint64_t frames = 0;

    bool emergency() const { return call_control == 1 || call_control == 2; }
    std::string call_type() const;       // "INDIVIDUAL" / "GROUP" / "ALL CALL" / "GROUP/ALL" / ""
    std::string destination() const;     // 宛先の機器 ID / "GROUP 03" / "ALL"
};

// 表 Annex 2-4-14(音声の出力)。nullopt = まだ決められない(通話種別が未確定)
std::optional<bool> accepts(const CallInfo& call, Standby mode, const std::string& own_id, int own_group);
// 機器 ID "MMM-IIIIIII"(メーカー 0..127、個別 0..2097151)に正規化。不正なら nullopt
std::optional<std::string> normalize_device_id(int maker, int64_t individual);

class CallMonitor {
public:
    static constexpr std::size_t kHistory = 100;
    // 受信したフレームごとに呼ぶ(Session::update の後)。戻り値: このフレームで新しい送信が始まったか。
    // t: 受信機の時刻 [s](フレームの間隔の判定。壁時計は録音の復号では使えない)、utc: 表示用の壁時計
    bool frame(const Frame& f, const Session& session, const std::vector<Decoded>& decoded, double t, double utc, uint64_t rx_index);
    const std::optional<CallInfo>& current() const { return call_; }
    const std::deque<CallInfo>& history() const { return history_; }   // 新しい順(現在の呼を含む)
    void reset() { call_.reset(); history_.clear(); count_ = 0; }

private:
    void remember(const CallInfo& c);
    std::optional<CallInfo> call_;
    double last_t_ = -1e9;
    std::deque<CallInfo> history_;
    uint64_t count_ = 0;
};

} // namespace spear::std_t99

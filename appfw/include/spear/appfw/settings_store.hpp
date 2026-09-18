// S.P.E.A.R. appfw — SettingsStore: 運転状態(スケルチ、MUTE、ゲイン/AGC、表示レンジ…)の自動保存と復元。
//
// 「再起動するたびにスケルチや AGC が初期化される」を、App ごとの保存コードではなく SDK の 1 つの仕組みで直す:
//   * 状態の所有者は今までどおり App / Shell の Q_PROPERTY(docs/state-ownership.md)。ここは値の写しを持つ「日誌」で、所有者ではない。
//   * bind(obj, prefix, {"squelchDb", "view.dbMax", ...}) で、保存値があれば setProperty で書き戻し(復元)、以後その NOTIFY signal を
//     監視して値をファイルに書く(自動保存、500 ms でまとめて原子的に書き換え)。子 QObject は 1 段のドット記法。
//   * 優先順位: App の既定値 < 保存値 < 起動時の --set(site.conf)。復元は configure(settings) より前に行うこと。
//   * ファイルは site.conf と同じ key=value(~/spear/state.conf、spear.sh が --state-file で渡す)。パス無しなら保存しない(検証用の起動)。
#pragma once

#include <QMetaProperty>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <map>
#include <set>
#include <vector>

namespace spear::appfw {

class SettingsStore : public QObject {
    Q_OBJECT
public:
    // path が空ならメモリだけ(復元も保存もしない)
    explicit SettingsStore(QString path, QObject* parent = nullptr);
    ~SettingsStore() override;

    bool load(QString* err = nullptr);   // ファイル → 値。無ければ空で true
    // obj の props(プロパティ名、または "child.prop")を prefix の下に束ねる: 保存値があれば復元し、以後の変更を保存する
    void bind(QObject* obj, const QString& prefix, const QStringList& props);
    void flush();                        // 未保存分を今書く(終了時にも呼ばれる)

    QString path() const { return path_; }
    QVariantMap values() const;          // 現在の値(テスト・診断用)
    int boundCount() const { return static_cast<int>(targets_.size()); }
    int restoredCount() const { return restored_; }
    int unresolvedCount() const { return unresolved_; }   // 名前が解決できなかった宣言(プログラムの誤り。ログに出す)

private Q_SLOTS:
    void onNotify();

private:
    struct Target { QPointer<QObject> holder; QString key; QMetaProperty prop; int signal = -1; };
    static QString to_text(const QVariant& v);
    void record(const Target& t);
    void write_file();

    QString path_;
    std::map<QString, QString> values_;
    std::vector<Target> targets_;
    std::set<std::pair<QObject*, int>> connected_;
    QTimer timer_;
    bool dirty_ = false, restoring_ = false;
    int restored_ = 0, unresolved_ = 0;
};

} // namespace spear::appfw

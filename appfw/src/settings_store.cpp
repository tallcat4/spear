#include "settings_store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaMethod>
#include <QSaveFile>
#include <QTextStream>

#include <algorithm>

namespace spear::appfw {

SettingsStore::SettingsStore(QString path, QObject* parent) : QObject(parent), path_(std::move(path)) {
    timer_.setSingleShot(true);
    timer_.setInterval(500);
    connect(&timer_, &QTimer::timeout, this, [this] { write_file(); });
}

SettingsStore::~SettingsStore() { flush(); }

bool SettingsStore::load(QString* err) {
    values_.clear();
    if (path_.isEmpty()) return true;
    QFile f(path_);
    if (!f.exists()) return true;
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { if (err) *err = "cannot read " + path_; return false; }
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const qsizetype eq = line.indexOf('=');
        if (eq <= 0) continue;
        values_[line.left(eq).trimmed()] = line.mid(eq + 1).trimmed();
    }
    return true;
}

QString SettingsStore::to_text(const QVariant& v) {
    switch (v.typeId()) {
    case QMetaType::Bool: return v.toBool() ? "true" : "false";
    case QMetaType::Double: return QString::number(v.toDouble(), 'g', 17);
    case QMetaType::Float: return QString::number(v.toFloat(), 'g', 9);
    default: return v.toString();
    }
}

void SettingsStore::bind(QObject* obj, const QString& prefix, const QStringList& props) {
    for (const QString& name : props) {
        // "child.prop": 1 段だけ子 QObject をたどる(App の ViewSource など)
        QObject* holder = obj;
        QString prop_name = name;
        const qsizetype dot = name.indexOf('.');
        if (dot > 0) {
            holder = obj->property(name.left(dot).toUtf8().constData()).value<QObject*>();
            prop_name = name.mid(dot + 1);
        }
        const int idx = holder ? holder->metaObject()->indexOfProperty(prop_name.toUtf8().constData()) : -1;
        if (idx < 0) { ++unresolved_; qWarning("SettingsStore: %s.%s: no such property", qPrintable(prefix), qPrintable(name)); continue; }
        Target t;
        t.holder = holder;
        t.key = prefix + "." + name;
        t.prop = holder->metaObject()->property(idx);
        if (!t.prop.isWritable() || !t.prop.hasNotifySignal()) { ++unresolved_; qWarning("SettingsStore: %s: property must be writable with a NOTIFY signal", qPrintable(t.key)); continue; }
        t.signal = t.prop.notifySignalIndex();
        // 復元(保存中は書かない)。型変換は QMetaProperty::write(文字列 → double / int / bool / QString)
        if (const auto it = values_.find(t.key); it != values_.end()) {
            restoring_ = true;
            if (t.prop.write(holder, QVariant(it->second))) ++restored_;
            else qWarning("SettingsStore: %s: cannot restore '%s'", qPrintable(t.key), qPrintable(it->second));
            restoring_ = false;
        }
        // 同じ (holder, signal) には 1 回だけ接続(1 つの NOTIFY を複数プロパティが共有する App が多い)
        if (connected_.insert({holder, t.signal}).second) {
            const QByteArray sig = QByteArray("2") + t.prop.notifySignal().methodSignature();
            connect(holder, sig.constData(), this, SLOT(onNotify()));
        }
        targets_.push_back(t);
    }
}

void SettingsStore::onNotify() {
    if (restoring_) return;
    QObject* s = sender();
    const int sig = senderSignalIndex();
    for (const auto& t : targets_) if (t.holder == s && t.signal == sig) record(t);
}

void SettingsStore::record(const Target& t) {
    if (!t.holder) return;
    const QString text = to_text(t.prop.read(t.holder));
    auto it = values_.find(t.key);
    if (it != values_.end() && it->second == text) return;
    values_[t.key] = text;
    dirty_ = true;
    if (!path_.isEmpty()) timer_.start();
}

QVariantMap SettingsStore::values() const {
    QVariantMap m;
    for (const auto& [k, v] : values_) m[k] = v;
    return m;
}

void SettingsStore::flush() {
    timer_.stop();
    if (dirty_) write_file();
}

void SettingsStore::write_file() {
    dirty_ = false;
    if (path_.isEmpty()) return;
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile f(path_);   // 一時ファイル → rename(途中で電源が落ちても壊れない)
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) { qWarning("SettingsStore: cannot write %s", qPrintable(path_)); return; }
    QTextStream out(&f);
    out << "# S.P.E.A.R. state — 運転状態の自動保存(スケルチ、音量、ゲイン/AGC、表示レンジ…)。起動時に復元される。\n"
           "# 手で編集するものではない(個体・現場の値は site.conf に。site.conf の --set はこのファイルより優先)。\n";
    for (const auto& [k, v] : values_) out << k << "=" << v << "\n";
    out.flush();
    if (!f.commit()) qWarning("SettingsStore: cannot commit %s", qPrintable(path_));
}

} // namespace spear::appfw

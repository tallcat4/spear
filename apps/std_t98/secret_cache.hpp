#pragma once
#include "secret/guided.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <set>

namespace spear::apps {
// SDRSharp.T98 SecretCache v1. Used only on the worker thread after startup.
class SecretCacheFile {
public:
    explicit SecretCacheFile(QString path) : path_(std::move(path)) {
        QFile file(path_);
        if (!file.exists()) return;
        if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
            fail("Cache read failed: " + file.errorString()); return;
        }
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
        const auto obj = doc.object();
        if (error.error != QJsonParseError::NoError || obj["Version"].toInt() != 1 || !obj["Codes"].isArray()) {
            fail("Cache read failed: unsupported or corrupt format"); return;
        }
        const QRegularExpression pattern("^(?:([0-9]{2})-)?([0-9]{5})$");
        for (const auto value : obj["Codes"].toArray()) {
            const auto match = pattern.match(value.toString());
            if (!match.hasMatch()) continue;
            const int series = match.captured(1).toInt(), key = match.captured(2).toInt();
            if ((!match.captured(1).isEmpty() && (series < 1 || series > 15)) || key < 1 || key > 32767) continue;
            codes_.insert({static_cast<uint8_t>(series), static_cast<uint16_t>(key)});
        }
    }
    std::vector<std_t98::secret::SeriesKey> codes() const { return {codes_.begin(), codes_.end()}; }
    QString warning() const { return warning_; }
    void remember(std_t98::secret::SeriesKey code) {
        if (code.series > 15 || code.key < 1 || code.key > 32767 || !readable_) return;
        const bool added = codes_.insert(code).second;
        if (!added && warning_.isEmpty()) return;
        QJsonArray values;
        for (auto c : codes_) {
            QString text = QString("%1").arg(c.key, 5, 10, QLatin1Char('0'));
            if (c.series) text.prepend(QString("%1-").arg(c.series, 2, 10, QLatin1Char('0')));
            values.append(text);
        }
        const auto data = QJsonDocument(QJsonObject{{"Version", 1}, {"Codes", values}}).toJson();
        QSaveFile file(path_);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            warning_ = "Cache save failed: " + file.errorString(); return;
        }
        warning_.clear();
    }
private:
    void fail(QString message) { readable_ = false; warning_ = std::move(message); }
    QString path_, warning_;
    bool readable_ = true;
    std::set<std_t98::secret::SeriesKey> codes_;
};
}

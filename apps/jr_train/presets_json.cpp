// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KIRISHIKI Yudai
// 自動生成: 原本の ui/radio-presets.json(commit dfc7440)をそのまま埋め込んだもの。手で編集しない。
// 作り直し: python3 で原本の JSON を R"JSON(...)JSON" に入れ直す(docs/apps/jr_train.md「プリセット」)。
// 路線表の出典は原本の docs/ROUTES.ja.md(odaQshirei 氏の Posfie まとめ、確認日 2026-10-12)。
namespace spear::jr_train {
extern const char* const kBuiltinPresetsJson = R"JSON({
  "schema_version": 1,
  "channel_sets": [
    {
      "id": "east-downlink",
      "region": "east",
      "direction": "downlink",
      "channels": [
        {
          "frequency_hz": 352537500,
          "label": "D5 音声第1波",
          "service": "voice",
          "note": "変則割当：大船渡線 D7 音声第1波"
        },
        {
          "frequency_hz": 352543750,
          "label": "D7 音声第1波",
          "service": "voice",
          "note": "変則割当：奥羽本線（大館～青森）D6 音声第1波 / 仙石線 D2 音声第1波"
        },
        {
          "frequency_hz": 352550000,
          "label": "D3 音声第1波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352556250,
          "label": "D1 音声第1波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352562500,
          "label": "D4 音声第1波",
          "service": "voice",
          "note": "変則割当：仙山線 D3 音声第2波"
        },
        {
          "frequency_hz": 352568750,
          "label": "D6 音声第1波",
          "service": "voice",
          "note": "変則割当：東北本線（黒磯～盛岡）D1 音声第2波"
        },
        {
          "frequency_hz": 352575000,
          "label": "D2 音声第1波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352581250,
          "label": "D6 音声第2波",
          "service": "voice",
          "note": "変則割当：陸羽東線 D3 音声第2波 / 大船渡線 D7 音声第2波"
        },
        {
          "frequency_hz": 352587500,
          "label": "D5 音声第2波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352593750,
          "label": "D2 音声第2波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352600000,
          "label": "D4 音声第2波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352606250,
          "label": "D7 音声第2波",
          "service": "voice",
          "note": "変則割当：仙石線 D2 音声第2波 / 石巻線 D5 音声第2波"
        },
        {
          "frequency_hz": 352612500,
          "label": "D3 音声第2波",
          "service": "voice",
          "note": "変則割当：中央本線（甲府～小淵沢）D6 音声第1波"
        },
        {
          "frequency_hz": 352618750,
          "label": "D1 音声第2波",
          "service": "voice",
          "note": "変則割当：小海線 D4 音声第1波"
        },
        {
          "frequency_hz": 352625000,
          "label": "旅客一斉情報",
          "service": "common",
          "note": ""
        },
        {
          "frequency_hz": 352631250,
          "label": "D1 データ波",
          "service": "data",
          "note": ""
        },
        {
          "frequency_hz": 352637500,
          "label": "D2 データ波",
          "service": "data",
          "note": ""
        },
        {
          "frequency_hz": 352643750,
          "label": "D3 データ波",
          "service": "data",
          "note": ""
        },
        {
          "frequency_hz": 352650000,
          "label": "D4 データ波",
          "service": "data",
          "note": ""
        },
        {
          "frequency_hz": 352656250,
          "label": "D5 データ波",
          "service": "data",
          "note": ""
        },
        {
          "frequency_hz": 352662500,
          "label": "D6 データ波",
          "service": "data",
          "note": ""
        },
        {
          "frequency_hz": 352668750,
          "label": "D7 データ波",
          "service": "data",
          "note": ""
        }
      ],
      "routes": [
        {
          "id": "east-metro-01",
          "line": "横須賀線",
          "section": "東京～久里浜",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352618750
          ],
          "data_hz": 352631250
        },
        {
          "id": "east-metro-02",
          "line": "総武快速線",
          "section": "東京～千葉",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352618750
          ],
          "data_hz": 352631250
        },
        {
          "id": "east-metro-03",
          "line": "八高線",
          "section": "八王子～高麗川",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352618750
          ],
          "data_hz": 352631250
        },
        {
          "id": "east-metro-04",
          "line": "東北本線",
          "section": "東京～黒磯",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-metro-05",
          "line": "中央快速線",
          "section": "四ツ谷～甲府",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-metro-06",
          "line": "総武本線",
          "section": "千葉～佐倉",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-metro-07",
          "line": "成田線",
          "section": "佐倉～成田空港",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-metro-08",
          "line": "成田線",
          "section": "我孫子～成田",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-metro-09",
          "line": "武蔵野線",
          "section": "新鶴見～府中本町",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-metro-10",
          "line": "武蔵野線",
          "section": "新松戸～西船橋",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-metro-11",
          "line": "東海道貨物線",
          "section": "東京（タ）～浜川崎",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-metro-12",
          "line": "東海道貨物線",
          "section": "新鶴見～東戸塚",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-metro-13",
          "line": "南武支線",
          "section": "尻手～浜川崎",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-metro-14",
          "line": "高島線",
          "section": "鶴見～桜木町",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-metro-15",
          "line": "埼京線",
          "section": "大崎～大宮",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-16",
          "line": "川越線",
          "section": "大宮～高麗川",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-17",
          "line": "常磐緩行線",
          "section": "綾瀬～取手",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-18",
          "line": "中央快速線",
          "section": "東京～四ツ谷",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-19",
          "line": "南武線",
          "section": "川崎～立川",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-20",
          "line": "相模線",
          "section": "茅ケ崎～橋本",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-21",
          "line": "鶴見線",
          "section": "鶴見～扇町",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-22",
          "line": "鶴見線",
          "section": "浅野～海芝浦",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-23",
          "line": "鶴見線",
          "section": "安善～大川",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-24",
          "line": "京葉線",
          "section": "東京～蘇我",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-25",
          "line": "京葉線",
          "section": "市川塩浜～西船橋",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-26",
          "line": "京葉線",
          "section": "西船橋～南船橋",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-27",
          "line": "高崎線",
          "section": "神保原～高崎",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-28",
          "line": "上越線",
          "section": "高崎～新前橋",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-29",
          "line": "青梅線",
          "section": "青梅～奥多摩",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-metro-30",
          "line": "東海道本線",
          "section": "東京～熱海",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-metro-31",
          "line": "常磐快速線",
          "section": "上野～勝田",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-metro-32",
          "line": "高崎線",
          "section": "大宮～神保原",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-metro-33",
          "line": "東北貨物線",
          "section": "田端操～大宮",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-metro-34",
          "line": "山手貨物線",
          "section": "池袋～田端操",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-metro-35",
          "line": "五日市線",
          "section": "拝島～武蔵五日市",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-metro-36",
          "line": "外房線",
          "section": "千葉～蘇我",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-metro-37",
          "line": "内房線",
          "section": "蘇我～君津",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-metro-38",
          "line": "中央・総武緩行線",
          "section": "三鷹～千葉",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-metro-39",
          "line": "横浜線",
          "section": "東神奈川～八王子",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-metro-40",
          "line": "武蔵野線",
          "section": "府中本町～新松戸",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-metro-41",
          "line": "武蔵野線",
          "section": "新小平～国立",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-metro-42",
          "line": "武蔵野線",
          "section": "西浦和～大宮",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-metro-43",
          "line": "青梅線",
          "section": "立川～青梅",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-metro-44",
          "line": "京浜東北・根岸線",
          "section": "大宮～大船",
          "designation": "D6",
          "voice_hz": [
            352568750,
            352581250
          ],
          "data_hz": 352662500
        },
        {
          "id": "east-metro-45",
          "line": "外房線",
          "section": "蘇我～大原",
          "designation": "D6",
          "voice_hz": [
            352568750,
            352581250
          ],
          "data_hz": 352662500
        },
        {
          "id": "east-metro-46",
          "line": "山手線",
          "section": "品川～品川",
          "designation": "D7",
          "voice_hz": [
            352543750,
            352606250
          ],
          "data_hz": 352668750
        },
        {
          "id": "east-regional-01",
          "line": "東北本線",
          "section": "黒磯～盛岡",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352568750
          ],
          "data_hz": 352631250,
          "note": "変則割当：音声第2波 352.56875 MHz"
        },
        {
          "id": "east-regional-02",
          "line": "利府線",
          "section": "岩切～利府",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352618750
          ],
          "data_hz": 352631250
        },
        {
          "id": "east-regional-03",
          "line": "宮城野貨物線",
          "section": "長町～東仙台",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352618750
          ],
          "data_hz": 352631250
        },
        {
          "id": "east-regional-04",
          "line": "陸羽西線",
          "section": "新庄～余目",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352618750
          ],
          "data_hz": 352631250
        },
        {
          "id": "east-regional-05",
          "line": "左沢線",
          "section": "北山形～左沢",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352618750
          ],
          "data_hz": 352631250
        },
        {
          "id": "east-regional-06",
          "line": "信越本線",
          "section": "直江津～新潟",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352618750
          ],
          "data_hz": 352631250
        },
        {
          "id": "east-regional-07",
          "line": "信越本線貨物支線",
          "section": "越後石山～新潟（タ）",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352618750
          ],
          "data_hz": 352631250
        },
        {
          "id": "east-regional-08",
          "line": "白新線",
          "section": "上沼垂（信）～新潟（タ）",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352618750
          ],
          "data_hz": 352631250
        },
        {
          "id": "east-regional-09",
          "line": "両毛線",
          "section": "新前橋～小山",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352618750
          ],
          "data_hz": 352631250
        },
        {
          "id": "east-regional-10",
          "line": "鹿島線",
          "section": "香取～鹿島サッカースタジアム",
          "designation": "D1",
          "voice_hz": [
            352556250,
            352618750
          ],
          "data_hz": 352631250
        },
        {
          "id": "east-regional-11",
          "line": "奥羽本線（狭軌区間）",
          "section": "新庄～秋田",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-regional-12",
          "line": "釜石線",
          "section": "花巻～釜石",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-regional-13",
          "line": "仙石線",
          "section": "あおば通～石巻",
          "designation": "D2",
          "voice_hz": [
            352543750,
            352606250
          ],
          "data_hz": 352637500,
          "note": "変則割当：音声第1波 352.54375 MHz / 音声第2波 352.60625 MHz"
        },
        {
          "id": "east-regional-14",
          "line": "越後線",
          "section": "柏崎～新潟",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-regional-15",
          "line": "水郡線",
          "section": "水戸～安積永盛・常陸太田",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-regional-16",
          "line": "吾妻線",
          "section": "渋川～大前",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-regional-17",
          "line": "八高線",
          "section": "高麗川～北藤岡",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-regional-18",
          "line": "新金貨物線",
          "section": "新小岩（信）～金町",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-regional-19",
          "line": "越中島支線",
          "section": "新小岩（信）～越中島（貨）",
          "designation": "D2",
          "voice_hz": [
            352575000,
            352593750
          ],
          "data_hz": 352637500
        },
        {
          "id": "east-regional-20",
          "line": "飯山線",
          "section": "豊野～越後川口",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-21",
          "line": "津軽線",
          "section": "青森～新中小国（信）",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-22",
          "line": "八戸線",
          "section": "八戸～久慈",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-23",
          "line": "山田線",
          "section": "盛岡～区界",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-24",
          "line": "男鹿線",
          "section": "追分～男鹿",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-25",
          "line": "陸羽東線",
          "section": "小牛田～新庄",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352581250
          ],
          "data_hz": 352643750,
          "note": "変則割当：音声第2波 352.58125 MHz"
        },
        {
          "id": "east-regional-26",
          "line": "仙山線",
          "section": "仙台～山形",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352562500
          ],
          "data_hz": 352643750,
          "note": "変則割当：音声第2波 352.56250 MHz"
        },
        {
          "id": "east-regional-27",
          "line": "磐越東線",
          "section": "郡山～いわき",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-28",
          "line": "米坂線",
          "section": "米沢～坂町",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-29",
          "line": "弥彦線",
          "section": "東三条～弥彦",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-30",
          "line": "上越線",
          "section": "新前橋～宮内",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-31",
          "line": "常磐線",
          "section": "原ノ町～岩沼",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-32",
          "line": "水戸線",
          "section": "小山～友部",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-33",
          "line": "烏山線",
          "section": "宝積寺～烏山",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-34",
          "line": "東金線",
          "section": "大網～成東",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-35",
          "line": "内房線",
          "section": "館山～安房鴨川",
          "designation": "D3",
          "voice_hz": [
            352550000,
            352612500
          ],
          "data_hz": 352643750
        },
        {
          "id": "east-regional-36",
          "line": "大湊線",
          "section": "野辺地～大湊",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-regional-37",
          "line": "北上線",
          "section": "北上～横手",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-regional-38",
          "line": "気仙沼線",
          "section": "前谷地～柳津",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-regional-39",
          "line": "羽越本線",
          "section": "新津～秋田",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-regional-40",
          "line": "常磐線",
          "section": "勝田～いわき",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-regional-41",
          "line": "只見線",
          "section": "只見～小出",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-regional-42",
          "line": "白新線",
          "section": "新潟～新発田",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-regional-43",
          "line": "大糸線",
          "section": "松本～南小谷",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-regional-44",
          "line": "小海線",
          "section": "小淵沢～小諸",
          "designation": "D4",
          "voice_hz": [
            352618750,
            352600000
          ],
          "data_hz": 352650000,
          "note": "変則割当：音声第1波 352.61875 MHz"
        },
        {
          "id": "east-regional-45",
          "line": "成田線",
          "section": "成田～松岸",
          "designation": "D4",
          "voice_hz": [
            352562500,
            352600000
          ],
          "data_hz": 352650000
        },
        {
          "id": "east-regional-46",
          "line": "田沢湖線・秋田新幹線",
          "section": "盛岡～大曲",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-regional-47",
          "line": "奥羽本線・秋田新幹線（標準軌区間）",
          "section": "大曲～秋田",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-regional-48",
          "line": "石巻線",
          "section": "小牛田～女川",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352606250
          ],
          "data_hz": 352656250,
          "note": "変則割当：音声第2波 352.60625 MHz"
        },
        {
          "id": "east-regional-49",
          "line": "奥羽本線・山形新幹線",
          "section": "福島～新庄",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-regional-50",
          "line": "磐越西線",
          "section": "郡山～新津",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-regional-51",
          "line": "常磐線",
          "section": "いわき～原ノ町",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-regional-52",
          "line": "篠ノ井線",
          "section": "松本～篠ノ井",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-regional-53",
          "line": "信越本線",
          "section": "高崎～横川",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-regional-54",
          "line": "信越本線",
          "section": "篠ノ井～長野",
          "designation": "D5",
          "voice_hz": [
            352537500,
            352587500
          ],
          "data_hz": 352656250
        },
        {
          "id": "east-regional-55",
          "line": "総武本線",
          "section": "佐倉～銚子",
          "designation": "D6",
          "voice_hz": [
            352568750,
            352581250
          ],
          "data_hz": 352662500
        },
        {
          "id": "east-regional-56",
          "line": "内房線",
          "section": "君津～館山",
          "designation": "D6",
          "voice_hz": [
            352568750,
            352581250
          ],
          "data_hz": 352662500
        },
        {
          "id": "east-regional-57",
          "line": "伊東線",
          "section": "熱海～伊東",
          "designation": "D6",
          "voice_hz": [
            352568750,
            352581250
          ],
          "data_hz": 352662500
        },
        {
          "id": "east-regional-58",
          "line": "奥羽本線",
          "section": "秋田～大館",
          "designation": "D6",
          "voice_hz": [
            352568750,
            352581250
          ],
          "data_hz": 352662500
        },
        {
          "id": "east-regional-59",
          "line": "奥羽本線",
          "section": "大館～青森",
          "designation": "D6",
          "voice_hz": [
            352543750,
            352581250
          ],
          "data_hz": 352662500,
          "note": "変則割当：音声第1波 352.54375 MHz"
        },
        {
          "id": "east-regional-60",
          "line": "中央本線",
          "section": "甲府～小淵沢",
          "designation": "D6",
          "voice_hz": [
            352612500,
            352581250
          ],
          "data_hz": 352662500,
          "note": "変則割当：音声第1波 352.61250 MHz"
        },
        {
          "id": "east-regional-61",
          "line": "中央本線",
          "section": "小淵沢～塩尻",
          "designation": "D6",
          "voice_hz": [
            352568750,
            352581250
          ],
          "data_hz": 352662500
        },
        {
          "id": "east-regional-62",
          "line": "辰野支線",
          "section": "岡谷～辰野～塩尻",
          "designation": "D6",
          "voice_hz": [
            352568750,
            352581250
          ],
          "data_hz": 352662500
        },
        {
          "id": "east-regional-63",
          "line": "篠ノ井線",
          "section": "塩尻～松本",
          "designation": "D6",
          "voice_hz": [
            352568750,
            352581250
          ],
          "data_hz": 352662500
        },
        {
          "id": "east-regional-64",
          "line": "外房線",
          "section": "大原～安房鴨川",
          "designation": "D6",
          "voice_hz": [
            352568750,
            352581250
          ],
          "data_hz": 352662500
        },
        {
          "id": "east-regional-65",
          "line": "大船渡線",
          "section": "一ノ関～気仙沼",
          "designation": "D7",
          "voice_hz": [
            352537500,
            352581250
          ],
          "data_hz": 352668750,
          "note": "変則割当：音声第1波 352.53750 MHz / 音声第2波 352.58125 MHz"
        },
        {
          "id": "east-regional-66",
          "line": "日光線",
          "section": "宇都宮～日光",
          "designation": "D7",
          "voice_hz": [
            352543750,
            352606250
          ],
          "data_hz": 352668750
        }
      ]
    },
    {
      "id": "central-downlink",
      "region": "central",
      "direction": "downlink",
      "channels": [
        {
          "frequency_hz": 352600000,
          "label": "D4 音声第1波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352637500,
          "label": "D4 音声第2波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352650000,
          "label": "D4 データ波",
          "service": "data",
          "note": ""
        }
      ],
      "routes": [
        {
          "id": "central-01",
          "line": "東海道本線",
          "section": "新所原～米原",
          "designation": "D4",
          "voice_hz": [
            352600000,
            352637500
          ],
          "data_hz": 352650000,
          "note": "掲載表は送信を確認した区間。運用開始時の無線切替地点により変更される可能性があります。"
        },
        {
          "id": "central-02",
          "line": "美濃赤坂線",
          "section": "大垣～美濃赤坂",
          "designation": "D4",
          "voice_hz": [
            352600000,
            352637500
          ],
          "data_hz": 352650000,
          "note": "掲載表は送信を確認した区間。運用開始時の無線切替地点により変更される可能性があります。"
        }
      ]
    },
    {
      "id": "west-downlink",
      "region": "west",
      "direction": "downlink",
      "channels": [
        {
          "frequency_hz": 352537500,
          "label": "D2 音声第1波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352543750,
          "label": "旅客一斉情報",
          "service": "common",
          "note": ""
        },
        {
          "frequency_hz": 352550000,
          "label": "D5 音声第2波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352556250,
          "label": "D4 音声第2波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352562500,
          "label": "D2 音声第2波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352568750,
          "label": "D6 音声第2波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352575000,
          "label": "D5 音声第1波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352581250,
          "label": "D3 音声第2波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352587500,
          "label": "D7 音声第2波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352593750,
          "label": "D7 音声第1波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352600000,
          "label": "D4 音声第1波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352606250,
          "label": "D6 音声第1波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352612500,
          "label": "D3 音声第1波",
          "service": "voice",
          "note": ""
        },
        {
          "frequency_hz": 352637500,
          "label": "D2 データ波",
          "service": "data",
          "note": ""
        },
        {
          "frequency_hz": 352643750,
          "label": "D3 データ波",
          "service": "data",
          "note": ""
        },
        {
          "frequency_hz": 352650000,
          "label": "D4 データ波",
          "service": "data",
          "note": ""
        },
        {
          "frequency_hz": 352656250,
          "label": "D5 データ波",
          "service": "data",
          "note": ""
        },
        {
          "frequency_hz": 352662500,
          "label": "D6 データ波",
          "service": "data",
          "note": ""
        },
        {
          "frequency_hz": 352668750,
          "label": "D7 データ波",
          "service": "data",
          "note": ""
        }
      ],
      "routes": [
        {
          "id": "west-01",
          "line": "湖西線",
          "section": "山科～近江塩津",
          "designation": "D2",
          "voice_hz": [
            352537500,
            352562500
          ],
          "data_hz": 352637500
        },
        {
          "id": "west-02",
          "line": "片町線",
          "section": "木津～京橋",
          "designation": "D2",
          "voice_hz": [
            352537500,
            352562500
          ],
          "data_hz": 352637500
        },
        {
          "id": "west-03",
          "line": "JR東西線",
          "section": "京橋～尼崎",
          "designation": "D2",
          "voice_hz": [
            352537500,
            352562500
          ],
          "data_hz": 352637500
        },
        {
          "id": "west-04",
          "line": "福知山線",
          "section": "尼崎～新三田",
          "designation": "D3",
          "voice_hz": [
            352612500,
            352581250
          ],
          "data_hz": 352643750
        },
        {
          "id": "west-05",
          "line": "阪和線",
          "section": "天王寺～和歌山",
          "designation": "D3",
          "voice_hz": [
            352612500,
            352581250
          ],
          "data_hz": 352643750
        },
        {
          "id": "west-06",
          "line": "阪和線",
          "section": "鳳～東羽衣",
          "designation": "D3",
          "voice_hz": [
            352612500,
            352581250
          ],
          "data_hz": 352643750
        },
        {
          "id": "west-07",
          "line": "東海道本線",
          "section": "米原～神戸",
          "designation": "D4",
          "voice_hz": [
            352600000,
            352556250
          ],
          "data_hz": 352650000
        },
        {
          "id": "west-08",
          "line": "山陽本線",
          "section": "神戸～姫路",
          "designation": "D4",
          "voice_hz": [
            352600000,
            352556250
          ],
          "data_hz": 352650000
        },
        {
          "id": "west-09",
          "line": "北方貨物線",
          "section": "吹田（タ）～尼崎",
          "designation": "D4",
          "voice_hz": [
            352600000,
            352556250
          ],
          "data_hz": 352650000
        },
        {
          "id": "west-10",
          "line": "梅田貨物線",
          "section": "吹田（タ）～大阪",
          "designation": "D4",
          "voice_hz": [
            352600000,
            352556250
          ],
          "data_hz": 352650000
        },
        {
          "id": "west-11",
          "line": "和田岬線",
          "section": "兵庫～和田岬",
          "designation": "D4",
          "voice_hz": [
            352600000,
            352556250
          ],
          "data_hz": 352650000
        },
        {
          "id": "west-12",
          "line": "関西空港線",
          "section": "日根野～関西空港",
          "designation": "D5",
          "voice_hz": [
            352575000,
            352550000
          ],
          "data_hz": 352656250
        },
        {
          "id": "west-13",
          "line": "関西本線",
          "section": "JR難波～加茂",
          "designation": "D5",
          "voice_hz": [
            352575000,
            352550000
          ],
          "data_hz": 352656250
        },
        {
          "id": "west-14",
          "line": "山陰本線",
          "section": "京都～園部",
          "designation": "D5",
          "voice_hz": [
            352575000,
            352550000
          ],
          "data_hz": 352656250
        },
        {
          "id": "west-15",
          "line": "奈良線",
          "section": "京都～木津",
          "designation": "D6",
          "voice_hz": [
            352606250,
            352568750
          ],
          "data_hz": 352662500
        },
        {
          "id": "west-16",
          "line": "大阪環状線",
          "section": "大阪～大阪",
          "designation": "D6",
          "voice_hz": [
            352606250,
            352568750
          ],
          "data_hz": 352662500
        },
        {
          "id": "west-17",
          "line": "桜島線",
          "section": "西九条～桜島",
          "designation": "D6",
          "voice_hz": [
            352606250,
            352568750
          ],
          "data_hz": 352662500
        },
        {
          "id": "west-18",
          "line": "梅田貨物線",
          "section": "大阪～西九条",
          "designation": "D6",
          "voice_hz": [
            352606250,
            352568750
          ],
          "data_hz": 352662500
        },
        {
          "id": "west-19",
          "line": "おおさか東線",
          "section": "新大阪～久宝寺",
          "designation": "D7",
          "voice_hz": [
            352593750,
            352587500
          ],
          "data_hz": 352668750
        }
      ]
    }
  ],
  "profiles": [
    {
      "id": "east",
      "label": "JR 東日本（22波）",
      "channel_set": "east-downlink",
      "services": [
        "voice",
        "data",
        "common"
      ],
      "capture": {
        "center_hz": 352603125,
        "sample_rate": 240000
      },
      "note": "音声14波・データ7波・旅客一斉情報1波を同時受信します。"
    },
    {
      "id": "central",
      "label": "JR 東海（3波）",
      "channel_set": "central-downlink",
      "services": [
        "voice",
        "data"
      ],
      "capture": {
        "center_hz": 352625000,
        "sample_rate": 240000
      },
      "note": "音声2波・データ1波を同時受信します。"
    },
    {
      "id": "west",
      "label": "JR 西日本（19波）",
      "channel_set": "west-downlink",
      "services": [
        "voice",
        "data",
        "common"
      ],
      "capture": {
        "center_hz": 352603125,
        "sample_rate": 240000
      },
      "note": "音声12波・データ6波・旅客一斉情報1波を同時受信します。"
    }
  ]
}
)JSON";
} // namespace spear::jr_train

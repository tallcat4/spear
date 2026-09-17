#!/usr/bin/env python3
"""ミニマップ用の地図データ world.spearmap を作る(通常は不要 — 生成物はリポジトリに同梱しバイナリに埋め込む)。

出典(いずれもパブリックドメイン、ネット接続が要る):
  Natural Earth 1:10m (https://www.naturalearthdata.com/, GeoJSON は github.com/nvkelso/natural-earth-vector):
    ne_10m_land, ne_10m_lakes, ne_10m_admin_0_boundary_lines_land, ne_10m_admin_1_states_provinces_lines(日本の都道府県境だけ), ne_10m_populated_places_simple
  OurAirports (https://ourairports.com/data/): airports.csv, runways.csv
依存は Python 標準ライブラリのみ(shapely 不要)。全世界を持ち、海岸線は Douglas–Peucker で tol 度に間引く(既定 0.003° ≈ 330 m)。

使い方: build_map.py [--tol 0.003] [--cache DIR] [--out world.spearmap]

形式(little endian、apps/adsb/map/map_data.cpp が読む):
  magic "SPMAP1\\0\\0"
  section land:     u32 n; n × { u32 npts; npts × (f32 lon, f32 lat) }   閉じた多角形(外周も穴も同列、even-odd で塗る)
  section lakes:    同上
  section borders:  同上(折れ線、国境)
  section admin1:   同上(折れ線、日本の都道府県境)
  section places:   u32 n; n × { f32 lon, f32 lat, u32 pop, u8 rank, u8 len, name(utf-8) }
  section airports: u32 n; n × { f32 lon, f32 lat, u8 kind(0 large 1 medium 2 small), u8 len, ident, u8 len, name }
  section runways:  u32 n; n × { f32 lon1, f32 lat1, f32 lon2, f32 lat2, f32 length_m }
"""
import csv, json, math, os, struct, sys, urllib.request

NE = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/"
OA = "https://davidmegginson.github.io/ourairports-data/"
FILES = {
    "land": NE + "ne_10m_land.geojson",
    "lakes": NE + "ne_10m_lakes.geojson",
    "borders": NE + "ne_10m_admin_0_boundary_lines_land.geojson",
    "admin1": NE + "ne_10m_admin_1_states_provinces_lines.geojson",
    "places": NE + "ne_10m_populated_places_simple.geojson",
    "airports": OA + "airports.csv",
    "runways": OA + "runways.csv",
}

def arg(key, default):
    return sys.argv[sys.argv.index(key) + 1] if key in sys.argv else default

def fetch(name, cache):
    url = FILES[name]
    path = os.path.join(cache, os.path.basename(url))
    if not os.path.exists(path):
        print("download", url)
        urllib.request.urlretrieve(url, path)
    return path

def simplify(pts, tol):
    """Douglas–Peucker(反復版)。pts = [(lon, lat), ...]"""
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a]; bx, by = pts[b]
        dx, dy = bx - ax, by - ay
        ln = math.hypot(dx, dy)
        best, bi = 0.0, -1
        for i in range(a + 1, b):
            px, py = pts[i]
            d = abs(dy * px - dx * py + bx * ay - by * ax) / ln if ln > 0 else math.hypot(px - ax, py - ay)
            if d > best:
                best, bi = d, i
        if best > tol and bi > 0:
            keep[bi] = True
            stack.append((a, bi)); stack.append((bi, b))
    return [p for p, k in zip(pts, keep) if k]

def ring_area(pts):
    s = 0.0
    for i in range(len(pts) - 1):
        s += pts[i][0] * pts[i + 1][1] - pts[i + 1][0] * pts[i][1]
    return abs(s) / 2

def rings_of(geom):
    t = geom["type"]
    if t == "Polygon":
        return [r for r in geom["coordinates"]]
    if t == "MultiPolygon":
        return [r for poly in geom["coordinates"] for r in poly]
    if t == "LineString":
        return [geom["coordinates"]]
    if t == "MultiLineString":
        return list(geom["coordinates"])
    return []

def load_lines(path, tol, min_area=0.0, keep=lambda f: True):
    out = []
    with open(path, encoding="utf-8") as f:
        gj = json.load(f)
    for feat in gj["features"]:
        if not keep(feat):
            continue
        for ring in rings_of(feat["geometry"]):
            pts = [(float(p[0]), float(p[1])) for p in ring]
            if min_area > 0 and ring_area(pts) < min_area:
                continue
            s = simplify(pts, tol)
            if len(s) >= 2:
                out.append(s)
    return out

def pack_lines(lines):
    b = [struct.pack("<I", len(lines))]
    for pts in lines:
        b.append(struct.pack("<I", len(pts)))
        b.append(struct.pack("<%df" % (2 * len(pts)), *[c for p in pts for c in p]))
    return b"".join(b)

def pstr(s, n=60):
    e = s.encode("utf-8")[:n]
    return struct.pack("<B", len(e)) + e

def main():
    tol = float(arg("--tol", "0.003"))
    cache = arg("--cache", os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache"))
    out = arg("--out", os.path.join(os.path.dirname(os.path.abspath(__file__)), "world.spearmap"))
    os.makedirs(cache, exist_ok=True)

    land = load_lines(fetch("land", cache), tol, min_area=tol * tol * 4)
    lakes = load_lines(fetch("lakes", cache), tol, min_area=tol * tol * 16)
    borders = load_lines(fetch("borders", cache), tol)
    admin1 = load_lines(fetch("admin1", cache), tol, keep=lambda f: (f["properties"].get("ADM0_A3") or f["properties"].get("adm0_a3")) == "JPN")

    places = []
    with open(fetch("places", cache), encoding="utf-8") as f:
        for feat in json.load(f)["features"]:
            p = feat["properties"]; lon, lat = feat["geometry"]["coordinates"]
            pop = int(p.get("pop_max") or 0)
            jp = p.get("adm0_a3") == "JPN"
            if pop >= 200000 or (jp and pop >= 30000) or p.get("featurecla", "").startswith("Admin-0 capital"):
                places.append((lon, lat, pop, int(p.get("scalerank") or 10), p.get("name") or ""))

    airports, kept = [], set()
    kinds = {"large_airport": 0, "medium_airport": 1, "small_airport": 2}
    with open(fetch("airports", cache), encoding="utf-8") as f:
        for r in csv.DictReader(f):
            k = kinds.get(r["type"])
            if k is None:
                continue
            jp = r["iso_country"] == "JP"
            if k <= 1 or (jp and r["scheduled_service"] == "yes"):
                airports.append((float(r["longitude_deg"]), float(r["latitude_deg"]), k, r["ident"], r["name"]))
                kept.add(r["ident"])
    runways = []
    with open(fetch("runways", cache), encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["airport_ident"] not in kept or r["closed"] == "1":
                continue
            try:
                a = (float(r["le_longitude_deg"]), float(r["le_latitude_deg"]))
                b = (float(r["he_longitude_deg"]), float(r["he_latitude_deg"]))
                ln = float(r["length_ft"] or 0) * 0.3048
            except ValueError:
                continue
            runways.append((a[0], a[1], b[0], b[1], ln))

    blob = [b"SPMAP1\0\0", pack_lines(land), pack_lines(lakes), pack_lines(borders), pack_lines(admin1)]
    blob.append(struct.pack("<I", len(places)))
    for lon, lat, pop, rank, name in places:
        blob.append(struct.pack("<ffIB", lon, lat, pop, min(rank, 255)) + pstr(name))
    blob.append(struct.pack("<I", len(airports)))
    for lon, lat, k, ident, name in airports:
        blob.append(struct.pack("<ffB", lon, lat, k) + pstr(ident, 8) + pstr(name))
    blob.append(struct.pack("<I", len(runways)))
    for rw in runways:
        blob.append(struct.pack("<5f", *rw))
    data = b"".join(blob)
    with open(out, "wb") as f:
        f.write(data)
    npts = sum(len(l) for l in land)
    print("land %d rings %d pts, lakes %d, borders %d, admin1(JP) %d, places %d, airports %d, runways %d -> %s (%.1f MB)" %
          (len(land), npts, len(lakes), len(borders), len(admin1), len(places), len(airports), len(runways), out, len(data) / 1e6))

if __name__ == "__main__":
    main()

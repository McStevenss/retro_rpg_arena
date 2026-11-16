// Map loading and spatial queries. See maps/arena.map for the file format.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>  // GetModuleFileNameW (NOMINMAX/WIN32_LEAN_AND_MEAN set in CMake)
#endif

#include "game.h"

namespace game {

namespace {

struct SpawnPos {
    float x, y, z;
};

struct TorchDef {
    int8_t dx, dz;
    float y;
};

struct MapData {
    std::string name;
    uint32_t hash = 0;
    int w = 0, h = 0;
    std::vector<Column> cols;
    std::vector<SpawnPos> spawns;
    std::vector<Torch> torches;
};

MapData g_map;

Column makeSolidColumn() {
    Column c{};
    c.numSpans = 1;
    c.spans[0] = {WORLD_BOTTOM, WORLD_TOP};
    return c;
}
const Column kSolid = makeSolidColumn();

uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char ch : s) {
        h ^= ch;
        h *= 16777619u;
    }
    return h;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

struct ColumnDef {
    Column col{};
    bool spawn = false;
    float spawnY = 0;
    std::vector<TorchDef> torches;
};

bool parseDir(const std::string& dir, int8_t& dx, int8_t& dz) {
    dx = dz = 0;
    if (dir == "n") dz = -1;
    else if (dir == "s") dz = 1;
    else if (dir == "e") dx = 1;
    else if (dir == "w") dx = -1;
    else return false;
    return true;
}

// Reads an optional number; leaves the stream untouched if the next token isn't one.
bool optionalNumber(std::istringstream& in, float& out) {
    auto pos = in.tellg();
    if (in >> out) return true;
    in.clear();
    in.seekg(pos);
    return false;
}

bool parseColumn(const std::string& spec, ColumnDef& out, std::string& err) {
    std::istringstream in(spec);
    std::string tok;
    bool solid = false, spawnHasY = false;
    float floorH = 0, ceilH = 3;
    std::vector<Span> slabs;
    Column c{};
    while (in >> tok) {
        if (tok == "solid") {
            solid = true;
        } else if (tok == "floor") {
            if (!(in >> floorH)) { err = "'floor' needs a height"; return false; }
        } else if (tok == "ceil") {
            if (!(in >> ceilH)) { err = "'ceil' needs a height"; return false; }
        } else if (tok == "slab") {
            float a, b;
            if (!(in >> a >> b) || b <= a) { err = "'slab' needs bottom and top heights"; return false; }
            slabs.push_back({a, b});
        } else if (tok == "stairs") {
            std::string dir;
            float h0, h1;
            if (!(in >> dir >> h0 >> h1)) { err = "'stairs' needs a direction and two heights"; return false; }
            Ramp r{0, 0, h0, h1};
            if (!parseDir(dir, r.dx, r.dz)) { err = "stairs direction must be n, s, e or w"; return false; }
            c.hasRamp = 1;
            c.ramp = r;
            floorH = h0;
        } else if (tok == "spawn") {
            out.spawn = true;
            spawnHasY = optionalNumber(in, out.spawnY);
        } else if (tok == "torch") {
            std::string dir;
            TorchDef t{0, 0, 2.0f};
            if (!(in >> dir) || !parseDir(dir, t.dx, t.dz)) { err = "'torch' needs a wall direction (n, s, e or w)"; return false; }
            optionalNumber(in, t.y);
            out.torches.push_back(t);
        } else {
            err = "unknown keyword '" + tok + "'";
            return false;
        }
    }
    if (solid) {
        out.col = kSolid;
        return true;
    }

    std::vector<Span> spans;
    spans.push_back({WORLD_BOTTOM, floorH});
    spans.insert(spans.end(), slabs.begin(), slabs.end());
    spans.push_back({ceilH, WORLD_TOP});
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.y0 < b.y0; });
    for (size_t i = 1; i < spans.size(); ++i)
        if (spans[i].y0 < spans[i - 1].y1) { err = "solid layers overlap"; return false; }
    if (spans.size() > MAX_SPANS) { err = "too many layers in one column"; return false; }
    if (c.hasRamp && std::max(c.ramp.h0, c.ramp.h1) >= ceilH) { err = "stairs go through the ceiling"; return false; }

    c.numSpans = static_cast<uint8_t>(spans.size());
    std::copy(spans.begin(), spans.end(), c.spans);
    out.col = c;
    if (!spawnHasY) out.spawnY = floorH;
    return true;
}

// Ramp surface height at the uphill-most point of the rectangle (clipped to the tile).
float rampMaxOver(const Ramp& r, int tx, int tz, float x0, float x1, float z0, float z1) {
    const float tX0 = tx * TILE, tZ0 = tz * TILE;
    float t;
    if (r.dx > 0) t = (std::min(x1, tX0 + TILE) - tX0) / TILE;
    else if (r.dx < 0) t = (tX0 + TILE - std::max(x0, tX0)) / TILE;
    else if (r.dz > 0) t = (std::min(z1, tZ0 + TILE) - tZ0) / TILE;
    else t = (tZ0 + TILE - std::max(z0, tZ0)) / TILE;
    t = std::clamp(t, 0.0f, 1.0f);
    return r.h0 + (r.h1 - r.h0) * t;
}

}  // namespace

bool loadMap(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();

    MapData m;
    m.hash = fnv1a(text);
    m.name = std::filesystem::path(path).stem().string();

    std::map<char, ColumnDef> legend;
    std::vector<std::string> grid;
    enum { NONE, LEGEND, GRID } section = NONE;
    std::istringstream lines(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(lines, line)) {
        ++lineNo;
        std::string t = trim(line);
        if (t.empty() || t.rfind("//", 0) == 0) continue;
        if (t == "[legend]") { section = LEGEND; continue; }
        if (t == "[grid]") { section = GRID; continue; }
        auto fail = [&](const std::string& msg) {
            err = path + ":" + std::to_string(lineNo) + ": " + msg;
            return false;
        };
        if (section == LEGEND) {
            ColumnDef def;
            std::string e;
            if (!parseColumn(t.substr(1), def, e)) return fail(e);
            legend[t[0]] = def;
        } else if (section == GRID) {
            if (!grid.empty() && t.size() != grid[0].size()) return fail("grid rows must all be the same width");
            grid.push_back(t);
        } else {
            return fail("expected [legend] or [grid]");
        }
    }
    if (grid.empty()) { err = path + ": no [grid] section"; return false; }

    m.w = static_cast<int>(grid[0].size());
    m.h = static_cast<int>(grid.size());
    m.cols.resize(static_cast<size_t>(m.w) * m.h);
    for (int z = 0; z < m.h; ++z) {
        for (int x = 0; x < m.w; ++x) {
            auto it = legend.find(grid[z][x]);
            if (it == legend.end()) {
                err = path + ": tile '" + std::string(1, grid[z][x]) + "' at column " + std::to_string(x) +
                      ", row " + std::to_string(z) + " is not in the legend";
                return false;
            }
            m.cols[static_cast<size_t>(z) * m.w + x] = it->second.col;
            if (it->second.spawn) m.spawns.push_back({(x + 0.5f) * TILE, it->second.spawnY, (z + 0.5f) * TILE});
            for (const TorchDef& t : it->second.torches) {
                const float off = TILE * 0.5f - 0.2f;  // just off the wall surface
                m.torches.push_back({(x + 0.5f) * TILE + t.dx * off, t.y, (z + 0.5f) * TILE + t.dz * off, t.dx, t.dz});
            }
        }
    }
    if (m.spawns.empty()) { err = path + ": map has no spawn points"; return false; }
    g_map = std::move(m);
    return true;
}

std::string findMapFile(const std::string& nameOrPath, const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_regular_file(nameOrPath, ec)) return nameOrPath;

    const std::string file = nameOrPath + ".map";
    std::vector<fs::path> dirs = {"maps"};
    fs::path exe;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) exe = fs::path(buf, buf + len);
#else
    exe = fs::read_symlink("/proc/self/exe", ec);
#endif
    if (exe.empty() && argv0) exe = fs::absolute(argv0, ec);
    if (!exe.empty()) {
        dirs.push_back(exe.parent_path() / "maps");
        dirs.push_back(exe.parent_path().parent_path() / "maps");
    }
#ifdef BOYZ_SOURCE_MAPS_DIR
    dirs.push_back(BOYZ_SOURCE_MAPS_DIR);
#endif
    for (const auto& d : dirs) {
        fs::path p = d / file;
        if (fs::is_regular_file(p, ec)) return p.string();
    }
    return "";
}

const std::string& mapName() { return g_map.name; }
uint32_t mapHash() { return g_map.hash; }
int mapWidth() { return g_map.w; }
int mapHeight() { return g_map.h; }

const Column& column(int tx, int tz) {
    if (tx < 0 || tz < 0 || tx >= g_map.w || tz >= g_map.h) return kSolid;
    return g_map.cols[static_cast<size_t>(tz) * g_map.w + tx];
}

int solidIntervals(int tx, int tz, float x0, float x1, float z0, float z1, Span* out) {
    const Column& c = column(tx, tz);
    int n = 0;
    for (int i = 0; i < c.numSpans; ++i) out[n++] = c.spans[i];
    if (c.hasRamp) out[n++] = {c.ramp.h0, rampMaxOver(c.ramp, tx, tz, x0, x1, z0, z1)};
    return n;
}

bool pointInWall(float x, float y, float z) {
    int tx = static_cast<int>(std::floor(x / TILE)), tz = static_cast<int>(std::floor(z / TILE));
    Span iv[MAX_SPANS + 1];
    int n = solidIntervals(tx, tz, x, x, z, z, iv);
    for (int i = 0; i < n; ++i)
        if (y > iv[i].y0 && y < iv[i].y1) return true;
    return false;
}

bool lineOfSight(float x0, float y0, float z0, float x1, float y1, float z1) {
    float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
    float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    int steps = std::max(1, static_cast<int>(len / 0.2f));
    for (int i = 1; i < steps; ++i) {
        float t = static_cast<float>(i) / steps;
        if (pointInWall(x0 + dx * t, y0 + dy * t, z0 + dz * t)) return false;
    }
    return true;
}

int spawnCount() { return static_cast<int>(g_map.spawns.size()); }

void spawnPoint(int index, float& x, float& y, float& z) {
    const SpawnPos& s = g_map.spawns[static_cast<size_t>(index) % g_map.spawns.size()];
    x = s.x;
    y = s.y;
    z = s.z;
}

int torchCount() { return static_cast<int>(g_map.torches.size()); }
const Torch& torchAt(int index) { return g_map.torches[static_cast<size_t>(index)]; }

}  // namespace game

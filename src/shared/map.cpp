// Hardcoded test arena. '#' = wall, '.' = floor, 'S' = spawn point.
// Later: load from file / a proper level format with heights, doors, etc.
#include "game.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace game {

static const char* kMap[] = {
    "########################",
    "#S.....#.......#......S#",
    "#......#.......#.......#",
    "#..##..#...##..#..###..#",
    "#..##......##.....#....#",
    "#......#.......#..#....#",
    "###.####..#.#..##.##.###",
    "#......................#",
    "#.S....##....##......S.#",
    "#......................#",
    "###.####..#.#..##.##.###",
    "#......#.......#.......#",
    "#..##..#...S...#..##...#",
    "#..##......##......#...#",
    "#S.....#.......#......S#",
    "########################",
};

static const int kH = static_cast<int>(sizeof(kMap) / sizeof(kMap[0]));
static const int kW = static_cast<int>(std::strlen(kMap[0]));

static const std::vector<int>& spawns() {
    static std::vector<int> s = [] {
        std::vector<int> v;
        for (int z = 0; z < kH; ++z) {
            if (static_cast<int>(std::strlen(kMap[z])) != kW) {
                std::fprintf(stderr, "map row %d has wrong width\n", z);
                std::abort();
            }
            for (int x = 0; x < kW; ++x)
                if (kMap[z][x] == 'S') v.push_back(z * kW + x);
        }
        return v;
    }();
    return s;
}

int mapWidth() { return kW; }
int mapHeight() { return kH; }

bool isSolid(int tx, int tz) {
    if (tx < 0 || tz < 0 || tx >= kW || tz >= kH) return true;
    return kMap[tz][tx] == '#';
}

int spawnCount() { return static_cast<int>(spawns().size()); }

void spawnPoint(int index, float& x, float& z) {
    const auto& s = spawns();
    int t = s[index % s.size()];
    x = (t % kW + 0.5f) * TILE;
    z = (t / kW + 0.5f) * TILE;
}

}  // namespace game

// UI3D 2026-10-31 offline tests (docs/world-render/ui-3d.md section 14): stages 2-3 of the 3D reinterpretation of the menus ("glass and
// amber"): panel groups on their own curved planes (u3gp), shaded bevels (u3bv), raised text (u3tx), the selection raised in front of its pane
// (u3sr), pane back faces (u3pb). Pure parts (ui3d_groups.h, the Surf refactor of ui3d.h) and the PRODUCER (brief3d_live.h) with the fakes of
// ui3d_panel_offline_tests.cpp, fed with REAL overlay geometry from the censuses:
//   * step124 runD1 ee-camera.log, s=10, ord 1..48 (the main menu, head of the frame: backdrop, strips, tiles, the selection box, glyph rows);
//   * step147 batchAG-ui runAGU1 ee-camera.log (census mode=tail), s=10 ord 323..347 (the main menu's tail: tile, glyph rows, the header bar,
//     the z-0 dim) and s=45 (a 199-draw menu: a panel box drawn AFTER the text it covers);
//   * step124 runK10 ee-camera.log, ticks 127156569-570, s=8, all=154..187 (the PLAYER weapon hangar overlay, as ui3d_select_offline).
// Nothing here touches D3D12, the game or a headset: fixture success says nothing about what the user will see.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui3d_groups.h"
#include "ui3d_hangar.h"
#include "brief3d_live.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz::brief3d::live;
namespace u3 = acz::ui3d;
namespace gp = acz::ui3d::groups;
namespace hz = acz::ui3d::hangar;
using acz::vscreen::Completion;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool streq(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }
static unsigned long long field(const std::string& s, const char* key) {
    const size_t at = s.find(key);
    if (at == std::string::npos) return ~0ull;
    return std::strtoull(s.c_str() + at + std::strlen(key), nullptr, 10);
}

namespace {

// ================================================================================================== real census geometry
struct RealDraw { int all; float v[4][3]; };   // census `v=` x,y,z per vertex in the logged order (q = 1 everywhere in these frames)
// step124 runD1 s=10 ord 1..48 (main menu, head)
const RealDraw kD1[] = {
    {1, {{1792, 1824, 64}, {1792, 2272, 64}, {2304, 1824, 64}, {2304, 2272, 64}}},   // backdrop (full 4:3 area)
    {2, {{1792, 1824, 64}, {1792, 2272, 64}, {2304, 1824, 64}, {2304, 2272, 64}}},
    {3, {{1792, 1823, 64}, {1792, 2271, 64}, {2304, 1823, 64}, {2304, 2271, 64}}},
    {4, {{1792, 1824, 64}, {1792, 2272, 64}, {2304, 1824, 64}, {2304, 2272, 64}}},
    {5, {{1792, 1824, 64}, {1792, 2272, 64}, {2304, 1824, 64}, {2304, 2272, 64}}},
    {6, {{2265, 1887, 64}, {2265, 2047, 64}, {2281, 1887, 64}, {2281, 2047, 64}}},   // panel strips (0.023 x 0.36)
    {7, {{2259, 1898, 64}, {2259, 2166, 64}, {2275, 1898, 64}, {2275, 2166, 64}}},
    {8, {{2006, 1899, 64}, {2006, 1915, 64}, {2022, 1899, 64}, {2022, 1915, 64}}},   // small z-64 decorations
    {9, {{2006, 1914, 64}, {2006, 1930, 64}, {2022, 1914, 64}, {2022, 1930, 64}}},
    {10, {{2006, 1929, 64}, {2006, 1945, 64}, {2022, 1929, 64}, {2022, 1945, 64}}},
    {11, {{2006, 1944, 64}, {2006, 1960, 64}, {2022, 1944, 64}, {2022, 1960, 64}}},
    {12, {{2006, 1966, 64}, {2006, 1966, 64}, {2022, 1966, 64}, {2022, 1966, 64}}},  // zero-height quads (rasterise nothing)
    {13, {{2006, 1981, 64}, {2006, 1981, 64}, {2022, 1981, 64}, {2022, 1981, 64}}},
    {14, {{2006, 2010, 64}, {2006, 2010, 64}, {2022, 2010, 64}, {2022, 2010, 64}}},
    {15, {{2006, 1995, 64}, {2006, 1995, 64}, {2022, 1995, 64}, {2022, 1995, 64}}},
    {16, {{2006, 1980, 64}, {2006, 1980, 64}, {2022, 1980, 64}, {2022, 1980, 64}}},
    {17, {{2006, 1965, 64}, {2006, 1965, 64}, {2022, 1965, 64}, {2022, 1965, 64}}},
    {18, {{2006, 1996, 64}, {2006, 1996, 64}, {2022, 1996, 64}, {2022, 1996, 64}}},
    {19, {{2006, 2011, 64}, {2006, 2011, 64}, {2022, 2011, 64}, {2022, 2011, 64}}},
    {20, {{2006, 2026, 64}, {2006, 2026, 64}, {2022, 2026, 64}, {2022, 2026, 64}}},
    {21, {{2006, 2041, 64}, {2006, 2041, 64}, {2022, 2041, 64}, {2022, 2041, 64}}},
    {22, {{2006, 2056, 64}, {2006, 2056, 64}, {2022, 2056, 64}, {2022, 2056, 64}}},
    {23, {{2006, 2071, 64}, {2006, 2071, 64}, {2022, 2071, 64}, {2022, 2071, 64}}},
    {24, {{2006, 2086, 64}, {2006, 2086, 64}, {2022, 2086, 64}, {2022, 2086, 64}}},
    {25, {{2006, 2101, 64}, {2006, 2101, 64}, {2022, 2101, 64}, {2022, 2101, 64}}},
    {26, {{1809, 1842, 64}, {1809, 1874, 64}, {1841, 1842, 64}, {1841, 1874, 64}}},   // tiles (0.047 x 0.071)
    {27, {{1860, 1842, 64}, {1860, 1874, 64}, {1892, 1842, 64}, {1892, 1874, 64}}},
    {28, {{1814, 1881, 64}, {1814, 1945, 64}, {2014, 1881, 64}, {2014, 1945, 64}}},   // the selection box (0.293 x 0.143)
    {29, {{1824, 1902, 1}, {1834, 1902, 1}, {1824, 1926, 1}, {1834, 1926, 1}}},       // glyph row 1 (inside the selection box)
    {30, {{1834, 1902, 1}, {1845, 1902, 1}, {1834, 1926, 1}, {1845, 1926, 1}}},
    {31, {{1845, 1902, 1}, {1856, 1902, 1}, {1845, 1926, 1}, {1856, 1926, 1}}},
    {32, {{1856, 1902, 1}, {1867, 1902, 1}, {1856, 1926, 1}, {1867, 1926, 1}}},
    {33, {{1867, 1902, 1}, {1878, 1902, 1}, {1867, 1926, 1}, {1878, 1926, 1}}},
    {34, {{1878, 1902, 1}, {1885, 1902, 1}, {1878, 1926, 1}, {1885, 1926, 1}}},
    {35, {{1886, 1902, 1}, {1897, 1902, 1}, {1886, 1926, 1}, {1897, 1926, 1}}},
    {36, {{1897, 1902, 1}, {1908, 1902, 1}, {1897, 1926, 1}, {1908, 1926, 1}}},
    {37, {{1824, 1932, 1}, {1835, 1932, 1}, {1824, 1956, 1}, {1835, 1956, 1}}},       // glyph row 2 (centre 1944: inside the box, 1945)
    {38, {{1835, 1932, 1}, {1846, 1932, 1}, {1835, 1956, 1}, {1846, 1956, 1}}},
    {39, {{1846, 1932, 1}, {1851, 1932, 1}, {1846, 1938, 1}, {1851, 1938, 1}}},
    {40, {{1851, 1932, 1}, {1862, 1932, 1}, {1851, 1956, 1}, {1862, 1956, 1}}},
    {41, {{1862, 1932, 1}, {1873, 1932, 1}, {1862, 1956, 1}, {1873, 1956, 1}}},
    {42, {{1873, 1932, 1}, {1884, 1932, 1}, {1873, 1956, 1}, {1884, 1956, 1}}},
    {43, {{1884, 1932, 1}, {1895, 1932, 1}, {1884, 1956, 1}, {1895, 1956, 1}}},
    {44, {{1824, 1962, 1}, {1835, 1962, 1}, {1824, 1986, 1}, {1835, 1986, 1}}},       // glyph row 3 (outside the box)
    {45, {{1835, 1962, 1}, {1846, 1962, 1}, {1835, 1986, 1}, {1846, 1986, 1}}},
    {46, {{1846, 1962, 1}, {1856, 1962, 1}, {1846, 1986, 1}, {1856, 1986, 1}}},
    {47, {{1856, 1962, 1}, {1863, 1962, 1}, {1856, 1986, 1}, {1863, 1986, 1}}},
    {48, {{1864, 1962, 1}, {1875, 1962, 1}, {1864, 1986, 1}, {1875, 1986, 1}}},
};
// step147 runAGU1 s=10 ord 323..347 (main menu, tail; census mode=tail)
const RealDraw kAguMain[] = {
    {323, {{1818, 1880, 64}, {1818, 1896, 64}, {1834, 1880, 64}, {1834, 1896, 64}}},   // a small z-64 tile
    {324, {{1837, 1883, 1}, {1844, 1883, 1}, {1837, 1899, 1}, {1844, 1899, 1}}},
    {325, {{1844, 1883, 1}, {1850, 1883, 1}, {1844, 1899, 1}, {1850, 1899, 1}}},
    {326, {{1851, 1883, 1}, {1856, 1883, 1}, {1851, 1899, 1}, {1856, 1899, 1}}},
    {327, {{1856, 1883, 1}, {1862, 1883, 1}, {1856, 1899, 1}, {1862, 1899, 1}}},
    {328, {{1863, 1883, 1}, {1867, 1883, 1}, {1863, 1899, 1}, {1867, 1899, 1}}},
    {333, {{1837, 1848, 1}, {1846, 1848, 1}, {1837, 1868, 1}, {1846, 1868, 1}}},
    {334, {{1846, 1848, 1}, {1856, 1848, 1}, {1846, 1868, 1}, {1856, 1868, 1}}},
    {335, {{1856, 1848, 1}, {1868, 1848, 1}, {1856, 1864, 1}, {1868, 1864, 1}}},
    {340, {{1889, 1848, 1}, {1899, 1848, 1}, {1889, 1868, 1}, {1899, 1868, 1}}},
    {346, {{1792, 1644, 64}, {1792, 1772, 64}, {2304, 1644, 64}, {2304, 1772, 64}}},   // the header bar (0.751 x 0.286), after the text
    {347, {{128, 256, 0}, {2688, 256, 0}, {128, 1696, 0}, {2688, 1696, 0}}},           // the z-0 full-screen dim
};
// step147 runAGU1 s=45 (a 199-draw menu): the box 1792..2304 x 1844..1972 is drawn AFTER the glyph rows it covers
const RealDraw kAgu45[] = {
    {164, {{1818, 1880, 64}, {1818, 1896, 64}, {1834, 1880, 64}, {1834, 1896, 64}}},
    {165, {{1837, 1883, 1}, {1844, 1883, 1}, {1837, 1899, 1}, {1844, 1899, 1}}},
    {166, {{1844, 1883, 1}, {1850, 1883, 1}, {1844, 1899, 1}, {1850, 1899, 1}}},
    {167, {{1851, 1883, 1}, {1856, 1883, 1}, {1851, 1899, 1}, {1856, 1899, 1}}},
    {185, {{1837, 1848, 1}, {1846, 1848, 1}, {1837, 1868, 1}, {1846, 1868, 1}}},
    {186, {{1846, 1848, 1}, {1856, 1848, 1}, {1846, 1868, 1}, {1856, 1868, 1}}},
    {187, {{1856, 1848, 1}, {1868, 1848, 1}, {1856, 1864, 1}, {1868, 1864, 1}}},
    {198, {{1792, 1844, 64}, {1792, 1972, 64}, {2304, 1844, 64}, {2304, 1972, 64}}},
    {199, {{128, 256, 0}, {2688, 256, 0}, {128, 1696, 0}, {2688, 1696, 0}}},
};
// step124 runK10 s=8 all=154..187 (PLAYER weapon hangar overlay)
const RealDraw kK10[] = {
    {154, {{1768, 1808, 64}, {1768, 2287, 64}, {2328, 1808, 64}, {2328, 2287, 64}}},   // 0.821 x 1.0 additive (tint)
    {155, {{1792, 1824, 64}, {1792, 2272, 64}, {2304, 1824, 64}, {2304, 2272, 64}}},   // 0.751 x 1.0 (tint / grid / frame)
    {156, {{1792, 1824, 64}, {1792, 2272, 64}, {2304, 1824, 64}, {2304, 2272, 64}}},
    {157, {{1924, 2096, 64}, {1924, 2176, 64}, {2004, 2096, 64}, {2004, 2176, 64}}},   // 0.117 x 0.179: INSIDE [STATISTICS] (nested)
    {158, {{1780, 2065, 64}, {1780, 2193, 64}, {2036, 2065, 64}, {2036, 2193, 64}}},   // 0.375 x 0.286 [STATISTICS] panel
    {159, {{1806, 1858, 64}, {1806, 1986, 64}, {2062, 1858, 64}, {2062, 1986, 64}}},   // 0.375 x 0.286 [SP WEAPON NAME] panel
    {160, {{2152, 2092, 64}, {2152, 2188, 64}, {2288, 2092, 64}, {2288, 2188, 64}}},   // 0.199 x 0.214 [WEAPONS] panel
    {161, {{2168, 1878, 64}, {2168, 1910, 64}, {2296, 1878, 64}, {2296, 1910, 64}}},   // 0.188 x 0.071 PLAYER box
    {162, {{1809, 1834, 64}, {1809, 1866, 64}, {1841, 1834, 64}, {1841, 1866, 64}}},   // 0.047 x 0.071 button tiles
    {163, {{1860, 1834, 64}, {1860, 1866, 64}, {1892, 1834, 64}, {1892, 1866, 64}}},
    {164, {{1952, 1834, 64}, {1952, 1866, 64}, {1984, 1834, 64}, {1984, 1866, 64}}},
    {165, {{1820, 1889, 1}, {1826, 1889, 1}, {1820, 1905, 1}, {1826, 1905, 1}}},       // glyphs "[SP WEAPON NAME]" (z 1)
    {166, {{1826, 1889, 1}, {1832, 1889, 1}, {1826, 1905, 1}, {1832, 1905, 1}}},
    {167, {{1832, 1889, 1}, {1838, 1889, 1}, {1832, 1905, 1}, {1838, 1905, 1}}},
    {168, {{1839, 1889, 1}, {1842, 1889, 1}, {1839, 1893, 1}, {1842, 1893, 1}}},
    {169, {{1843, 1889, 1}, {1853, 1889, 1}, {1843, 1905, 1}, {1853, 1905, 1}}},
    {170, {{1853, 1889, 1}, {1859, 1889, 1}, {1853, 1905, 1}, {1859, 1905, 1}}},
    {171, {{1860, 1889, 1}, {1867, 1889, 1}, {1860, 1905, 1}, {1867, 1905, 1}}},
    {172, {{1868, 1889, 1}, {1874, 1889, 1}, {1868, 1905, 1}, {1874, 1905, 1}}},
    {173, {{1874, 1889, 1}, {1881, 1889, 1}, {1874, 1905, 1}, {1881, 1905, 1}}},
    {174, {{1882, 1889, 1}, {1889, 1889, 1}, {1882, 1905, 1}, {1889, 1905, 1}}},
    {175, {{1890, 1889, 1}, {1893, 1889, 1}, {1890, 1893, 1}, {1893, 1893, 1}}},
    {176, {{1893, 1889, 1}, {1900, 1889, 1}, {1893, 1905, 1}, {1900, 1905, 1}}},
    {177, {{1901, 1889, 1}, {1908, 1889, 1}, {1901, 1905, 1}, {1908, 1905, 1}}},
    {178, {{1909, 1889, 1}, {1916, 1889, 1}, {1909, 1905, 1}, {1916, 1905, 1}}},
    {179, {{1916, 1889, 1}, {1922, 1889, 1}, {1916, 1905, 1}, {1922, 1905, 1}}},
    {180, {{1923, 1889, 1}, {1930, 1889, 1}, {1923, 1901, 1}, {1930, 1901, 1}}},
    {181, {{1820, 1901, 1}, {1832, 1901, 1}, {1820, 1933, 1}, {1832, 1933, 1}}},       // glyphs "SAAM" (0.018 x 0.071)
    {182, {{1832, 1901, 1}, {1844, 1901, 1}, {1832, 1933, 1}, {1844, 1933, 1}}},
    {183, {{1845, 1901, 1}, {1857, 1901, 1}, {1845, 1933, 1}, {1857, 1933, 1}}},
    {184, {{1858, 1901, 1}, {1873, 1901, 1}, {1858, 1933, 1}, {1873, 1933, 1}}},
    {185, {{1820, 1928, 1}, {1828, 1928, 1}, {1820, 1952, 1}, {1828, 1952, 1}}},       // glyphs "SEMI-ACTIVE"...
    {186, {{1828, 1928, 1}, {1836, 1928, 1}, {1828, 1952, 1}, {1836, 1952, 1}}},
    {187, {{1837, 1928, 1}, {1845, 1928, 1}, {1837, 1952, 1}, {1845, 1952, 1}}},
};
template <size_t N> constexpr size_t count(const RealDraw (&)[N]) { return N; }

// ================================================================================================== fakes (as ui3d_panel_offline_tests.cpp)
const char* kRealA = "808.52179,1085.18433,-142.494934,0.558786631,-443.357697,-909.892456,55.2049141,-0.21648325,1873.98059,1554.71289,-204.148453,0.800557733,258920.094,294018.25,16740630,144.465683";
void parse16(const char* s, float out[16]) {
    char* e = nullptr; const char* p = s;
    for (int i = 0; i < 16; ++i) { out[i] = std::strtof(p, &e); CHECK(e != p); p = e; if (*p == ',') ++p; }
}
void real_record(uint8_t rec[hz::kRecBytes]) {
    float A[16]; parse16(kRealA, A);
    for (unsigned i = 0; i < hz::kRecBytes; ++i) rec[i] = uint8_t(i * 7 + 3);
    std::memcpy(rec, A, 64);
    const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(rec + 256, I, 64);
    const float one = 1.0f; std::memcpy(rec + 360, &one, 4);
    std::memcpy(rec + 400, hz::kScreen, 16);
}
Completion done(uint64_t v) { Completion c; c.value = v; return c; }
const uint64_t kUploadVa = 0x40000000ull;
int g_ui_rt = 0;
acz::brief3d::EyeView make_eye(double yaw, double ipd) {
    acz::brief3d::EyeView e;
    e.orientation[0] = 0; e.orientation[1] = std::sin(yaw / 2); e.orientation[2] = 0; e.orientation[3] = std::cos(yaw / 2);
    e.position[0] = ipd * std::cos(yaw); e.position[1] = 1.6; e.position[2] = -ipd * std::sin(yaw);
    e.fov[0] = -0.785; e.fov[1] = 0.785; e.fov[2] = 0.785; e.fov[3] = -0.785;
    return e;
}
PoseIn pose() { PoseIn p; p.ok = true; p.space_generation = 1; p.eyes[0] = make_eye(0.0, -0.032); p.eyes[1] = make_eye(0.0, 0.032); return p; }
void screen() {
    acz::vscreen::Options o; const float head[3] = {0.0f, 1.6f, 0.0f}, q[4] = {0, 0, 0, 1};
    acz::vscreen::publish_placement(acz::vscreen::place(o, head, q, 16.0f / 9.0f, 0), head, 1);
}
Producer& configure(Producer& p, const u3::Options& o) { p.configure_ui3d(o); p.gpu_ready(kUploadVa); screen(); return p; }
AcmFacts acm(const void* list, int64_t now, const uint8_t* rec) {
    AcmFacts f; f.list = list; f.now_ms = now; f.indexed = true; f.topology_type = 3; f.instances = 1;
    f.rt = &g_ui_rt; f.rt_w = 2560; f.rt_h = 1440; f.rt_fmt = kRtvFormat;
    f.pso_known = true; f.rt_count = 1; f.rtv0 = kRtvFormat; f.dsv = kDsvFormat; f.samples = 1;
    f.depth_enable = true; f.depth_write = true; f.depth_func = acz::brief3d::kCmpGreater; f.stencil = true; f.stencil_always = true;
    f.rs_known = true; f.viewports = 1; f.scissors = 1; f.om_known = true;
    f.t10_why = nullptr; f.t10_param = 5; f.t10_va = 0x30be00000ull; std::memcpy(f.rec, rec, hz::kRecBytes);
    f.cb_why = nullptr; f.cb10_0 = 2;
    return f;
}
// colour (0.5, 0.5, 0.5, 1) per vertex as the fixtures of ui3d_panel_offline; texcoord the strip corner
UiFacts ps2(const void* list, int64_t now, float x0, float y0, float x1, float y1, float z) {
    UiFacts f;
    f.list = list; f.now_ms = now; f.vs8 = u3::kPs2Vs8; f.vertices = 4; f.instances = 1; f.topology_type = 3;
    f.rt = &g_ui_rt; f.rt_w = 2560; f.rt_h = 1440; f.rt_fmt = kRtvFormat;
    f.pso_known = true; f.rt_count = 1; f.rtv0 = kRtvFormat; f.dsv = kDsvFormat; f.samples = 1;
    f.depth_enable = true; f.depth_func = acz::brief3d::kCmpAlways; f.stencil = true; f.stencil_always = true;
    f.rs_known = true; f.viewports = 1; f.scissors = 1; f.vp[2] = 2560; f.vp[3] = 1440; f.sc[0] = 320; f.sc[2] = 2240; f.sc[3] = 1440;
    f.om_known = true; f.cb_why = nullptr; f.cb0 = u3::Cb0{682.0f, -448.0f, 2048.0f, 2048.0f};
    f.vb_why = nullptr; f.stride = 64;
    const float xy[4][2] = {{x0, y0}, {x0, y1}, {x1, y0}, {x1, y1}};
    for (int i = 0; i < 4; ++i) {
        float v[16] = {xy[i][0], xy[i][1], z, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f, float(i & 1), float(i >> 1), 0, 0, 0, 0, 0, 0};
        std::memcpy(f.vtx + 64 * i, v, 64);
    }
    return f;
}
UiFacts real_draw(const void* list, int64_t now, const RealDraw& r) {
    UiFacts f = ps2(list, now, 0, 0, 1, 1, 0);
    for (int i = 0; i < 4; ++i) {
        const float xyzq[4] = {r.v[i][0], r.v[i][1], r.v[i][2], 1.0f};
        std::memcpy(f.vtx + 64 * i, xyzq, 16);
    }
    return f;
}
struct Driver {
    Producer& p; int L = 0, Q = 0; uint64_t executed = 0;
    explicit Driver(Producer& pr) : p(pr) {}
    UiPlan ui(const UiFacts& f) {
        UiPlan pl = p.ui3d_draw(f, p.wants_poses(f.now_ms) ? pose() : PoseIn{}, done(executed));
        if (pl.record) p.ui3d_commit(pl);
        return pl;
    }
    AcmPlan acm_draw(const AcmFacts& f) {
        AcmPlan pl = p.ui3d_acm(f, p.wants_poses(f.now_ms) ? pose() : PoseIn{}, done(executed));
        if (pl.record) p.ui3d_acm_commit(pl);
        return pl;
    }
    unsigned end() {
        const void* ls[1] = {&L};
        const auto tok = p.before_execute(&Q, ls, 1);
        if (tok.signal) { p.after_execute(tok, true); executed = tok.value; }
        p.on_reset(&L, true);
        Held h; unsigned replays = 0;
        if (p.acquire(0, done(executed), h)) { replays = unsigned(h.meta.replays); p.release(h); }
        return replays;
    }
};
float fl(const uint8_t* v, unsigned vertex, unsigned k) { float f; std::memcpy(&f, v + 64 * vertex + 4 * k, 4); return f; }
u3::Options menu_lopts(const u3::Options& o) { u3::Options lo = o; lo.near_m = o.menu_near_m; lo.far_m = o.menu_far_m; return lo; }
u3::Screen menu_screen(const u3::Options& o) {
    screen();
    const acz::vscreen::SharedPlacement sp = acz::vscreen::read_placement();
    CHECK(sp.valid);
    return u3::scaled(u3::make_screen(sp.centre, sp.yaw, sp.width, sp.height, sp.anchor), double(o.scale));
}
const u3::SourceRect kFull = u3::rect_from_viewport(0, 0, 2560, 1440, 2560, 1440);
const u3::Cb0 kCb{682.0f, -448.0f, 2048.0f, 2048.0f};
// an eye AT the anchor, looking along the screen's forward (from here every surface must show the flat image)
u3::EyeView anchor_eye(const u3::Screen& s, double dx = 0.0, double dy = 0.0) {
    u3::EyeView e;
    for (int i = 0; i < 3; ++i) e.p[i] = s.anchor[i] + dx * s.right[i] + dy * s.up[i];
    const double yaw = std::atan2(-s.fwd[0], -s.fwd[2]);
    e.q[0] = 0; e.q[1] = std::sin(yaw / 2); e.q[2] = 0; e.q[3] = std::cos(yaw / 2);
    return e;
}
void vtx_of(const RealDraw& r, uint8_t out[4 * 64]) {
    for (int i = 0; i < 4; ++i) {
        float v[16] = {r.v[i][0], r.v[i][1], r.v[i][2], 1.0f, 0.5f, 0.5f, 0.5f, 1.0f, float(i & 1), float(i >> 1), 0, 0, 0, 0, 0, 0};
        std::memcpy(out + 64 * i, v, 64);
    }
}
u3::Box box_of(const RealDraw& r) { uint8_t v[4 * 64]; vtx_of(r, v); u3::Box b; CHECK(u3::screen_box(v, 4, kCb, kFull, b)); return b; }
bool is_frame_quad(const RealDraw& r) {
    uint8_t v[4 * 64]; vtx_of(r, v); double w = 0, h = 0; CHECK(u3::screen_bbox(v, 4, kCb, kFull, w, h));
    return u3::frame_quad(4, w, h, true, r.v[0][2]);
}
// The vertex of copy `c` of `copies` whose flat-image position it must have from the anchor: walls carry (front a, back a, front b, back b)
unsigned flat_vertex(unsigned copies, unsigned c, unsigned i) {
    const bool wall = (copies == u3::kRimCopies && c < 4) || (copies == u3::kSlabCopies && c >= 1 && c <= 4);
    if (!wall) return i;
    const unsigned e = copies == u3::kRimCopies ? c : c - 1;
    return i < 2 ? u3::kSlabEdges[e][0] : u3::kSlabEdges[e][1];
}
// every copy's vertex = the flat quad's vertex (x, y, q) from the anchor
void check_anchor(const uint8_t* copies_vtx, unsigned copies, const uint8_t* flat, double tol) {
    for (unsigned c = 0; c < copies; ++c)
        for (unsigned i = 0; i < 4; ++i) {
            const unsigned j = flat_vertex(copies, c, i);
            const uint8_t* q = copies_vtx + size_t(c) * 4 * 64;
            CHECK(std::fabs(fl(q, i, 0) - fl(flat, j, 0)) < tol);
            CHECK(std::fabs(fl(q, i, 1) - fl(flat, j, 1)) < tol);
            CHECK(std::fabs(fl(q, i, 3) - fl(flat, j, 3)) < 1e-4 * std::fabs(fl(flat, j, 3)) + 1e-6);
        }
}

// ================================================================================================== options
const std::string kUserCfg = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;"
                             "brief3d:1;fxsig:f1a4e000;ui3d:1;u3p:1;u3a:1;fxtrail:8;fxcloud:120;u3pp:1;u3lt:6;dbr3d:1;pov:2;povrig:level";
void test_options() {
    std::string rest, why; u3::Options o;
    CHECK(kUserCfg.size() == 224);   // the user's xr_terrain_stage value on 2026-10-31 (runtime.cfg)
    CHECK(u3::extract_options(kUserCfg, rest, o, why));
    CHECK(o.look == 0 && !o.groups && !o.reinterpret() && o.curve_k == 1.0f && o.nest_mm == 5.0f);
    CHECK(!has(u3::describe(o), "u3gp"));   // the default line is unchanged
    const std::string base_line = u3::describe(o);
    // the one-token look: the user's cfg + ";u3gp:3" fits the 255-character buffer (31 characters were left)
    const std::string cfg3 = kUserCfg + ";u3gp:3";
    CHECK(cfg3.size() <= 255);
    CHECK(u3::extract_options(cfg3, rest, o, why) && o.look == 3 && o.groups && o.reinterpret());
    CHECK(o.bevel == 0.4f && o.text_mm == 3.0f && o.sel_raise_mm == 8.0f && o.back_alpha == 0.35f);
    CHECK(u3::describe(o) == base_line + ";u3gp=3;u3gk=1.00;u3gn=5.0;u3bv=0.40;u3tx=3.0;u3sr=8.0;u3pb=0.35");
    std::string rest_user; u3::Options o0; CHECK(u3::extract_options(kUserCfg, rest_user, o0, why) && rest == rest_user);   // ours removed
    CHECK(u3::extract_options("auto;u3gp:1", rest, o, why) && o.groups && o.bevel == 0.0f && o.text_mm == 0.0f && o.sel_raise_mm == 0.0f && o.back_alpha == 0.0f);
    CHECK(u3::extract_options("auto;u3gp:2", rest, o, why) && o.back_alpha == 0.0f && o.sel_raise_mm == 8.0f && rest == "auto");
    // explicit knobs override the level in either order
    CHECK(u3::extract_options("auto;u3bv:0.2;u3gp:2;u3sr:0", rest, o, why) && o.bevel == 0.2f && o.sel_raise_mm == 0.0f && o.text_mm == 3.0f);
    CHECK(u3::extract_options("auto;u3gp:3;u3pb:0;u3gk:0.5;u3gn:2", rest, o, why) && o.back_alpha == 0.0f && o.curve_k == 0.5f && o.nest_mm == 2.0f);
    // stage-3 knobs without groups
    CHECK(u3::extract_options("auto;u3tx:2", rest, o, why) && !o.groups && o.reinterpret());
    const char* bad[] = {"auto;u3gp:4", "auto;u3gp:x", "auto;u3gk:1.5", "auto;u3gn:30", "auto;u3bv:1", "auto;u3tx:-1", "auto;u3sr:60", "auto;u3pb:2"};
    for (const char* b : bad) CHECK(!u3::extract_options(b, rest, o, why) && !why.empty());
}

// ================================================================================================== Surf: identical arithmetic
void test_surf_identity() {
    u3::Options o; o.on = true;
    const u3::Screen s = menu_screen(o);
    for (float tilt : {0.0f, 12.0f, -20.0f}) {
        o.tilt_deg = tilt;
        const u3::EyeView eye = anchor_eye(s, 0.21, -0.07);
        for (const RealDraw& r : kK10) {
            uint8_t v[4 * 64], a[4 * 64], b[4 * 64], c[4 * 64];
            vtx_of(r, v);
            CHECK(!u3::rewrite_vertices(v, 4, kCb, kFull, s, o, 2, eye, a).why);
            CHECK(!u3::rewrite_surf(v, 4, kCb, kFull, s, u3::layer_surf(s, o, u3::layer_distance(o, 2), 1.0), eye, b).why);
            CHECK(!u3::rewrite_surf(v, 4, kCb, kFull, s, gp::plane_surf(s, o, u3::layer_distance(o, 2), 0.0), eye, c).why);
            CHECK(std::memcmp(a, b, sizeof a) == 0 && std::memcmp(a, c, sizeof a) == 0);   // yaw 0 = the layer plane, byte for byte
        }
    }
}

// ================================================================================================== panel groups (pure)
gp::PanelGroups groups_of(const RealDraw* d, size_t n) {
    gp::PanelGroups g;
    for (size_t i = 0; i < n; ++i) {
        if (!is_frame_quad(d[i])) continue;
        g.add(box_of(d[i]));
    }
    g.finish();
    return g;
}
int group_with(const gp::PanelGroups& g, const RealDraw& r) {
    const u3::Box b = box_of(r);
    for (unsigned i = 0; i < g.size(); ++i) if (gp::same(g.at(i).b, b)) return int(i);
    return -1;
}
void test_groups_k10() {
    const gp::PanelGroups g = groups_of(kK10, count(kK10));
    CHECK(g.size() == 8);   // 157..164: the full-screen tint draws are not frame quads (large)
    const int i157 = group_with(g, kK10[3]), i158 = group_with(g, kK10[4]), i159 = group_with(g, kK10[5]), i161 = group_with(g, kK10[7]);
    CHECK(i157 >= 0 && i158 >= 0 && i159 >= 0 && i161 >= 0);
    CHECK(g.at(unsigned(i157)).level == 1 && g.at(unsigned(i157)).top == i158);   // the 0.117 x 0.179 box nests in [STATISTICS]
    CHECK(g.at(unsigned(i158)).level == 0 && g.at(unsigned(i158)).top == i158);
    CHECK(g.at(unsigned(i159)).level == 0 && g.at(unsigned(i161)).level == 0);
    // every glyph of the overlay lies in [SP WEAPON NAME] (its centre)
    for (const RealDraw& r : kK10) if (r.v[0][2] == 1.0f) {
        const u3::Box b = box_of(r);
        CHECK(g.find(0.5 * (b.x0 + b.x1), 0.5 * (b.y0 + b.y1)) == i159);
    }
    // the smallest containing rectangle wins
    const u3::Box b157 = box_of(kK10[3]);
    CHECK(g.find(0.5 * (b157.x0 + b157.x1), 0.5 * (b157.y0 + b157.y1)) == i157);
    CHECK(g.find(0.01, 0.99) == -1);
    // duplicates merge; degenerate is ignored; the cap is counted
    gp::PanelGroups h; h.add(b157); h.add(b157); u3::Box z = b157; z.x1 = z.x0; h.add(z); CHECK(h.size() == 1);
    for (unsigned i = 0; i < gp::kMaxGroups + 3; ++i) { u3::Box b; b.x0 = 0.01 * i; b.x1 = b.x0 + 0.005; b.y0 = 0.1; b.y1 = 0.2; h.add(b); }
    CHECK(h.size() == gp::kMaxGroups && h.dropped() == 4);
}
void test_groups_menus() {
    // the main menu head: tiles 26 / 27 and the selection box 28 are frame quads; the strips 6 / 7 (0.023 x 0.36) too; the backdrop is not
    const gp::PanelGroups g = groups_of(kD1, count(kD1));
    CHECK(g.size() == 5);
    for (unsigned i = 0; i < g.size(); ++i) CHECK(g.at(i).level == 0);
    // AGU1 s=45: the box drawn after its text is a group, and its glyphs lie in it
    const gp::PanelGroups h = groups_of(kAgu45, count(kAgu45));
    CHECK(h.size() == 1);
    for (const RealDraw& r : kAgu45) if (r.v[0][2] == 1.0f) { const u3::Box b = box_of(r); CHECK(h.find(0.5 * (b.x0 + b.x1), 0.5 * (b.y0 + b.y1)) == 0); }
}

// ================================================================================================== the pane, curvature, nesting
void test_pane_geometry() {
    u3::Options o; o.on = true; o.groups = true;
    const u3::Options lo = menu_lopts(o);
    const u3::Screen s = menu_screen(o);
    const gp::PanelGroups g = groups_of(kK10, count(kK10));
    const double D = gp::text_distance(lo);
    // the PLAYER box (right side) sits on the cylinder tangent at its own yaw: its centre at horizontal distance D from the anchor
    const u3::Box b161 = box_of(kK10[7]);
    const gp::Pane p161 = gp::pane_for(s, lo, &g, b161);
    CHECK(p161.group >= 0 && p161.level == 0 && !p161.s.cyl);
    const double yaw = gp::centre_yaw(s, b161);
    CHECK(yaw > 0.05);   // right of centre
    double P[3];
    CHECK(!u3::surf_point(s, p161.s, (b161.x0 + b161.x1) - 1.0, 1.0 - (b161.y0 + b161.y1), P));
    const double a = (P[0] - s.anchor[0]) * s.right[0] + (P[2] - s.anchor[2]) * s.right[2], f = (P[0] - s.anchor[0]) * s.fwd[0] + (P[2] - s.anchor[2]) * s.fwd[2];
    CHECK(std::fabs(std::sqrt(a * a + f * f) - D) < 1e-9);
    // its plane is flat (all corners at distance D along its normal) and faces the anchor's yaw direction
    CHECK(std::fabs(p161.s.n[0] - (std::cos(yaw) * s.fwd[0] + std::sin(yaw) * s.right[0])) < 1e-12);
    // u3gk 0.5: half the yaw; u3gk 0: the base plane
    u3::Options h = lo; h.curve_k = 0.5f;
    const gp::Pane ph = gp::pane_for(s, h, &g, b161);
    CHECK(std::fabs(ph.s.n[0] - (std::cos(0.5 * yaw) * s.fwd[0] + std::sin(0.5 * yaw) * s.right[0])) < 1e-12);
    // nesting: the box inside [STATISTICS] is u3gn in front of it, with its parent's orientation
    const gp::Pane p157 = gp::pane_for(s, lo, &g, box_of(kK10[3])), p158 = gp::pane_for(s, lo, &g, box_of(kK10[4]));
    CHECK(p157.level == 1 && std::fabs(p157.s.D - (p158.s.D - 0.005)) < 1e-12);
    for (int i = 0; i < 3; ++i) CHECK(p157.s.n[i] == p158.s.n[i]);
    // a draw outside every rectangle: the base surface; without groups: the base surface
    u3::Box out; out.x0 = 0.02; out.x1 = 0.03; out.y0 = 0.9; out.y1 = 0.95;
    CHECK(gp::pane_for(s, lo, &g, out).group == -1 && gp::pane_for(s, lo, nullptr, b161).group == -1);
    u3::Options off = lo; off.groups = false;
    CHECK(gp::pane_for(s, off, &g, b161).group == -1);
}

// ================================================================================================== anchor invariance + coplanarity on real geometry
// Every draw of the three real frames, with the frame's own rectangles as the reference (the steady state), under the full look (u3gp:3,
// u3lt 6): from the ANCHOR every copy (front, walls, back) shows exactly the flat image (the u3pp:1 text plane); from a moved head a glyph and
// its panel's corner at the same screen point stay registered (same plane: < 0.05 GS units; u3tx 3 mm: < 3 GS units = 1 % of the half image).
template <size_t N> void anchor_frame(const RealDraw (&fr)[N], unsigned& checked) {
    u3::Options o; o.on = true; o.groups = true; o.look = 3; o.frame_mm = 6.0f; o.bevel = 0.4f; o.text_mm = 3.0f; o.sel_raise_mm = 8.0f; o.back_alpha = 0.35f;
    const u3::Options lo = menu_lopts(o);
    const u3::Screen s = menu_screen(o);
    const gp::PanelGroups g = groups_of(fr, N);
    const u3::EyeView eye = anchor_eye(s);
    for (size_t i = 0; i < N; ++i) {
        uint8_t v[4 * 64]; vtx_of(fr[i], v);
        const u3::Box b = box_of(fr[i]);
        const gp::Pane pane = gp::pane_for(s, lo, &g, b);
        const bool frame = is_frame_quad(fr[i]);
        const gp::Shape shapes[4] = {gp::shape_flat(pane), frame ? gp::shape_rim(lo, pane) : gp::shape_flat(pane), gp::shape_selection(lo, pane, 4),
                                     gp::shape_text(lo, pane, true)};
        uint8_t flat[4 * 64];
        CHECK(!u3::rewrite_vertices(v, 4, kCb, kFull, s, lo, 2, eye, flat).why);
        for (const gp::Shape& sh : shapes) {
            uint8_t out[u3::kSlabCopies * 4 * 64];
            const u3::RewriteResult rw = gp::rewrite_shape(v, 4, kCb, kFull, s, sh, eye, out);
            CHECK(!rw.why && !rw.culled);
            gp::shade(out, v, kCb, kFull, sh, lo.bevel, lo.back_alpha);   // colours only: positions unchanged
            check_anchor(out, sh.copies, flat, 1e-3);
            ++checked;
        }
    }
}
void test_anchor_invariance_real() {
    unsigned checked = 0;
    anchor_frame(kD1, checked); anchor_frame(kAguMain, checked); anchor_frame(kAgu45, checked); anchor_frame(kK10, checked);
    CHECK(checked == 4 * (count(kD1) + count(kAguMain) + count(kAgu45) + count(kK10)));
}
void test_coplanar_from_moved_head() {
    u3::Options o; o.on = true; o.groups = true; o.frame_mm = 6.0f;
    const u3::Options lo = menu_lopts(o);
    const u3::Screen s = menu_screen(o);
    const gp::PanelGroups g = groups_of(kK10, count(kK10));
    const u3::EyeView moved = anchor_eye(s, 0.3, 0.12);
    // [SP WEAPON NAME] (159): its top-left corner (1806, 1858) and a glyph-sized quad with a vertex at the same GS point
    const RealDraw& panel = kK10[5];
    const RealDraw glyph = {0, {{1806, 1858, 1}, {1818, 1858, 1}, {1806, 1890, 1}, {1818, 1890, 1}}};   // centre (1812, 1874): below the tile 162
    uint8_t pv[4 * 64], gv[4 * 64]; vtx_of(panel, pv); vtx_of(glyph, gv);
    const gp::Pane pp = gp::pane_for(s, lo, &g, box_of(panel)), pg = gp::pane_for(s, lo, &g, box_of(glyph));
    CHECK(pp.group == pg.group && pp.group >= 0);
    uint8_t rim[u3::kRimCopies * 4 * 64], txt[u3::kSlabCopies * 4 * 64];
    CHECK(!gp::rewrite_shape(pv, 4, kCb, kFull, s, gp::shape_rim(lo, pp), moved, rim).why);
    CHECK(!gp::rewrite_shape(gv, 4, kCb, kFull, s, gp::shape_text(lo, pg, false), moved, txt).why);
    const uint8_t* front = rim + 4 * 4 * 64;
    CHECK(std::fabs(fl(front, 0, 0) - fl(txt, 0, 0)) < 0.05 && std::fabs(fl(front, 0, 1) - fl(txt, 0, 1)) < 0.05);
    // with u3tx 3 mm the text stands in front: a small, bounded parallax
    u3::Options t = lo; t.text_mm = 3.0f;
    CHECK(!gp::rewrite_shape(gv, 4, kCb, kFull, s, gp::shape_text(t, pg, false), moved, txt).why);
    const double d = std::hypot(fl(front, 0, 0) - fl(txt, 0, 0), fl(front, 0, 1) - fl(txt, 0, 1));
    CHECK(d > 0.05 && d < 3.0);
    // the walls of the rim have depth (from the moved head the back edge projects elsewhere than the front edge)
    CHECK(std::fabs(fl(rim, 0, 0) - fl(rim, 1, 0)) + std::fabs(fl(rim, 0, 1) - fl(rim, 1, 1)) > 0.05);
}

// ================================================================================================== shapes + shading
void test_shapes() {
    u3::Options o; o.on = true; o.frame_mm = 6.0f; o.sel_gap_mm = 4.0f; o.sel_depth_mm = 12.0f;
    const u3::Screen s = menu_screen(o);
    const u3::Options lo = menu_lopts(o);
    const gp::Pane p = gp::pane_for(s, lo, nullptr, box_of(kK10[5]));
    const double D = p.s.D;
    gp::Shape r = gp::shape_rim(lo, p);
    CHECK(r.role == gp::Role::rim && r.copies == u3::kRimCopies && r.front.D == D && std::fabs(r.back.D - (D + 0.006)) < 1e-12);
    u3::Options b = lo; b.back_alpha = 0.35f;
    r = gp::shape_rim(b, p); CHECK(r.copies == u3::kSlabCopies);
    CHECK(gp::downgrade(r) && r.copies == u3::kRimCopies && gp::downgrade(r) && r.copies == 1 && !gp::downgrade(r));
    u3::Options z = lo; z.frame_mm = 0.0f; CHECK(gp::shape_rim(z, p).copies == 1);
    // the selection: the u3sel rule (behind the pane), or raised IN FRONT of it (u3sr)
    gp::Shape sel = gp::shape_selection(lo, p, 4);
    CHECK(sel.copies == u3::kSlabCopies && std::fabs(sel.front.D - (D + 0.004)) < 1e-12 && std::fabs(sel.back.D - (D + 0.016)) < 1e-12);
    // identical to the stage-1 selection surfaces on the base pane
    CHECK(sel.front.D == u3::selection_front(lo) && sel.back.D == u3::selection_back(lo));
    u3::Options rs = lo; rs.sel_raise_mm = 8.0f;
    sel = gp::shape_selection(rs, p, 4);
    CHECK(std::fabs(sel.front.D - (D - 0.008)) < 1e-12 && sel.back.D == D);
    u3::Options s1 = rs; s1.select = 1; CHECK(gp::shape_selection(s1, p, 4).copies == 1);
    CHECK(gp::shape_selection(rs, p, 3).copies == 1);
    // text: u3tx in front; on the raised selection: on its front face, u3tx in front of it
    u3::Options tx = rs; tx.text_mm = 3.0f;
    CHECK(std::fabs(gp::shape_text(tx, p, false).front.D - (D - 0.003)) < 1e-12);
    CHECK(std::fabs(gp::shape_text(tx, p, true).front.D - (D - 0.011)) < 1e-12);
    CHECK(gp::shape_text(lo, p, true).front.D == D);   // no raise configured: on the pane
}
void test_shade() {
    u3::Options o; o.on = true; o.frame_mm = 6.0f;
    const u3::Screen s = menu_screen(o);
    const u3::Options lo = menu_lopts(o);
    uint8_t v[4 * 64]; vtx_of(kK10[5], v);   // strip order (x0,y0) (x0,y1) (x1,y0) (x1,y1): GS y grows DOWN the image here (sy < 0)
    const gp::Pane p = gp::pane_for(s, lo, nullptr, box_of(kK10[5]));
    // which edges face down / right: kSlabEdges {0,1} left, {1,3} bottom (y1 = 1986 is lower on the image), {3,2} right, {2,0} top
    CHECK(gp::shaded_edges(v, kCb, kFull) == ((1u << 1) | (1u << 2)));
    u3::Options b = lo; b.back_alpha = 0.35f;
    const gp::Shape sh = gp::shape_rim(b, p);
    uint8_t out[u3::kSlabCopies * 4 * 64];
    CHECK(!gp::rewrite_shape(v, 4, kCb, kFull, s, sh, anchor_eye(s), out).why);
    uint8_t ref[sizeof out]; std::memcpy(ref, out, sizeof out);
    const gp::ShadeCount n = gp::shade(out, v, kCb, kFull, sh, 0.4f, 0.35f);
    CHECK(n.walls == 2 && n.back == 1 && n.skipped == 0);
    for (unsigned c = 0; c < u3::kSlabCopies; ++c)
        for (unsigned i = 0; i < 4; ++i) {
            const uint8_t* q = out + size_t(c) * 4 * 64; const uint8_t* r = ref + size_t(c) * 4 * 64;
            const bool lit_wall = c == 2 || c == 3;   // walls 1, 2 (edges {1,3}, {3,2}) at copies 2, 3
            for (unsigned k = 0; k < 3; ++k) CHECK(std::fabs(fl(q, i, 4 + k) - fl(r, i, 4 + k) * (lit_wall ? 0.6f : 1.0f)) < 1e-6f);
            CHECK(std::fabs(fl(q, i, 7) - fl(r, i, 7) * (c == 0 ? 0.35f : 1.0f)) < 1e-6f);   // only the back face's alpha
            CHECK(std::memcmp(q + 64 * i, r + 64 * i, 16) == 0 && std::memcmp(q + 64 * i + 32, r + 64 * i + 32, 32) == 0);   // position, uv, COLOR1
        }
    // a selection slab's back face keeps its colour; a single copy is never shaded; a non-finite colour is skipped
    const gp::Shape sel = gp::shape_selection(lo, p, 4);
    CHECK(!gp::rewrite_shape(v, 4, kCb, kFull, s, sel, anchor_eye(s), out).why);
    CHECK(gp::shade(out, v, kCb, kFull, sel, 0.4f, 0.35f).back == 0);
    CHECK(gp::shade(out, v, kCb, kFull, gp::shape_flat(p), 0.4f, 0.35f).walls == 0);
    const float nan = std::nanf(""); std::memcpy(out + 4 * 64 * 2 + 16, &nan, 4);
    CHECK(gp::shade(out, v, kCb, kFull, sel, 0.4f, 0.0f).skipped == 1);
}

// ================================================================================================== the producer
// Runs `frames` UI3D menu frames of `draws` and returns the plans of the last one.
std::vector<UiPlan> run_menu(Producer& p, Driver& d, const RealDraw* draws, size_t n, int frames, int64_t& t) {
    std::vector<UiPlan> out;
    for (int fr = 0; fr < frames; ++fr) {
        out.clear();
        t += 16;
        p.on_present(t);
        for (size_t i = 0; i < n; ++i) out.push_back(d.ui(real_draw(&d.L, t + 1, draws[i])));
        d.end();
    }
    return out;
}
bool same_plan(const UiPlan& a, const UiPlan& b) {
    if (a.record != b.record || a.copies != b.copies || a.vertices != b.vertices || a.layer != b.layer) return false;
    if (!streq(a.why ? a.why : "-", b.why ? b.why : "-")) return false;
    for (unsigned e = 0; e < 2; ++e) {
        if (a.eye_draw[e] != b.eye_draw[e]) return false;
        if (!a.record || !a.eye_draw[e]) continue;
        if (std::memcmp(a.scissor[e], b.scissor[e], sizeof a.scissor[e]) != 0) return false;
        if (std::memcmp(a.vtx[e], b.vtx[e], size_t(a.copies) * a.vertices * 64) != 0) return false;
    }
    return true;
}
// R4 + the user's cfg: with every stage-2/3 option off the plans are the stage-1 plans (u3pp:1;u3lt:6); with u3gp:1, u3gk 0, u3gn 0 every
// group plane IS the base plane, so the plans are byte-identical too (the surface path computes the same vertices and scissors); the first
// frame of u3gp:1 (no reference rectangles yet) is the base surface: identical to stage 1 as well.
void test_producer_matches_stage1() {
    u3::Options base; base.on = true; base.panel_plane = true; base.frame_mm = 6.0f;
    u3::Options flatgroups = base; flatgroups.groups = true; flatgroups.look = 1; flatgroups.curve_k = 0.0f; flatgroups.nest_mm = 0.0f;
    u3::Options curved = base; curved.groups = true; curved.look = 1;
    for (int which = 0; which < 3; ++which) {
        const RealDraw* fr = which == 0 ? kD1 : which == 1 ? kAguMain : kAgu45;
        const size_t n = which == 0 ? count(kD1) : which == 1 ? count(kAguMain) : count(kAgu45);
        Producer a, b, c; configure(a, base); configure(b, flatgroups); configure(c, curved);
        Driver da(a), db(b), dc(c);
        int64_t ta = 1000, tb = 1000, tc = 1000;
        const std::vector<UiPlan> pa = run_menu(a, da, fr, n, 3, ta), pb = run_menu(b, db, fr, n, 3, tb);
        const std::vector<UiPlan> pc1 = run_menu(c, dc, fr, n, 1, tc);
        Producer a1; configure(a1, base); Driver da1(a1); int64_t t1 = 1000;
        const std::vector<UiPlan> pa1 = run_menu(a1, da1, fr, n, 1, t1);
        for (size_t i = 0; i < n; ++i) { CHECK(same_plan(pa[i], pb[i])); CHECK(same_plan(pa1[i], pc1[i])); }
        const std::string sb = b.ui3d_summary();
        CHECK(has(sb, ";grp=") && field(sb, ";rejected=") == 0);
    }
}
// The main menu head (runD1) under the full look: frame 1 sees no rectangles (all on the base surface), from frame 2 the tiles and strips are
// groups, the selection box is recognised (frame 1's rows) and raised IN FRONT of its pane with the two text rows inside it riding on its front
// face; the third row stays u3tx in front of its pane; the backdrop keeps layer 0.
void test_producer_main_menu() {
    u3::Options o; o.on = true; o.panel_plane = true; o.frame_mm = 6.0f; o.groups = true; o.look = 3;
    o.bevel = 0.4f; o.text_mm = 3.0f; o.sel_raise_mm = 8.0f; o.back_alpha = 0.35f;
    Producer p; configure(p, o); Driver d(p);
    int64_t t = 1000;
    const u3::Options lo = menu_lopts(o);
    const u3::Screen s = menu_screen(o);
    const double D = gp::text_distance(lo);
    std::vector<UiPlan> f1 = run_menu(p, d, kD1, count(kD1), 1, t);
    std::string s1 = p.ui3d_summary();
    CHECK(field(s1, ";grp=") == 0 && field(s1, ";rejected=") == 0);
    std::vector<UiPlan> f2 = run_menu(p, d, kD1, count(kD1), 1, t);
    const std::string s2 = p.ui3d_summary();
    CHECK(field(s2, ";rejected=") == 0 && field(s2, ";grp-n=") == 5);   // frame 1's rectangles (incl. the not-yet-recognised selection box)
    const PoseIn ps = pose();
    const u3::EyeView e0 = ui_eye(ps.eyes[0]);
    for (size_t i = 0; i < count(kD1); ++i) {
        const UiPlan& pl = f2[i];
        const int all = kD1[i].all;
        const u3::Box b = box_of(kD1[i]);
        if (all <= 5) { CHECK(pl.record && pl.layer == 0 && pl.copies == 1); continue; }   // backdrop: stage 1, layer 0
        if (b.w() <= 0 || b.h() <= 0) { CHECK(pl.record && pl.copies == 1); continue; }   // the zero-height quads stay flat
        CHECK(pl.record && pl.eye_draw[0] && pl.eye_draw[1]);
        uint8_t v[4 * 64]; vtx_of(kD1[i], v);
        if (all == 28) {   // the selection: raised slab, back face on the pane (its own frame-1 rectangle, not a group from frame 2 on)
            CHECK(pl.copies == u3::kSlabCopies);
            gp::PanelGroups g1 = groups_of(kD1, count(kD1));
            const gp::Pane pn = gp::pane_for(s, lo, &g1, b);
            uint8_t ref[u3::kSlabCopies * 4 * 64];
            const gp::Shape sh = gp::shape_selection(lo, pn, 4);
            CHECK(sh.front.D < pn.s.D && sh.back.D == pn.s.D);
            CHECK(!gp::rewrite_shape(v, 4, kCb, kFull, s, sh, e0, ref).why);
            gp::shade(ref, v, kCb, kFull, sh, lo.bevel, lo.back_alpha);
            CHECK(std::memcmp(pl.vtx[0], ref, sizeof ref) == 0);
        } else if (kD1[i].v[0][2] == 1.0f) {   // glyphs: rows 1-2 inside the selection box ride on its front face
            CHECK(pl.copies == 1);
            const bool inside = all <= 43;
            gp::PanelGroups g1 = groups_of(kD1, count(kD1));
            const gp::Pane pn = gp::pane_for(s, lo, &g1, b);
            uint8_t ref[u3::kSlabCopies * 4 * 64];
            CHECK(!gp::rewrite_shape(v, 4, kCb, kFull, s, gp::shape_text(lo, pn, inside), e0, ref).why);
            CHECK(std::memcmp(pl.vtx[0], ref, 4 * 64) == 0);
            if (inside) CHECK(std::fabs(gp::shape_text(lo, pn, true).front.D - (pn.s.D - 0.011)) < 1e-12);
        } else if (is_frame_quad(kD1[i])) {
            CHECK(pl.copies == u3::kSlabCopies);   // rim + back face (u3pb)
        }
    }
    CHECK(field(s2, ";sel-text=") == 15 && field(s2, ";pane-back=") >= 4 && field(s2, ";bevel=") >= 8 && field(s2, ";shade-skip=") == 0);
    (void)D;
}
// The K10 hangar overlay twice (u3a:1): the nested box is 5 mm in front of [STATISTICS], every glyph lies on [SP WEAPON NAME]'s plane (u3tx in
// front), the tint draws are dropped as before, nothing is rejected.
void test_producer_k10_hangar() {
    uint8_t rec[hz::kRecBytes]; real_record(rec);
    u3::Options o; o.on = true; o.hangar = 1; o.panel_plane = true; o.frame_mm = 6.0f; o.groups = true; o.look = 2;
    o.bevel = 0.4f; o.text_mm = 3.0f; o.sel_raise_mm = 8.0f;
    Producer p; configure(p, o); Driver d(p);
    const u3::Options lo = menu_lopts(o);
    const u3::Screen s = menu_screen(o);
    const gp::PanelGroups g = groups_of(kK10, count(kK10));
    const u3::EyeView e1 = ui_eye(pose().eyes[1]);
    std::vector<UiPlan> fr;
    for (int k = 0; k < 2; ++k) {
        fr.clear();
        const int64_t t = 1000 + 16 * k;
        p.on_present(t);
        CHECK(d.acm_draw(acm(&d.L, t + 1, rec)).record);
        for (const RealDraw& r : kK10) fr.push_back(d.ui(real_draw(&d.L, t + 1, r)));
        d.end();
    }
    unsigned grouped = 0;
    for (size_t i = 0; i < count(kK10); ++i) {
        const UiPlan& pl = fr[i];
        if (kK10[i].all <= 156) { CHECK(!pl.record && streq(pl.why, "u3a-tint-dropped")); continue; }
        CHECK(pl.record && pl.eye_draw[0] && pl.eye_draw[1]);
        uint8_t v[4 * 64]; vtx_of(kK10[i], v);
        const gp::Pane pn = gp::pane_for(s, lo, &g, box_of(kK10[i]));
        CHECK(pn.group >= 0);
        ++grouped;
        const gp::Shape sh = is_frame_quad(kK10[i]) ? gp::shape_rim(lo, pn) : gp::shape_text(lo, pn, false);
        uint8_t ref[u3::kSlabCopies * 4 * 64];
        CHECK(!gp::rewrite_shape(v, 4, kCb, kFull, s, sh, e1, ref).why);
        gp::shade(ref, v, kCb, kFull, sh, lo.bevel, lo.back_alpha);
        CHECK(pl.copies == sh.copies && std::memcmp(pl.vtx[1], ref, size_t(sh.copies) * 4 * 64) == 0);
        if (kK10[i].all == 157) CHECK(pn.level == 1);
    }
    CHECK(grouped == count(kK10) - 3);
    const std::string sm = p.ui3d_summary();
    CHECK(has(sm, ";u3a-rejected=0;") && field(sm, ";grp-n=") == 8 && field(sm, ";rim-flat=") == 0);
}
// The AGU1 s=45 menu: the box drawn AFTER its text. The census logged only the frame's TAIL (mode=tail), so the frame is preceded by the main
// menu's head (runD1 ord 1..7: the 4:3 backdrop and the two panel strips) so that the classifier has seen its first panel draw, as in the
// whole 199-draw frame. From frame 2 the tile and the glyphs join the box's group plane (frame 1's rectangle: the box is a top-level panel
// centred on the screen); the z-0 full-screen dim is never put on a group plane (base surface = the stage-1 u3pp vertices).
void test_producer_box_after_text() {
    std::vector<RealDraw> fr(kD1, kD1 + 7);
    fr.insert(fr.end(), kAgu45, kAgu45 + count(kAgu45));
    u3::Options o; o.on = true; o.panel_plane = true; o.groups = true; o.look = 1; o.curve_k = 1.0f;
    Producer p; configure(p, o); Driver d(p);
    int64_t t = 1000;
    run_menu(p, d, fr.data(), fr.size(), 1, t);
    const std::vector<UiPlan> f2 = run_menu(p, d, fr.data(), fr.size(), 1, t);
    const std::string sm = p.ui3d_summary();
    CHECK(field(sm, ";grp-n=") == 3 && field(sm, ";rejected=") == 0);   // the two strips and the box
    const u3::Options lo = menu_lopts(o);
    const u3::Screen s = menu_screen(o);
    const u3::EyeView e0 = ui_eye(pose().eyes[0]);
    gp::PanelGroups g; for (const RealDraw& r : fr) if (is_frame_quad(r)) g.add(box_of(r)); g.finish();
    const int box = group_with(g, kAgu45[7]);
    CHECK(box >= 0);
    for (size_t i = 7; i < fr.size(); ++i) {
        const UiPlan& pl = f2[i];
        CHECK(pl.record && pl.eye_draw[0]);
        uint8_t v[4 * 64], ref[u3::kSlabCopies * 4 * 64]; vtx_of(fr[i], v);
        if (fr[i].all == 199) {   // the dim
            CHECK(!u3::rewrite_vertices(v, 4, kCb, kFull, s, lo, 2, e0, ref).why);
            CHECK(std::memcmp(pl.vtx[0], ref, 4 * 64) == 0);
            continue;
        }
        const gp::Pane pn = gp::pane_for(s, lo, &g, box_of(fr[i]));
        CHECK(pn.group == box);   // tile 164 (centre 1826,1888), the glyphs and the box itself: one plane
        CHECK(!gp::rewrite_shape(v, 4, kCb, kFull, s, gp::shape_flat(pn), e0, ref).why);
        CHECK(pl.copies == 1 && std::memcmp(pl.vtx[0], ref, 4 * 64) == 0);
    }
    // frame 2: strips 6 / 7 (their own groups) + the 8 draws above = 10 on group planes; base: frame 1's 11 non-backdrop draws + frame 2's dim
    CHECK(field(sm, ";grp=") == 10 && field(sm, ";grp-base=") == 12);
}
// The upload fallback: 180 frame quads with u3pb (6 copies each, 3 KiB per draw for both eyes): once the slot's span minus the 64 KiB reserve
// is used, the shapes step down to flat quads (counted as rim-flat); the frame is never refused (the real menus use < 250 KiB: 347 draws).
void test_upload_fallback() {
    u3::Options o; o.on = true; o.panel_plane = true; o.frame_mm = 6.0f; o.groups = true; o.look = 3; o.back_alpha = 0.35f;
    Producer p; configure(p, o); Driver d(p);
    p.on_present(1000);
    unsigned six = 0, five = 0, one = 0;
    for (int i = 0; i < 180; ++i) {
        const float x0 = 1800.0f + float(i % 20) * 20.0f, y0 = 1850.0f + float(i / 20) * 15.0f;
        const UiPlan pl = d.ui(ps2(&d.L, 1001, x0, y0, x0 + 40.0f, y0 + 40.0f, 64.0f));
        CHECK(pl.record);
        if (pl.copies == 6) ++six; else if (pl.copies == 5) ++five; else if (pl.copies == 1) ++one;
    }
    d.end();
    const std::string sm = p.ui3d_summary();
    const unsigned fit = unsigned((u3::kUploadPerSlot - gp::kShapeReserve) / (2u * 6u * 4u * 64u));
    CHECK(six == fit && five == 0 && one == 180 - fit && field(sm, ";rejected=") == 0 && field(sm, ";rim-flat=") == one && field(sm, ";rim=") == six);
}

} // namespace

int main() {
    test_options();
    test_surf_identity();
    test_groups_k10();
    test_groups_menus();
    test_pane_geometry();
    test_anchor_invariance_real();
    test_coplanar_from_moved_head();
    test_shapes();
    test_shade();
    test_producer_matches_stage1();
    test_producer_main_menu();
    test_producer_k10_hangar();
    test_producer_box_after_text();
    test_upload_fallback();
    std::printf("ui3d_groups_offline: %u checks passed\n", g_checks);
    return 0;
}

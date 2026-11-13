/**
 * @file test_mesh_snapshots.cpp
 * @brief Pinned outputs of the 2D mesh readers (mesh open plan: the E3 equivalence harness,
 *        kept as snapshot tests).
 *
 * @details Every input under data/mesh_snapshots/ is read and everything the reader fills is
 *          dumped -- every data member of the mesh, the 2D options, the pending boundary,
 *          edge-conveyance and quality rows and the infiltration rows, plus the return value or
 *          the open diagnostics -- and compared with `<input stem>.expected.txt` next to it.
 *          Doubles are written as their bit patterns; arrays longer than 64 elements as their
 *          length and an FNV-1a hash of their elements.
 *
 *          - `mesh_file/` (*.2dm): through load2DMeshExternalFile, as SWMMEngine::open loads a
 *            [2D_MESH_FILE].
 *          - `inp/` (*.inp): whole models opened leniently, as the GUI's editor opens them.
 *
 *          A mismatch writes the actual dump under tests/output/mesh_snapshots/. After an
 *          intended change, run with OPENSWMM_UPDATE_MESH_SNAPSHOTS=1 and review the diff of the
 *          expected files. The member lists are generated (tests/scripts/
 *          gen_mesh_snapshot_fields.py), so a new member shows up as a snapshot difference.
 */
#include "2d/infil/Infil2D.hpp"
#include "2d/input/SectionHandlers2D.hpp"
#include "core/SWMMEngine.hpp"

#include <openswmm/engine/openswmm_engine.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;

namespace {

#define MESH_SNAPSHOT_DECLARATIONS
#include "mesh_snapshot_fields.inc"
#undef MESH_SNAPSHOT_DECLARATIONS

template <class T> std::enable_if_t<std::is_floating_point_v<T>> dump(std::ostream& o, T v) {
    if constexpr (sizeof(T) == 8) {
        std::uint64_t b;
        std::memcpy(&b, &v, 8);
        o << "d" << std::hex << b << std::dec;
    } else {
        std::uint32_t b;
        std::memcpy(&b, &v, 4);
        o << "f" << std::hex << b << std::dec;
    }
}
template <class T> std::enable_if_t<std::is_integral_v<T>> dump(std::ostream& o, T v) {
    o << static_cast<long long>(v);
}
template <class T> std::enable_if_t<std::is_enum_v<T>> dump(std::ostream& o, T v) {
    o << "e" << static_cast<long long>(v);
}
void dump(std::ostream& o, const std::string& s) {
    o << '"';
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\')
            o << '\\' << c;
        else if (c < 0x20 || c >= 0x7f)
            o << "\\x" << std::hex << int(c) << std::dec;
        else
            o << c;
    }
    o << '"';
}
template <class T> void dump(std::ostream& o, const std::vector<T>& v) {
    o << "[" << v.size() << ":";
    if (v.size() <= 64) {
        for (const auto& e : v) {
            dump(o, static_cast<const T&>(e));
            o << ",";
        }
    } else {
        std::uint64_t h = 1469598103934665603ull; // FNV-1a over each element's text
        std::ostringstream one;
        for (const auto& e : v) {
            one.str("");
            dump(one, static_cast<const T&>(e));
            one << ",";
            for (const unsigned char c : one.str())
                h = (h ^ c) * 1099511628211ull;
        }
        o << "fnv" << std::hex << h << std::dec;
    }
    o << "]";
}
template <class T, std::size_t N> void dump(std::ostream& o, const T (&a)[N]) {
    o << "(";
    for (const auto& e : a) {
        dump(o, e);
        o << ",";
    }
    o << ")";
}
void dump(std::ostream& o, const openswmm::twoD::InternedStrings& v) {
    dump(o, v.dense()); // one string per element, as the plain array it replaced
}
void dump(std::ostream& o, const std::vector<bool>& v) {
    o << "[" << v.size() << ":";
    for (const bool b : v)
        o << (b ? '1' : '0');
    o << "]";
}

#include "mesh_snapshot_fields.inc"

/// load2DMeshExternalFile on `path` (relative to the data directory), every output dumped.
std::string dumpMeshFile(const std::string& path) {
    using namespace openswmm::twoD;
    MeshData mesh;
    SolverOptions2D opts;
    std::vector<PendingBoundaryRow> bc;
    std::vector<PendingEdgeConveyanceRow> ec;
    Infil2D infil;
    std::vector<std::string> warnings;
    std::vector<PendingInitialQualityRow> iq;
    std::vector<PendingInitialBedQualityRow> ibq;
    std::vector<PendingBoundaryQualityRow> bq;
    const std::string ret =
        load2DMeshExternalFile(mesh, opts, bc, ec, &infil, path, "", &warnings, &iq, &ibq, &bq);
    std::ostringstream o;
    o << "return=";
    dump(o, ret);
    o << "\nwarnings=";
    dump(o, warnings);
    o << "\nmesh=";
    dump(o, mesh);
    o << "\noptions=";
    dump(o, opts);
    o << "\nbc=";
    dump(o, bc);
    o << "\nec=";
    dump(o, ec);
    o << "\niq=";
    dump(o, iq);
    o << "\nibq=";
    dump(o, ibq);
    o << "\nbq=";
    dump(o, bq);
    o << "\ninfil.defaults=";
    dump(o, infil.defaults());
    o << "\ninfil.overrides=";
    dump(o, infil.overrides());
    o << "\ninfil.options=";
    dump(o, infil.options());
    o << "\n";
    return o.str();
}

/// A whole model opened leniently (the GUI editor's open): diagnostics and the 2D state.
std::string dumpInp(const std::string& path) {
    SWMM_Engine e = swmm_engine_create();
    swmm_engine_set_lenient_open(e, 1);
    const int rc = swmm_engine_open(e, path.c_str(), "", "", nullptr);
    auto* engine = reinterpret_cast<openswmm::SWMMEngine*>(e);
    const auto& ctx = engine->context();
    auto& router = engine->surfaceRouter2D(); // non-const: the pending-row accessors
    std::ostringstream o;
    o << "rc=" << rc << "\nerror_code=" << ctx.error_code << "\nerror_message=";
    dump(o, ctx.error_message);
    o << "\nerrors=";
    dump(o, ctx.errors);
    o << "\nwarnings=";
    dump(o, ctx.warnings);
    o << "\nmesh=";
    dump(o, router.mesh());
    o << "\noptions=";
    dump(o, router.options());
    o << "\nbc=";
    dump(o, router.pendingBCRows());
    o << "\nec=";
    dump(o, router.pendingEdgeConveyanceRows());
    o << "\niq=";
    dump(o, router.pendingInitialQualityRows());
    o << "\nibq=";
    dump(o, router.pendingInitialBedQualityRows());
    o << "\nbq=";
    dump(o, router.pendingBoundaryQualityRows());
    o << "\ninfil.defaults=";
    dump(o, router.infil().defaults());
    o << "\ninfil.overrides=";
    dump(o, router.infil().overrides());
    o << "\ninfil.options=";
    dump(o, router.infil().options());
    o << "\n";
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return o.str();
}

/// The inputs with extension `ext` in `dir`, sorted by name.
std::vector<fs::path> inputs(const fs::path& dir, const std::string& ext) {
    std::vector<fs::path> out;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().extension() == ext)
            out.push_back(entry.path());
    std::sort(out.begin(), out.end());
    return out;
}

/// Compare `actual` with the snapshot next to `input` (or write it, in update mode).
void check(const fs::path& input, std::string actual) {
    // Paths the engine made absolute start at the data directory (the working directory).
    const std::string root = fs::current_path().generic_string();
    for (auto at = actual.find(root); at != std::string::npos; at = actual.find(root, at))
        actual.replace(at, root.size(), "<data>");
    const fs::path expected_path = input.parent_path() / (input.stem().string() + ".expected.txt");
    if (std::getenv("OPENSWMM_UPDATE_MESH_SNAPSHOTS") != nullptr) {
        std::ofstream(expected_path, std::ios::binary) << actual;
        return;
    }
    std::ifstream in(expected_path, std::ios::binary);
    ASSERT_TRUE(in.good()) << "no snapshot for " << input.generic_string() << " ("
                           << expected_path.generic_string() << ")";
    std::ostringstream expected;
    expected << in.rdbuf();
    if (actual == expected.str())
        return;
    const fs::path out_dir = fs::path(OPENSWMM_MESH_SNAPSHOT_OUT_DIR) / input.parent_path();
    fs::create_directories(out_dir);
    const fs::path out = out_dir / (input.stem().string() + ".actual.txt");
    std::ofstream(out, std::ios::binary) << actual;
    ADD_FAILURE() << input.generic_string()
                  << " differs from its snapshot; actual dump: " << out.generic_string();
}

} // namespace

TEST(MeshSnapshots, MeshFileReader) {
    const auto files = inputs("mesh_snapshots/mesh_file", ".2dm");
    ASSERT_FALSE(files.empty());
    for (const auto& f : files) {
        SCOPED_TRACE(f.generic_string());
        check(f, dumpMeshFile(f.generic_string()));
    }
}

TEST(MeshSnapshots, InpReader) {
    const auto files = inputs("mesh_snapshots/inp", ".inp");
    ASSERT_FALSE(files.empty());
    for (const auto& f : files) {
        SCOPED_TRACE(f.generic_string());
        check(f, dumpInp(f.generic_string()));
    }
}

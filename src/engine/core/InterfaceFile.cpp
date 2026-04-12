/**
 * @file InterfaceFile.cpp
 * @brief Routing interface file — coupling between separate SWMM simulations.
 *
 * @details Text-based interface file format matching legacy iface.c:
 *
 *   Line 1: "SWMM5 Interface File"
 *   Line 2: Title
 *   Line 3: <step> - reporting time step in sec
 *   Line 4: <n> - number of constituents (flow + pollutants)
 *   Lines 5..5+n: constituent names and units (FLOW first)
 *   Next line: <m> - number of nodes
 *   Next m lines: node names
 *   Column headings line
 *   Data lines: NodeID YYYY MM DD HH MM SS FLOW [pollut1] [pollut2] ...
 *
 * @note Legacy reference: src/legacy/engine/iface.c
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "InterfaceFile.hpp"
#include "SimulationContext.hpp"
#include "DateTime.hpp"
#include "UnitConversion.hpp"

#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

// Portable strtok: MSVC deprecates strtok in favour of strtok_s
#ifdef _MSC_VER
#  define OPENSWMM_STRTOK(str, delim, ctx) strtok_s((str), (delim), (ctx))
#else
#  define OPENSWMM_STRTOK(str, delim, ctx) strtok((str), (delim))
#endif

namespace openswmm {
namespace iface {

static constexpr int MAXLINE = 1024;
static constexpr double NO_DATE = -1.0e10;

// ============================================================================
// InterfaceFileSoA
// ============================================================================

void InterfaceFileSoA::resize(int n) {
    count = n;
    auto un = static_cast<std::size_t>(std::max(n, 0));
    node_idx.assign(un, -1);
    // old_values and new_values are resized by the caller based on
    // n_nodes * n_values_per_node
}

// ============================================================================
// InterfaceManager::init
// ============================================================================

void InterfaceManager::init(SimulationContext& /*ctx*/) {
    data_.count = 0;
    infile_  = nullptr;
    outfile_ = nullptr;
    old_time_ = 0.0;
    new_time_ = 0.0;
    n_iface_polluts_ = 0;
    n_values_per_node_ = 1;
    iface_frac_ = 0.0;
    pollut_map_.clear();
}

// ============================================================================
// openFiles
// ============================================================================

int InterfaceManager::openFiles(const std::string& infile_path,
                                 const std::string& outfile_path) {
    // Validate that infile and outfile are not the same
    if (!infile_path.empty() && !outfile_path.empty()) {
        if (infile_path == outfile_path) {
            return -1;  // Error: same file for input and output
        }
    }

    // Open output file for writing
    if (!outfile_path.empty()) {
        outfile_ = std::fopen(outfile_path.c_str(), "wt");
        if (!outfile_) return -2;
    }

    // Open input file for reading
    if (!infile_path.empty()) {
        infile_ = std::fopen(infile_path.c_str(), "rt");
        if (!infile_) return -3;
    }

    return 0;
}

// ============================================================================
// readFileHeader — parse the header of an inflow interface file
// ============================================================================

int InterfaceManager::readFileHeader(SimulationContext& ctx) {
    if (!infile_) return -1;

    char line[MAXLINE + 1];
    char s1[MAXLINE + 1];
    char s2[MAXLINE + 1];

    // Line 1: check "SWMM5 Interface File"
    if (!std::fgets(line, MAXLINE, infile_)) return -1;
    if (std::sscanf(line, "%s", s1) != 1) return -1;
    if (std::strncmp(s1, "SWMM5", 5) != 0) return -1;

    // Line 2: skip title
    if (!std::fgets(line, MAXLINE, infile_)) return -1;

    // Line 3: reporting time step
    if (!std::fgets(line, MAXLINE, infile_)) return -1;
    if (std::sscanf(line, "%d", &iface_step_) != 1 || iface_step_ <= 0) return -1;

    // Line 4: number of constituents (including FLOW)
    int n_constit = 0;
    if (!std::fgets(line, MAXLINE, infile_)) return -1;
    if (std::sscanf(line, "%d", &n_constit) != 1) return -1;
    n_iface_polluts_ = n_constit - 1;  // subtract FLOW
    if (n_iface_polluts_ < 0) return -1;
    n_values_per_node_ = 1 + n_iface_polluts_;

    // Line 5: FLOW units
    if (!std::fgets(line, MAXLINE, infile_)) return -1;
    if (std::sscanf(line, "%s %s", s1, s2) < 2) return -1;
    if (std::strncmp(s1, "FLOW", 4) != 0) return -1;
    // Map flow unit string to FlowUnits enum index (matching legacy iface.c)
    static const char* flow_words[] = {"CFS","GPM","MGD","CMS","LPS","MLD"};
    iface_flow_units_ = 0;
    for (int fi = 0; fi < 6; ++fi) {
        if (std::strncmp(s2, flow_words[fi], 3) == 0) {
            iface_flow_units_ = fi;
            break;
        }
    }

    // Initialize pollutant mapping: project pollutant -> interface file column
    int n_polluts = ctx.n_pollutants();
    if (n_polluts > 0) {
        pollut_map_.assign(static_cast<std::size_t>(n_polluts), -1);
    }

    // Read pollutant names and match to project pollutants
    for (int i = 0; i < n_iface_polluts_; ++i) {
        if (!std::fgets(line, MAXLINE, infile_)) return -1;
        if (std::sscanf(line, "%s %s", s1, s2) < 2) return -1;

        // Look up this pollutant name in the project
        int j = ctx.pollutant_names.find(s1);
        if (j >= 0 && j < n_polluts) {
            pollut_map_[static_cast<std::size_t>(j)] = i;
        }
    }

    // Read number of interface nodes
    int n_nodes = 0;
    if (!std::fgets(line, MAXLINE, infile_)) return -1;
    if (std::sscanf(line, "%d", &n_nodes) != 1 || n_nodes <= 0) return -1;

    // Allocate SoA arrays
    data_.resize(n_nodes);
    auto un = static_cast<std::size_t>(n_nodes);
    auto total_values = un * static_cast<std::size_t>(n_values_per_node_);
    data_.old_values.assign(total_values, 0.0);
    data_.new_values.assign(total_values, 0.0);

    // Read node names and find their indices in the project
    for (int i = 0; i < n_nodes; ++i) {
        if (!std::fgets(line, MAXLINE, infile_)) return -1;
        char name[MAXLINE + 1];
        if (std::sscanf(line, "%s", name) != 1) return -1;
        data_.node_idx[static_cast<std::size_t>(i)] = ctx.node_names.find(name);
    }

    // Skip column headings line
    if (!std::fgets(line, MAXLINE, infile_)) return -1;

    // Read first set of interface values
    readNextPeriod();
    old_time_ = new_time_;

    return 0;
}

// ============================================================================
// readNextPeriod — read data for the next timestep from the interface file
// ============================================================================

bool InterfaceManager::readNextPeriod() {
    if (!infile_) return false;

    char line[MAXLINE + 1];
    new_time_ = NO_DATE;

    for (int i = 0; i < data_.count; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (std::feof(infile_)) return false;
        if (!std::fgets(line, MAXLINE, infile_)) return false;

        // Parse: NodeName YYYY MM DD HH MM SS Flow [Pollut1] [Pollut2] ...
        char* strtok_ctx = nullptr;
        (void)strtok_ctx;  // used only on MSVC
        char* tok = OPENSWMM_STRTOK(line, " \t\n\r", &strtok_ctx);
        if (!tok) return false;

        // Parse date/time components
        int yr = 0, mon = 0, day = 0, hr = 0, mn = 0, sec = 0;
        tok = OPENSWMM_STRTOK(nullptr, " \t\n\r", &strtok_ctx); if (!tok) return false; yr  = std::atoi(tok);
        tok = OPENSWMM_STRTOK(nullptr, " \t\n\r", &strtok_ctx); if (!tok) return false; mon = std::atoi(tok);
        tok = OPENSWMM_STRTOK(nullptr, " \t\n\r", &strtok_ctx); if (!tok) return false; day = std::atoi(tok);
        tok = OPENSWMM_STRTOK(nullptr, " \t\n\r", &strtok_ctx); if (!tok) return false; hr  = std::atoi(tok);
        tok = OPENSWMM_STRTOK(nullptr, " \t\n\r", &strtok_ctx); if (!tok) return false; mn  = std::atoi(tok);
        tok = OPENSWMM_STRTOK(nullptr, " \t\n\r", &strtok_ctx); if (!tok) return false; sec = std::atoi(tok);

        // Parse flow value
        tok = OPENSWMM_STRTOK(nullptr, " \t\n\r", &strtok_ctx);
        if (!tok) return false;
        auto base = ui * static_cast<std::size_t>(n_values_per_node_);
        data_.new_values[base] = std::atof(tok);
        // Convert interface flow from interface file units to CFS (internal)
        // Legacy: newQual[0] /= Qcf[IfaceFlowUnits]  (iface.c readNewIfaceValues)
        if (iface_flow_units_ >= 0 && iface_flow_units_ <= 5)
            data_.new_values[base] /= ucf::Qcf[iface_flow_units_];

        // Parse pollutant values
        for (int j = 1; j <= n_iface_polluts_; ++j) {
            tok = OPENSWMM_STRTOK(nullptr, " \t\n\r", &strtok_ctx);
            if (!tok) return false;
            data_.new_values[base + static_cast<std::size_t>(j)] = std::atof(tok);
        }

        // Encode the date/time as decimal days using DateTime.hpp
        // All nodes share the same date — last iteration's value is used
        new_time_ = datetime::encodeDate(yr, mon, day)
                  + datetime::encodeTime(hr, mn, sec);
    }

    return true;
}

// ============================================================================
// setOldValues — shift new values to old for next interpolation
// ============================================================================

void InterfaceManager::setOldValues() {
    old_time_ = new_time_;
    auto total = static_cast<std::size_t>(data_.count) *
                 static_cast<std::size_t>(n_values_per_node_);
    for (std::size_t i = 0; i < total; ++i) {
        data_.old_values[i] = data_.new_values[i];
    }
}

// ============================================================================
// readInflows — update interface inflows for the current simulation time
// ============================================================================

void InterfaceManager::readInflows(SimulationContext& ctx, double current_time) {
    if (!infile_ || data_.count == 0) return;

    // On first call, read the header if not yet done
    if (data_.old_values.empty()) {
        if (readFileHeader(ctx) != 0) {
            closeFiles();
            return;
        }
    }

    // Return if interface file starts after current time
    if (old_time_ > current_time) return;

    // Advance until current time is bracketed by old/new times
    while (new_time_ < current_time && new_time_ != NO_DATE) {
        setOldValues();
        readNextPeriod();
    }

    // If no more data, stop
    if (new_time_ == NO_DATE) return;

    // Compute interpolation fraction
    double dt_iface = new_time_ - old_time_;
    if (dt_iface > 0.0) {
        iface_frac_ = (current_time - old_time_) / dt_iface;
        iface_frac_ = std::max(0.0, std::min(1.0, iface_frac_));
    } else {
        iface_frac_ = 0.0;
    }

    // Add interpolated interface flows to node lateral inflows
    for (int i = 0; i < data_.count; ++i) {
        int node = data_.node_idx[static_cast<std::size_t>(i)];
        if (node < 0 || node >= ctx.n_nodes()) continue;

        double flow = getFlow(i, iface_frac_);
        ctx.nodes.iface_inflow[static_cast<std::size_t>(node)] += flow;

        // Add interpolated quality values
        int n_polluts = ctx.n_pollutants();
        for (int p = 0; p < n_polluts; ++p) {
            if (pollut_map_.empty()) continue;
            double qual = getQuality(i, p, iface_frac_);
            if (qual > 0.0) {
                // Add quality loading to node
                // (full implementation would use mass-weighted mixing)
                // ctx.nodes.qual[node * n_polluts + p] += qual;
            }
        }
    }
}

// ============================================================================
// writeOutfallResults — save outfall flows/quality to the output interface file
// ============================================================================

void InterfaceManager::writeOutfallResults(const SimulationContext& ctx,
                                            double current_time) {
    if (!outfile_) return;

    // Decode current_time (Julian decimal days) into yr/mon/day/hr/min/sec
    // using DateTime.hpp — matching legacy datetime_decodeDate/Time
    int yr, mon, day, hr, mn, sec;
    datetime::decodeDate(current_time, yr, mon, day);
    datetime::decodeTime(current_time, hr, mn, sec);

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (!isOutletNode(ctx, i)) continue;

        // Write node ID
        const std::string& name = ctx.node_names.names()[ui];
        std::fprintf(outfile_, "\n%-16s", name.c_str());

        // Write date/time
        std::fprintf(outfile_, " %04d %02d  %02d  %02d  %02d  %02d ",
                     yr, mon, day, hr, mn, sec);

        // Write flow (in output flow units)
        std::fprintf(outfile_, " %-10f", ctx.nodes.inflow[ui]);

        // Write pollutant concentrations
        int np = ctx.n_pollutants();
        for (int p = 0; p < np; ++p) {
            auto qi = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            double conc = (qi < ctx.nodes.conc.size())
                          ? ctx.nodes.conc[qi] : 0.0;
            std::fprintf(outfile_, " %-10f", conc);
        }
    }
}

// ============================================================================
// writeFileHeader — write the interface file header for output
// ============================================================================

void InterfaceManager::writeFileHeader(const SimulationContext& ctx) {
    if (!outfile_) return;

    std::fprintf(outfile_, "SWMM5 Interface File");
    std::fprintf(outfile_, "\n%s", "OpenSWMM Engine Output");

    // Reporting timestep
    int report_step = static_cast<int>(ctx.options.report_step);
    std::fprintf(outfile_, "\n%-4d - reporting time step in sec", report_step);

    // Number of constituents (FLOW + pollutants)
    int n_polluts = ctx.n_pollutants();
    std::fprintf(outfile_, "\n%-4d - number of constituents as listed below:",
                 n_polluts + 1);
    static const char* flow_words[] = {"CFS","GPM","MGD","CMS","LPS","MLD"};
    int fu = static_cast<int>(ctx.options.flow_units);
    if (fu < 0 || fu > 5) fu = 0;
    std::fprintf(outfile_, "\nFLOW %s", flow_words[fu]);

    // Write pollutant names
    for (int i = 0; i < n_polluts; ++i) {
        const std::string& pname = ctx.pollutant_names.names()[static_cast<std::size_t>(i)];
        // Pollutant concentration units (matching legacy: MG/L, UG/L, #/L)
        // Default to MG/L; full implementation would read from PollutantData
        std::fprintf(outfile_, "\n%s MG/L", pname.c_str());
    }

    // Count outlet nodes
    int n_outlets = 0;
    for (int i = 0; i < ctx.n_nodes(); ++i) {
        if (isOutletNode(ctx, i)) n_outlets++;
    }

    // Write outlet node names
    std::fprintf(outfile_, "\n%-4d - number of nodes as listed below:", n_outlets);
    for (int i = 0; i < ctx.n_nodes(); ++i) {
        if (!isOutletNode(ctx, i)) continue;
        const std::string& nname = ctx.node_names.names()[static_cast<std::size_t>(i)];
        std::fprintf(outfile_, "\n%s", nname.c_str());
    }

    // Column headings
    std::fprintf(outfile_, "\nNode             Year Mon Day Hr  Min Sec FLOW      ");
    for (int i = 0; i < n_polluts; ++i) {
        const std::string& pname = ctx.pollutant_names.names()[static_cast<std::size_t>(i)];
        std::fprintf(outfile_, " %-10s", pname.c_str());
    }
}

// ============================================================================
// closeFiles
// ============================================================================

void InterfaceManager::closeFiles() {
    if (infile_) {
        std::fclose(infile_);
        infile_ = nullptr;
    }
    if (outfile_) {
        std::fclose(outfile_);
        outfile_ = nullptr;
    }
}

// ============================================================================
// getFlow — interpolated flow for an interface node
// ============================================================================

double InterfaceManager::getFlow(int index, double frac) const {
    if (index < 0 || index >= data_.count) return 0.0;

    auto base = static_cast<std::size_t>(index) *
                static_cast<std::size_t>(n_values_per_node_);
    double q1 = data_.old_values[base];
    double q2 = data_.new_values[base];
    return (1.0 - frac) * q1 + frac * q2;
}

// ============================================================================
// getQuality — interpolated quality for an interface node and pollutant
// ============================================================================

double InterfaceManager::getQuality(int index, int pollut_idx, double frac) const {
    if (index < 0 || index >= data_.count) return 0.0;
    if (pollut_idx < 0 ||
        pollut_idx >= static_cast<int>(pollut_map_.size())) return 0.0;

    int col = pollut_map_[static_cast<std::size_t>(pollut_idx)];
    if (col < 0) return 0.0;

    auto base = static_cast<std::size_t>(index) *
                static_cast<std::size_t>(n_values_per_node_);
    auto offset = base + static_cast<std::size_t>(col + 1);  // +1 for flow column

    double c1 = data_.old_values[offset];
    double c2 = data_.new_values[offset];
    return (1.0 - frac) * c1 + frac * c2;
}

// ============================================================================
// getIfaceNode — project node index for a given interface file entry
// ============================================================================

int InterfaceManager::getIfaceNode(int index) const {
    if (index >= 0 && index < data_.count)
        return data_.node_idx[static_cast<std::size_t>(index)];
    return -1;
}

// ============================================================================
// isOutletNode — check if a node is an outlet (outfall or terminal)
// ============================================================================

bool InterfaceManager::isOutletNode(const SimulationContext& ctx, int node_idx) {
    auto ui = static_cast<std::size_t>(node_idx);

    // For dynamic wave routing, only outfalls are outlets
    if (ctx.options.routing_model == RoutingModel::DYNWAVE) {
        return ctx.nodes.type[ui] == NodeType::OUTFALL;
    }

    // For other routing methods, outlets are nodes with no outflow links
    return ctx.nodes.degree[ui] == 0;
}

} // namespace iface
} // namespace openswmm

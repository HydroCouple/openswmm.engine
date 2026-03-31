/**
 * @file InflowData.hpp
 * @brief SoA stores for external inflows, DWF, RDII, and time patterns.
 *
 * @details Persistent data for .inp round-trip. Separate from the runtime
 *          InflowSolver which holds transient computation state.
 *
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_INFLOW_DATA_HPP
#define OPENSWMM_ENGINE_INFLOW_DATA_HPP

#include <vector>
#include <string>

namespace openswmm {

// ============================================================================
// External inflow definitions (from [INFLOWS] section)
// ============================================================================

struct ExtInflowData {
    int count() const { return static_cast<int>(node_idx.size()); }

    std::vector<int>         node_idx;       ///< Target node index
    std::vector<std::string> constituent;    ///< "FLOW" or pollutant name
    std::vector<std::string> ts_name;        ///< Timeseries name ("" if none)
    std::vector<std::string> inflow_type;    ///< "FLOW","CONCEN","MASS"
    std::vector<double>      m_factor;       ///< Multiplier factor
    std::vector<double>      s_factor;       ///< Scaling factor
    std::vector<double>      baseline;       ///< Baseline value
    std::vector<std::string> pattern_name;   ///< Baseline pattern name

    void add(int ni, const std::string& cons, const std::string& ts,
             const std::string& type, double mf, double sf, double base,
             const std::string& pat) {
        node_idx.push_back(ni); constituent.push_back(cons);
        ts_name.push_back(ts); inflow_type.push_back(type);
        m_factor.push_back(mf); s_factor.push_back(sf);
        baseline.push_back(base); pattern_name.push_back(pat);
    }
};

// ============================================================================
// Dry weather flow definitions (from [DWF] section)
// ============================================================================

struct DwfData {
    int count() const { return static_cast<int>(node_idx.size()); }

    std::vector<int>         node_idx;       ///< Target node
    std::vector<std::string> constituent;    ///< "FLOW" or pollutant name
    std::vector<double>      avg_value;      ///< Average value
    std::vector<std::string> pat1;           ///< Monthly pattern name
    std::vector<std::string> pat2;           ///< Daily pattern name
    std::vector<std::string> pat3;           ///< Hourly pattern name
    std::vector<std::string> pat4;           ///< Weekend pattern name

    void add(int ni, const std::string& cons, double avg,
             const std::string& p1, const std::string& p2,
             const std::string& p3, const std::string& p4) {
        node_idx.push_back(ni); constituent.push_back(cons);
        avg_value.push_back(avg);
        pat1.push_back(p1); pat2.push_back(p2);
        pat3.push_back(p3); pat4.push_back(p4);
    }
};

// ============================================================================
// RDII assignments (from [RDII] section)
// ============================================================================

struct RDIIAssignData {
    int count() const { return static_cast<int>(node_idx.size()); }

    std::vector<int>         node_idx;    ///< Target node
    std::vector<std::string> uh_name;     ///< Unit hydrograph name
    std::vector<double>      sewer_area;  ///< Tributary sewer area

    void add(int ni, const std::string& uh, double area) {
        node_idx.push_back(ni); uh_name.push_back(uh);
        sewer_area.push_back(area);
    }
};

// ============================================================================
// Unit Hydrograph data (from [HYDROGRAPHS] section)
// ============================================================================

struct UnitHydEntry {
    std::string name;       ///< UH group name
    std::string gage_name;  ///< Associated rain gage name
    int month;              ///< Month index (0-11, or -1 for ALL)
    int response;           ///< 0=SHORT, 1=MEDIUM, 2=LONG
    double r;               ///< Fraction of rainfall volume
    double t;               ///< Time to peak (hours)
    double k;               ///< Recession limb ratio (tBase/tPeak)
    double dmax;            ///< Max initial abstraction depth
    double drecov;          ///< IA recovery rate
    double dinit;           ///< Initial IA used
};

struct UnitHydData {
    int count() const { return static_cast<int>(entries.size()); }

    std::vector<UnitHydEntry> entries;

    /// Rain gage names associated with each UH group (name → gage name)
    std::vector<std::string> gage_assignments; ///< UH group names
    std::vector<std::string> gage_names;       ///< Assigned rain gage names

    void add_gage(const std::string& uh_name, const std::string& gage) {
        gage_assignments.push_back(uh_name);
        gage_names.push_back(gage);
    }

    void add(const UnitHydEntry& e) { entries.push_back(e); }
};

// ============================================================================
// Time patterns (from [PATTERNS] section)
// ============================================================================

struct PatternData {
    int count() const { return static_cast<int>(names.size()); }

    std::vector<std::string> names;           ///< Pattern name
    std::vector<int>         types;           ///< 0=MONTHLY,1=DAILY,2=HOURLY,3=WEEKEND
    std::vector<std::vector<double>> factors; ///< Up to 24 multiplier values

    void add(const std::string& name, int type, const std::vector<double>& facs) {
        names.push_back(name); types.push_back(type);
        factors.push_back(facs);
    }
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_INFLOW_DATA_HPP

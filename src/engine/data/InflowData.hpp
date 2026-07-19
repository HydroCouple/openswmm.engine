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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
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
    std::vector<std::string> node_name;      ///< Raw node name from input (for post-parse re-resolution)
    std::vector<std::string> constituent;    ///< "FLOW" or pollutant name
    std::vector<std::string> ts_name;        ///< Timeseries name ("" if none)
    std::vector<std::string> inflow_type;    ///< "FLOW","CONCEN","MASS"
    std::vector<double>      m_factor;       ///< Multiplier factor
    std::vector<double>      s_factor;       ///< Scaling factor
    std::vector<double>      baseline;       ///< Baseline value
    std::vector<std::string> pattern_name;   ///< Baseline pattern name

    void add(int ni, const std::string& cons, const std::string& ts,
             const std::string& type, double mf, double sf, double base,
             const std::string& pat, const std::string& name = {}) {
        node_idx.push_back(ni); node_name.push_back(name);
        constituent.push_back(cons);
        ts_name.push_back(ts); inflow_type.push_back(type);
        m_factor.push_back(mf); s_factor.push_back(sf);
        baseline.push_back(base); pattern_name.push_back(pat);
    }

    /// Remove the entry at @p idx. No-op if out of range. Subsequent entries
    /// shift down by one — callers that hold cached indices must re-resolve.
    void erase(int idx) {
        if (idx < 0 || idx >= count()) return;
        const auto u = static_cast<std::size_t>(idx);
        node_idx.erase(node_idx.begin() + u);
        node_name.erase(node_name.begin() + u);
        constituent.erase(constituent.begin() + u);
        ts_name.erase(ts_name.begin() + u);
        inflow_type.erase(inflow_type.begin() + u);
        m_factor.erase(m_factor.begin() + u);
        s_factor.erase(s_factor.begin() + u);
        baseline.erase(baseline.begin() + u);
        pattern_name.erase(pattern_name.begin() + u);
    }
};

// ============================================================================
// Dry weather flow definitions (from [DWF] section)
// ============================================================================

struct DwfData {
    int count() const { return static_cast<int>(node_idx.size()); }

    std::vector<int>         node_idx;       ///< Target node
    std::vector<std::string> node_name;      ///< Raw node name from input (for post-parse re-resolution)
    std::vector<std::string> constituent;    ///< "FLOW" or pollutant name
    std::vector<double>      avg_value;      ///< Average value
    std::vector<std::string> pat1;           ///< Monthly pattern name
    std::vector<std::string> pat2;           ///< Daily pattern name
    std::vector<std::string> pat3;           ///< Hourly pattern name
    std::vector<std::string> pat4;           ///< Weekend pattern name

    void add(int ni, const std::string& cons, double avg,
             const std::string& p1, const std::string& p2,
             const std::string& p3, const std::string& p4,
             const std::string& name = {}) {
        node_idx.push_back(ni); node_name.push_back(name);
        constituent.push_back(cons);
        avg_value.push_back(avg);
        pat1.push_back(p1); pat2.push_back(p2);
        pat3.push_back(p3); pat4.push_back(p4);
    }

    void erase(int idx) {
        if (idx < 0 || idx >= count()) return;
        const auto u = static_cast<std::size_t>(idx);
        node_idx.erase(node_idx.begin() + u);
        node_name.erase(node_name.begin() + u);
        constituent.erase(constituent.begin() + u);
        avg_value.erase(avg_value.begin() + u);
        pat1.erase(pat1.begin() + u);
        pat2.erase(pat2.begin() + u);
        pat3.erase(pat3.begin() + u);
        pat4.erase(pat4.begin() + u);
    }
};

// ============================================================================
// RDII assignments (from [RDII] section)
// ============================================================================

struct RDIIAssignData {
    int count() const { return static_cast<int>(node_idx.size()); }

    std::vector<int>         node_idx;    ///< Target node
    std::vector<std::string> node_name;   ///< Raw node name from input (for post-parse re-resolution)
    std::vector<std::string> uh_name;     ///< Unit hydrograph name
    std::vector<double>      sewer_area;  ///< Tributary sewer area

    void add(int ni, const std::string& uh, double area,
             const std::string& name = {}) {
        node_idx.push_back(ni); node_name.push_back(name);
        uh_name.push_back(uh);
        sewer_area.push_back(area);
    }

    void erase(int idx) {
        if (idx < 0 || idx >= count()) return;
        const auto u = static_cast<std::size_t>(idx);
        node_idx.erase(node_idx.begin() + u);
        node_name.erase(node_name.begin() + u);
        uh_name.erase(uh_name.begin() + u);
        sewer_area.erase(sewer_area.begin() + u);
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
    double k;               ///< Recession-limb-to-peak-time ratio (tBase = t*(1+k); k >= 0)
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
// RDII exponential-decay parameters (from [RDII_DECAY] section)
// ============================================================================
//
// One row per (UH group, response). Granularity is per-response, NOT per-month
// — the whole point of the exponential model is that seasonal variation in
// IA recovery emerges from temperature dynamics, not from a monthly lookup.
//
// A UH group with no row here uses the legacy linear IA recovery for every
// response. A group with one row falls back to linear for the two unspecified
// responses, so adoption is incremental.
//
// @see docs/RDII_ExpDecay_Implementation.md

struct RDIIDecayEntry {
    std::string uh_name;    ///< Matches UnitHydEntry::name
    int    response  = -1;  ///< 0=SHORT, 1=MEDIUM, 2=LONG
    double k_dep     = 0.0; ///< Depletion rate (1/project rain-depth unit: 1/in or 1/mm) — temperature-independent
    double k_0       = 0.0; ///< Base recovery rate (1/hr)
    double k_T       = 0.0; ///< Thermal recovery rate at T_ref (1/hr)
    double T_ref     = 10.0;///< Reference temperature (deg C)
    double theta_rec = 0.0; ///< Temperature sensitivity (1/deg C)
    double T_freeze  = 0.0; ///< Recovery suppressed when T < T_freeze (deg C)

    // Optional degree-day snow model (SNOW keyword + 2 extra columns).
    // When on, precipitation at T <= snow_T accumulates as SWE (no liquid
    // input); at T > snow_T, melt = min(SWE, snow_ddf*(T - snow_T)*dt_days)
    // is added to rainfall before the IA update (rain-on-snow adds).
    bool   snow_on   = false; ///< Degree-day snow model enabled for this row
    double snow_T    = 1.0;   ///< Rain/snow partition threshold & melt base (deg C)
    double snow_ddf  = 0.0;   ///< Degree-day melt factor (project rain-depth unit/degC/day)
};

struct RDIIDecayData {
    int count() const { return static_cast<int>(entries.size()); }

    std::vector<RDIIDecayEntry> entries;

    void add(const RDIIDecayEntry& e) { entries.push_back(e); }
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

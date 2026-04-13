/**
 * @file Controls.cpp
 * @brief Rule-based control engine — full legacy parity implementation.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Controls.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/DateTime.hpp"
#include "../core/UnitConversion.hpp"
#include "../hydraulics/XSectBatch.hpp"
#include "../hydraulics/Link.hpp"
#include "../data/TableData.hpp"

#include <cmath>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <unordered_map>

namespace openswmm {
namespace controls {

static constexpr double MISSING = -1.0e10;

void ControlEngine::init(const std::vector<Rule>& rules) {
    rules_ = rules;
    pid_states_.clear();
    for (int r = 0; r < static_cast<int>(rules_.size()); ++r) {
        for (auto& a : rules_[static_cast<size_t>(r)].then_actions) a.rule_idx = r;
        for (auto& a : rules_[static_cast<size_t>(r)].else_actions) a.rule_idx = r;
    }
    buildPremiseSoA();
    // link_time_last_set_ is sized lazily in applyPendingActions
    // when we first know the link count from ctx.
    link_time_last_set_.clear();
}

// ============================================================================
// Build SoA premise index — group by variable type for batch evaluation
// ============================================================================

void ControlEngine::buildPremiseSoA() {
    premise_groups_.clear();

    // Count premises per variable type
    std::unordered_map<int, int> type_counts;
    for (int r = 0; r < static_cast<int>(rules_.size()); ++r) {
        for (size_t p = 0; p < rules_[static_cast<size_t>(r)].premises.size(); ++p) {
            const auto& prem = rules_[static_cast<size_t>(r)].premises[p];
            if (!prem.is_expression) {
                type_counts[static_cast<int>(prem.lhs_var)]++;
            }
        }
    }

    // Create groups
    std::unordered_map<int, size_t> type_to_group;
    for (const auto& kv : type_counts) {
        type_to_group[kv.first] = premise_groups_.size();
        PremiseSoA g;
        g.var_type = static_cast<ConditionVar>(kv.first);
        g.count = 0;
        g.rule_idx.reserve(static_cast<size_t>(kv.second));
        g.premise_idx.reserve(static_cast<size_t>(kv.second));
        g.obj_idx.reserve(static_cast<size_t>(kv.second));
        g.op.reserve(static_cast<size_t>(kv.second));
        g.rhs_value.reserve(static_cast<size_t>(kv.second));
        g.rhs_is_variable.reserve(static_cast<size_t>(kv.second));
        g.is_expression.reserve(static_cast<size_t>(kv.second));
        premise_groups_.push_back(std::move(g));
    }

    // Fill groups
    for (int r = 0; r < static_cast<int>(rules_.size()); ++r) {
        const auto& rule = rules_[static_cast<size_t>(r)];
        for (int p = 0; p < static_cast<int>(rule.premises.size()); ++p) {
            const auto& prem = rule.premises[static_cast<size_t>(p)];
            if (prem.is_expression) continue;  // expressions evaluated per-premise

            auto it = type_to_group.find(static_cast<int>(prem.lhs_var));
            if (it == type_to_group.end()) continue;
            auto& g = premise_groups_[it->second];
            g.rule_idx.push_back(r);
            g.premise_idx.push_back(p);
            g.obj_idx.push_back(prem.lhs_idx);
            g.op.push_back(static_cast<int>(prem.op));
            g.rhs_value.push_back(prem.rhs_value);
            g.rhs_is_variable.push_back(prem.rhs_is_variable);
            g.is_expression.push_back(false);
            g.count++;
        }
    }

    // Allocate working buffers
    for (auto& g : premise_groups_) {
        auto un = static_cast<size_t>(g.count);
        g.lhs_values.resize(un);
        g.results.resize(un);
    }

    // Allocate per-rule result tracking
    rule_results_.resize(rules_.size(), true);
}

// ============================================================================
// Batch evaluate one variable-type group — VECTORISABLE
// ============================================================================

void ControlEngine::batchEvaluateGroup(PremiseSoA& g,
                                        const SimulationContext& ctx,
                                        double current_time, double half_step) {
    if (g.count == 0) return;

    // Phase 1: Batch gather LHS values — single pass over one SoA array
    // This is the key vectorisation win: all gathers hit the same array type
    for (int i = 0; i < g.count; ++i) {
        auto ui = static_cast<size_t>(i);
        g.lhs_values[ui] = getVariableValue(ctx, g.var_type, g.obj_idx[ui], current_time);
    }

    // Phase 2: Batch compare — SIMD-friendly: contiguous lhs[] vs threshold[]
    bool is_time = (g.var_type == ConditionVar::SIM_TIME ||
                    g.var_type == ConditionVar::CLOCK_TIME ||
                    g.var_type == ConditionVar::LINK_TIMEOPEN ||
                    g.var_type == ConditionVar::LINK_TIMECLOSED);

    for (int i = 0; i < g.count; ++i) {
        auto ui = static_cast<size_t>(i);
        double lhs = g.lhs_values[ui];
        double rhs = g.rhs_value[ui];
        auto op = static_cast<CompareOp>(g.op[ui]);

        if (lhs == MISSING) {
            g.results[ui] = false;
        } else if (g.rhs_is_variable[ui]) {
            // Variable RHS — can't batch, evaluate individually
            // (rare case; most premises have constant RHS)
            g.results[ui] = false;  // placeholder — full eval in per-rule pass
        } else if (is_time) {
            g.results[ui] = compareTimes(lhs, op, rhs, half_step);
        } else {
            // Pure arithmetic comparison — vectorisable
            g.results[ui] = compareValues(lhs, op, rhs);
        }
    }

    // Phase 3: Scatter results to per-rule tracking
    // (The actual AND/OR combination happens in the per-rule pass in evaluate())
}

// ============================================================================
// Main evaluate — short-circuit AND, priority dedup, modulated actions
// ============================================================================

int ControlEngine::evaluate(SimulationContext& ctx, double current_time, double dt) {
    double half_step = dt / 2.0;
    pending_actions_.clear();

    // ---- Phase 1: Batch evaluate all premise groups by variable type ----
    // Each group gathers all LHS values of one type, then batch-compares.
    // This is the SIMD-friendly hot path (AD-14).
    for (auto& g : premise_groups_) {
        batchEvaluateGroup(g, ctx, current_time, half_step);
    }

    // ---- Phase 2: Combine per-rule results using AND/OR logic ----
    // Build a lookup from (rule_idx, premise_idx) → batch result
    // For premises with variable RHS or expressions, fall back to per-premise eval.
    std::fill(rule_results_.begin(), rule_results_.end(), true);

    // Scatter batch results into per-rule tracking
    for (const auto& g : premise_groups_) {
        for (int i = 0; i < g.count; ++i) {
            auto ui = static_cast<size_t>(i);
            int r = g.rule_idx[ui];
            int p_idx = g.premise_idx[ui];
            auto ur = static_cast<size_t>(r);
            auto up = static_cast<size_t>(p_idx);

            if (g.rhs_is_variable[ui]) {
                // Variable RHS — can't use batch result; evaluate per-premise
                bool pval = evaluatePremise(ctx, rules_[ur].premises[up],
                                             current_time, half_step);
                // Apply AND/OR logic
                if (p_idx == 0) {
                    rule_results_[ur] = pval;
                } else if (rules_[ur].premises[up].logic == LogicOp::AND) {
                    if (!rule_results_[ur]) continue;  // short-circuit
                    rule_results_[ur] = rule_results_[ur] && pval;
                } else {
                    if (!rule_results_[ur]) rule_results_[ur] = pval;
                }
            } else {
                // Use batch result
                bool pval = g.results[ui];
                if (p_idx == 0) {
                    rule_results_[ur] = pval;
                } else if (rules_[ur].premises[up].logic == LogicOp::AND) {
                    if (!rule_results_[ur]) continue;
                    rule_results_[ur] = rule_results_[ur] && pval;
                } else {
                    if (!rule_results_[ur]) rule_results_[ur] = pval;
                }
            }
        }
    }

    // Handle expression-based premises (not in any batch group)
    for (int r = 0; r < static_cast<int>(rules_.size()); ++r) {
        auto ur = static_cast<size_t>(r);
        const auto& rule = rules_[ur];
        for (size_t p = 0; p < rule.premises.size(); ++p) {
            const auto& prem = rule.premises[p];
            if (!prem.is_expression) continue;
            bool pval = evaluatePremise(ctx, prem, current_time, half_step);
            if (p == 0) {
                rule_results_[ur] = pval;
            } else if (prem.logic == LogicOp::AND) {
                if (!rule_results_[ur]) continue;
                rule_results_[ur] = rule_results_[ur] && pval;
            } else {
                if (!rule_results_[ur]) rule_results_[ur] = pval;
            }
        }
    }

    // ---- Phase 3: Collect actions from fired rules ----
    for (int r = 0; r < static_cast<int>(rules_.size()); ++r) {
        auto ur = static_cast<size_t>(r);
        const auto& actions = rule_results_[ur]
            ? rules_[ur].then_actions : rules_[ur].else_actions;
        for (auto a : actions) {
            updateActionValue(a, ctx, current_time, dt);
            pending_actions_.push_back({a.link_idx, a.value, rules_[ur].priority, r});
        }
    }

    last_action_count_ = applyPendingActions(ctx, current_time);
    return last_action_count_;
}

// ============================================================================
// Apply pending actions — highest priority per link wins
// ============================================================================

int ControlEngine::applyPendingActions(SimulationContext& ctx, double current_time) {
    // Lazy-init link_time_last_set_ to match link count
    if (static_cast<int>(link_time_last_set_.size()) != ctx.n_links()) {
        link_time_last_set_.assign(static_cast<size_t>(ctx.n_links()), ctx.current_date);
    }

    std::unordered_map<int, PendingAction> best;
    for (const auto& pa : pending_actions_) {
        if (pa.link_idx < 0 || pa.link_idx >= ctx.n_links()) continue;
        auto it = best.find(pa.link_idx);
        if (it == best.end() || pa.priority > it->second.priority) {
            best[pa.link_idx] = pa;
        }
    }

    int changes = 0;
    for (const auto& kv : best) {
        auto ul = static_cast<size_t>(kv.first);
        if (ctx.links.target_setting[ul] != kv.second.value) {
            ctx.links.target_setting[ul] = kv.second.value;
            // Record the time when this link's setting changed
            // (for TIMEOPEN/TIMECLOSED premise evaluation).
            // Legacy stores this as absolute date (decimal days).
            link_time_last_set_[ul] = ctx.current_date;
            changes++;
        }
    }
    return changes;
}

// ============================================================================
// Update action value (modulated controls: curve, timeseries, PID)
// ============================================================================

void ControlEngine::updateActionValue(Action& a, SimulationContext& ctx,
                                      double current_time, double dt) {
    switch (a.type) {
        case ActionType::NUMERIC:
            break;
        case ActionType::CURVE:
            // Look up action value from curve using control_value_ as x.
            // Matches legacy: a->value = table_lookup(&Curve[a->curve], ControlValue)
            if (a.curve_idx >= 0 && a.curve_idx < static_cast<int>(ctx.tables.count())) {
                a.value = table_lookup_cursor(ctx.tables[a.curve_idx], control_value_);
            }
            break;
        case ActionType::TIMESERIES:
            // Look up from timeseries using current absolute date.
            // Matches legacy: a->value = table_tseriesLookup(&Tseries[a->tseries], currentTime, TRUE)
            if (a.tseries_idx >= 0 && a.tseries_idx < static_cast<int>(ctx.tables.count())) {
                a.value = table_lookup_cursor(ctx.tables[a.tseries_idx], ctx.current_date);
            }
            break;
        case ActionType::PID:
            for (auto& pid : pid_states_) {
                if (pid.action_idx >= 0) {
                    pid.setpoint = set_point_;
                    double cur_setting = (a.link_idx >= 0 && a.link_idx < ctx.n_links())
                        ? ctx.links.target_setting[static_cast<size_t>(a.link_idx)] : 0.0;
                    bool is_pump = (a.link_idx >= 0 && a.link_idx < ctx.n_links())
                        ? (ctx.links.type[static_cast<size_t>(a.link_idx)] == LinkType::PUMP) : false;
                    a.value = computePIDSetting(pid, control_value_, cur_setting, is_pump, dt);
                    break;
                }
            }
            break;
    }
}

// ============================================================================
// Premise evaluation with expression support
// ============================================================================

bool ControlEngine::evaluatePremise(const SimulationContext& ctx,
                                     const Premise& p, double current_time,
                                     double half_step) {
    double lhs;
    if (p.is_expression && p.expr_idx >= 0 &&
        p.expr_idx < static_cast<int>(expressions_.size())) {
        lhs = mathexpr::evaluate(expressions_[static_cast<size_t>(p.expr_idx)],
            [&](const std::string& name) {
                return getNamedVariableValue(name, ctx, current_time);
            });
    } else {
        lhs = getVariableValue(ctx, p.lhs_var, p.lhs_idx, current_time, p.lhs_param);
    }
    if (lhs == MISSING) return false;

    double rhs = p.rhs_is_variable
        ? getVariableValue(ctx, p.rhs_var, p.rhs_idx, current_time)
        : p.rhs_value;
    if (rhs == MISSING) return false;

    control_value_ = lhs;
    set_point_ = rhs;

    // Time-aware comparison
    bool is_time = (p.lhs_var == ConditionVar::SIM_TIME ||
                    p.lhs_var == ConditionVar::CLOCK_TIME ||
                    p.lhs_var == ConditionVar::LINK_TIMEOPEN ||
                    p.lhs_var == ConditionVar::LINK_TIMECLOSED);
    if (is_time) {
        bool result = compareTimes(lhs, p.op, rhs, half_step);
        if (p.lhs_var == ConditionVar::LINK_TIMEOPEN ||
            p.lhs_var == ConditionVar::LINK_TIMECLOSED)
            control_value_ = lhs * 24.0;
        return result;
    }

    return compareValues(lhs, p.op, rhs);
}

// ============================================================================
// Comparisons
// ============================================================================

bool ControlEngine::compareValues(double lhs, CompareOp op, double rhs) const {
    switch (op) {
        case CompareOp::EQ: return lhs == rhs;
        case CompareOp::NE: return lhs != rhs;
        case CompareOp::LT: return lhs <  rhs;
        case CompareOp::LE: return lhs <= rhs;
        case CompareOp::GT: return lhs >  rhs;
        case CompareOp::GE: return lhs >= rhs;
    }
    return false;
}

bool ControlEngine::compareTimes(double lhs, CompareOp op, double rhs,
                                  double half_step) const {
    if (op == CompareOp::EQ)
        return (lhs >= rhs - half_step && lhs < rhs + half_step);
    if (op == CompareOp::NE)
        return (lhs < rhs - half_step || lhs >= rhs + half_step);
    return compareValues(lhs, op, rhs);
}

// ============================================================================
// Get variable value — ALL legacy attribute types
// ============================================================================

double ControlEngine::getVariableValue(const SimulationContext& ctx,
                                        ConditionVar var, int idx,
                                        double current_time, int param) const {
    auto ui = static_cast<size_t>(idx);

    // Unit conversion factors — rule thresholds are in display/project units,
    // so internal values must be converted to display units for comparison.
    // (matching legacy controls.c getVariableValue lines 1878-1936)
    int fu = static_cast<int>(ctx.options.flow_units);
    int us = ucf::getUnitSystem(fu);
    // UCF converts internal → display: display = internal * UCF
    // (matching legacy controls.c: Node[i].newDepth * UCF(LENGTH), etc.)
    double ucf_len  = ucf::Ucf[3][us];  // LENGTH
    double ucf_flow = ucf::Qcf[fu];     // FLOW: internal*UCF → display
    double ucf_vol  = ucf::Ucf[5][us];  // VOLUME

    switch (var) {
        case ConditionVar::NODE_DEPTH:
            return (idx >= 0 && idx < ctx.n_nodes()) ? ctx.nodes.depth[ui] * ucf_len : MISSING;
        case ConditionVar::NODE_MAXDEPTH:
            return (idx >= 0 && idx < ctx.n_nodes()) ? ctx.nodes.full_depth[ui] * ucf_len : MISSING;
        case ConditionVar::NODE_HEAD:
            return (idx >= 0 && idx < ctx.n_nodes()) ? ctx.nodes.head[ui] * ucf_len : MISSING;
        case ConditionVar::NODE_VOLUME:
            return (idx >= 0 && idx < ctx.n_nodes()) ? ctx.nodes.volume[ui] * ucf_vol : MISSING;
        case ConditionVar::NODE_INFLOW:
            return (idx >= 0 && idx < ctx.n_nodes()) ? ctx.nodes.lat_flow[ui] * ucf_flow : MISSING;

        case ConditionVar::LINK_FLOW:
            return (idx >= 0 && idx < ctx.n_links())
                ? ctx.links.flow[ui] * static_cast<double>(ctx.links.direction[ui]) * ucf_flow : MISSING;
        case ConditionVar::LINK_DEPTH:
            return (idx >= 0 && idx < ctx.n_links()) ? ctx.links.depth[ui] * ucf_len : MISSING;
        case ConditionVar::LINK_SETTING:
            return (idx >= 0 && idx < ctx.n_links()) ? ctx.links.setting[ui] : MISSING;
        case ConditionVar::LINK_STATUS:
            return (idx >= 0 && idx < ctx.n_links()) ? (ctx.links.is_closed[ui] ? 0.0 : 1.0) : MISSING;
        case ConditionVar::LINK_FULLFLOW:
            return (idx >= 0 && idx < ctx.n_links()) ? ctx.links.q_full[ui] * ucf_flow : MISSING;
        case ConditionVar::LINK_FULLDEPTH:
            return (idx >= 0 && idx < ctx.n_links()) ? ctx.links.xsect_y_full[ui] * ucf_len : MISSING;
        case ConditionVar::LINK_LENGTH:
            return (idx >= 0 && idx < ctx.n_links()) ? ctx.links.length[ui] * ucf_len : MISSING;
        case ConditionVar::LINK_SLOPE:
            return (idx >= 0 && idx < ctx.n_links()) ? ctx.links.slope[ui] : MISSING;
        case ConditionVar::LINK_VELOCITY:
            if (idx < 0 || idx >= ctx.n_links()) return MISSING;
            if (ctx.links.type[ui] != LinkType::CONDUIT) return MISSING;
            {
                XSectParams xs;
                xs.type = ctx.links.xsect_batch_shape[ui];
                xs.y_full = ctx.links.xsect_y_full[ui];
                xs.a_full = ctx.links.xsect_a_full[ui];
                xs.w_max = ctx.links.xsect_w_max[ui];
                return link::getVelocity(xs, ctx.links.flow[ui], ctx.links.depth[ui],
                                          ctx.links.barrels[ui]) * ucf_len;
            }
        case ConditionVar::LINK_TIMEOPEN:
            // Returns MISSING if link is closed (setting <= 0).
            // Otherwise returns elapsed time (in days) since setting last changed.
            // Matches legacy: CurrentDate + CurrentTime - Link[j].timeLastSet
            if (idx < 0 || idx >= ctx.n_links()) return MISSING;
            if (ctx.links.setting[ui] <= 0.0) return MISSING;
            if (ui < link_time_last_set_.size())
                return ctx.current_date - link_time_last_set_[ui];
            return 0.0;
        case ConditionVar::LINK_TIMECLOSED:
            // Returns MISSING if link is open (setting > 0).
            // Otherwise returns elapsed time (in days) since setting last changed.
            if (idx < 0 || idx >= ctx.n_links()) return MISSING;
            if (ctx.links.setting[ui] > 0.0) return MISSING;
            if (ui < link_time_last_set_.size())
                return ctx.current_date - link_time_last_set_[ui];
            return 0.0;

        case ConditionVar::GAGE_RAIN:
            return (idx >= 0 && idx < ctx.n_gages()) ? ctx.gages.rainfall[ui] : MISSING;
        case ConditionVar::GAGE_RAIN_PAST: {
            if (idx < 0 || idx >= ctx.n_gages()) return MISSING;
            int hours = (param > 0 && param <= GageData::MAXPASTRAIN) ? param : 1;
            double total = 0.0;
            auto base = ui * static_cast<std::size_t>(GageData::MAXPASTRAIN);
            for (int h = 0; h < hours; ++h)
                total += ctx.gages.past_rain[base + static_cast<std::size_t>(h)];
            return total;
        }

        case ConditionVar::SIM_TIME:      return current_time;
        case ConditionVar::SIM_DATE:      return ctx.current_date;
        case ConditionVar::CLOCK_TIME: {
            int ch, cm, cs;
            datetime::decodeTime(ctx.current_date, ch, cm, cs);
            return static_cast<double>(ch) + cm / 60.0 + cs / 3600.0;
        }
        case ConditionVar::SIM_DAY:
            return static_cast<double>((static_cast<int>(std::floor(ctx.current_date)) % 7) + 1);
        case ConditionVar::SIM_MONTH:
            return static_cast<double>(datetime::monthOfYear(ctx.current_date));
        case ConditionVar::SIM_DAYOFYEAR:
            return static_cast<double>(datetime::dayOfYear(ctx.current_date));
    }
    return MISSING;
}

// ============================================================================
// PID controller
// ============================================================================

double ControlEngine::computePIDSetting(PIDState& pid, double control_value,
                                         double current_setting, bool is_pump, double dt) {
    if (dt <= 0.0) return current_setting;

    // Convert dt from seconds to minutes (matching legacy: dt *= 1440 from days)
    double dt_min = dt / 60.0;
    constexpr double TINY = 1.0e-6;
    constexpr double tolerance = 0.0001;

    // Relative error (matching legacy controls.c lines 1668-1677)
    double e0 = pid.setpoint - control_value;
    if (std::fabs(e0) > TINY) {
        if (pid.setpoint != 0.0)
            e0 = e0 / pid.setpoint;
        else
            e0 = e0 / control_value;
    }

    // Anti-windup: reset previous errors if controller gets stuck
    // (matching legacy controls.c lines 1679-1683)
    if (std::fabs(e0 - pid.e1) < tolerance) {
        pid.e2 = 0.0;
        pid.e1 = 0.0;
    }

    // Recursive PID (matching legacy controls.c lines 1685-1695)
    double p = e0 - pid.e1;
    double i = (pid.ki != 0.0) ? e0 * dt_min / pid.ki : 0.0;
    double d = (dt_min > 0.0) ? pid.kd * (e0 - 2.0 * pid.e1 + pid.e2) / dt_min : 0.0;
    double update = pid.kp * (p + i + d);
    if (std::fabs(update) < tolerance) update = 0.0;

    double setting = current_setting + update;

    // Update previous errors
    pid.e2 = pid.e1;
    pid.e1 = e0;

    // Clamp to feasible range (matching legacy controls.c lines 1703-1706)
    if (setting < 0.0) setting = 0.0;
    if (!is_pump && setting > 1.0) setting = 1.0;

    return setting;
}

// ============================================================================
// Named variable support
// ============================================================================

double ControlEngine::getNamedVariableValue(const std::string& name,
                                             const SimulationContext& ctx,
                                             double current_time) const {
    for (const auto& nv : named_vars_)
        if (nv.name == name)
            return getVariableValue(ctx, nv.var, nv.idx, current_time);
    return 0.0;
}

void ControlEngine::addNamedVariable(const std::string& name, ConditionVar var, int idx) {
    named_vars_.push_back({name, var, idx});
}

int ControlEngine::addExpression(const std::string& name, const std::string& formula) {
    mathexpr::Expression expr;
    if (mathexpr::parse(formula, expr) != 0) return -1;
    int idx = static_cast<int>(expressions_.size());
    expressions_.push_back(std::move(expr));
    expr_index_[name] = idx;
    return idx;
}

// ============================================================================
// Rule text parser — full legacy [CONTROLS] section format
// ============================================================================

namespace {

/// Convert a string to upper case (ASCII only).
static std::string to_upper(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

/// Split a line into whitespace-delimited tokens.
static std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> toks;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) toks.push_back(tok);
    return toks;
}

/// Try to parse a double from a string.  Returns true on success.
static bool tryParseDouble(const std::string& s, double& val) {
    try {
        size_t pos = 0;
        val = std::stod(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

/// Match a relational operator token.  Returns CompareOp or -1 on failure.
static int matchRelOp(const std::string& tok) {
    if (tok == "=")  return static_cast<int>(CompareOp::EQ);
    if (tok == "<>") return static_cast<int>(CompareOp::NE);
    if (tok == "<")  return static_cast<int>(CompareOp::LT);
    if (tok == "<=") return static_cast<int>(CompareOp::LE);
    if (tok == ">")  return static_cast<int>(CompareOp::GT);
    if (tok == ">=") return static_cast<int>(CompareOp::GE);
    return -1;
}

/// Map attribute keyword to ConditionVar for node attributes.
static int nodeAttribute(const std::string& attr) {
    if (attr == "DEPTH")    return static_cast<int>(ConditionVar::NODE_DEPTH);
    if (attr == "MAXDEPTH") return static_cast<int>(ConditionVar::NODE_MAXDEPTH);
    if (attr == "HEAD")     return static_cast<int>(ConditionVar::NODE_HEAD);
    if (attr == "VOLUME")   return static_cast<int>(ConditionVar::NODE_VOLUME);
    if (attr == "INFLOW")   return static_cast<int>(ConditionVar::NODE_INFLOW);
    return -1;
}

/// Map attribute keyword to ConditionVar for link attributes.
static int linkAttribute(const std::string& attr) {
    if (attr == "FLOW")       return static_cast<int>(ConditionVar::LINK_FLOW);
    if (attr == "DEPTH")      return static_cast<int>(ConditionVar::LINK_DEPTH);
    if (attr == "SETTING")    return static_cast<int>(ConditionVar::LINK_SETTING);
    if (attr == "STATUS")     return static_cast<int>(ConditionVar::LINK_STATUS);
    if (attr == "FULLFLOW")   return static_cast<int>(ConditionVar::LINK_FULLFLOW);
    if (attr == "FULLDEPTH")  return static_cast<int>(ConditionVar::LINK_FULLDEPTH);
    if (attr == "VELOCITY")   return static_cast<int>(ConditionVar::LINK_VELOCITY);
    if (attr == "LENGTH")     return static_cast<int>(ConditionVar::LINK_LENGTH);
    if (attr == "SLOPE")      return static_cast<int>(ConditionVar::LINK_SLOPE);
    if (attr == "TIMEOPEN")   return static_cast<int>(ConditionVar::LINK_TIMEOPEN);
    if (attr == "TIMECLOSED") return static_cast<int>(ConditionVar::LINK_TIMECLOSED);
    return -1;
}

/// Map attribute keyword to ConditionVar for simulation (system) attributes.
static int simAttribute(const std::string& attr) {
    if (attr == "TIME")      return static_cast<int>(ConditionVar::SIM_TIME);
    if (attr == "DATE")      return static_cast<int>(ConditionVar::SIM_DATE);
    if (attr == "CLOCKTIME") return static_cast<int>(ConditionVar::CLOCK_TIME);
    if (attr == "DAY")       return static_cast<int>(ConditionVar::SIM_DAY);
    if (attr == "MONTH")     return static_cast<int>(ConditionVar::SIM_MONTH);
    if (attr == "DAYOFYEAR") return static_cast<int>(ConditionVar::SIM_DAYOFYEAR);
    return -1;
}

/// Parse a premise variable from tokens starting at position k.
/// On success, returns true and advances k past the consumed tokens.
/// Fills out cv (condition variable type) and obj_idx (object index).
static bool parsePremiseVariable(const std::vector<std::string>& toks, int& k,
                                 const SimulationContext& ctx,
                                 ConditionVar& cv, int& obj_idx,
                                 int& extra_param) {
    extra_param = 0;
    if (k >= static_cast<int>(toks.size())) return false;
    std::string obj_type = to_upper(toks[static_cast<size_t>(k)]);

    if (obj_type == "NODE") {
        if (k + 2 >= static_cast<int>(toks.size())) return false;
        obj_idx = ctx.node_names.find(toks[static_cast<size_t>(k + 1)]);
        if (obj_idx < 0) return false;
        std::string attr = to_upper(toks[static_cast<size_t>(k + 2)]);
        int a = nodeAttribute(attr);
        if (a < 0) return false;
        cv = static_cast<ConditionVar>(a);
        k += 3;
        return true;
    }
    if (obj_type == "LINK" || obj_type == "CONDUIT" || obj_type == "PUMP" ||
        obj_type == "ORIFICE" || obj_type == "WEIR" || obj_type == "OUTLET") {
        if (k + 2 >= static_cast<int>(toks.size())) return false;
        obj_idx = ctx.link_names.find(toks[static_cast<size_t>(k + 1)]);
        if (obj_idx < 0) return false;
        std::string attr = to_upper(toks[static_cast<size_t>(k + 2)]);
        int a = linkAttribute(attr);
        if (a < 0) return false;
        cv = static_cast<ConditionVar>(a);
        k += 3;
        return true;
    }
    if (obj_type == "GAGE") {
        if (k + 2 >= static_cast<int>(toks.size())) return false;
        obj_idx = ctx.gage_names.find(toks[static_cast<size_t>(k + 1)]);
        if (obj_idx < 0) return false;
        std::string attr = to_upper(toks[static_cast<size_t>(k + 2)]);
        if (attr == "INTENSITY") {
            cv = ConditionVar::GAGE_RAIN;
        } else {
            // n-hour past rain: attribute is a number
            double nh = 0.0;
            if (!tryParseDouble(attr, nh) || nh < 1.0) return false;
            cv = ConditionVar::GAGE_RAIN_PAST;
            extra_param = static_cast<int>(nh);
        }
        k += 3;
        return true;
    }
    if (obj_type == "SIMULATION") {
        if (k + 1 >= static_cast<int>(toks.size())) return false;
        std::string attr = to_upper(toks[static_cast<size_t>(k + 1)]);
        int a = simAttribute(attr);
        if (a < 0) return false;
        cv = static_cast<ConditionVar>(a);
        obj_idx = -1;
        k += 2;
        return true;
    }
    return false;
}

/// Parse a status word to a numeric value.
static bool parseStatusValue(const std::string& tok, double& val) {
    std::string u = to_upper(tok);
    if (u == "OFF" || u == "CLOSED") { val = 0.0; return true; }
    if (u == "ON"  || u == "OPEN")   { val = 1.0; return true; }
    return false;
}

} // anonymous namespace

int ControlEngine::parseRuleText(const std::string& text, SimulationContext& ctx) {
    // Split the text into lines
    std::vector<std::string> lines;
    {
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) {
            // Trim leading/trailing whitespace
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            size_t end = line.find_last_not_of(" \t\r\n");
            std::string trimmed = line.substr(start, end - start + 1);
            if (!trimmed.empty()) lines.push_back(trimmed);
        }
    }

    if (lines.empty()) return 0;

    // State machine: parse lines into rules
    enum class ParseState { IDLE, IN_PREMISES, IN_THEN, IN_ELSE };
    ParseState state = ParseState::IDLE;
    Rule current_rule;
    int rules_parsed = 0;

    auto finishRule = [&]() {
        if (!current_rule.name.empty()) {
            rules_.push_back(std::move(current_rule));
            rules_parsed++;
        }
        current_rule = Rule{};
        state = ParseState::IDLE;
    };

    for (const auto& line : lines) {
        auto toks = tokenize(line);
        if (toks.empty()) continue;

        std::string keyword = to_upper(toks[0]);

        // ---- RULE keyword ----
        if (keyword == "RULE") {
            // If we were building a previous rule, finish it
            if (state != ParseState::IDLE) finishRule();
            if (toks.size() < 2) return -1;  // error: no rule name
            current_rule.name = toks[1];
            state = ParseState::IDLE;  // wait for IF
            continue;
        }

        // ---- PRIORITY keyword ----
        if (keyword == "PRIORITY") {
            if (toks.size() < 2) return -1;
            double p = 0.0;
            if (!tryParseDouble(toks[1], p)) return -1;
            current_rule.priority = p;
            continue;
        }

        // ---- IF / AND / OR keywords (premises) ----
        if (keyword == "IF" || keyword == "AND" || keyword == "OR") {

            // Determine if this is a premise line or action continuation
            bool is_premise_context = (keyword == "IF") ||
                                       (state == ParseState::IN_PREMISES);

            if (keyword == "IF") {
                state = ParseState::IN_PREMISES;
            }

            // AND/OR after THEN/ELSE → action continuation
            if ((keyword == "AND" || keyword == "OR") &&
                (state == ParseState::IN_THEN || state == ParseState::IN_ELSE)) {
                is_premise_context = false;
            }

            if (is_premise_context) {
                // Parse premise: <object> <name> <attribute> <relop> <value>
                Premise prem;
                prem.logic = (keyword == "OR") ? LogicOp::OR : LogicOp::AND;

                int k = 1;  // start after IF/AND/OR keyword

                // Parse LHS variable
                ConditionVar lhs_cv;
                int lhs_idx = -1;
                int lhs_param = 0;
                if (!parsePremiseVariable(toks, k, ctx, lhs_cv, lhs_idx, lhs_param))
                    return -1;
                prem.lhs_var = lhs_cv;
                prem.lhs_idx = lhs_idx;
                prem.lhs_param = lhs_param;

                // Parse relational operator
                if (k >= static_cast<int>(toks.size())) return -1;
                int rel = matchRelOp(toks[static_cast<size_t>(k)]);
                if (rel < 0) return -1;
                prem.op = static_cast<CompareOp>(rel);
                k++;

                // Parse RHS: either a variable or a constant value
                if (k >= static_cast<int>(toks.size())) return -1;

                // Try to parse as a variable first
                int saved_k = k;
                ConditionVar rhs_cv;
                int rhs_idx = -1;
                int rhs_param = 0;
                if (parsePremiseVariable(toks, k, ctx, rhs_cv, rhs_idx, rhs_param)) {
                    prem.rhs_is_variable = true;
                    prem.rhs_var = rhs_cv;
                    prem.rhs_idx = rhs_idx;
                } else {
                    // Parse as constant value
                    k = saved_k;
                    std::string val_tok = toks[static_cast<size_t>(k)];
                    double val = 0.0;

                    // Handle STATUS values (OFF/ON/CLOSED/OPEN)
                    if (lhs_cv == ConditionVar::LINK_STATUS) {
                        if (!parseStatusValue(val_tok, val)) return -1;
                    } else {
                        if (!tryParseDouble(val_tok, val)) return -1;
                    }
                    prem.rhs_value = val;
                    k++;
                }

                current_rule.premises.push_back(prem);
                continue;
            }

            // Fall through: AND after THEN/ELSE is handled as action below
        }

        // ---- THEN keyword ----
        if (keyword == "THEN") {
            state = ParseState::IN_THEN;
        }

        // ---- ELSE keyword ----
        if (keyword == "ELSE") {
            state = ParseState::IN_ELSE;
        }

        // ---- Parse action (THEN/ELSE/AND after THEN or ELSE) ----
        if (state == ParseState::IN_THEN || state == ParseState::IN_ELSE) {
            // Action format: [THEN/ELSE/AND] <object> <name> <attribute> = <value>
            // Skip the keyword token
            int k = 1;
            if (keyword == "THEN" || keyword == "ELSE" ||
                keyword == "AND" || keyword == "OR") {
                k = 1;
            }

            if (k >= static_cast<int>(toks.size())) continue;

            // Parse object type (must be a link type)
            std::string obj_type = to_upper(toks[static_cast<size_t>(k)]);
            if (obj_type != "LINK" && obj_type != "CONDUIT" && obj_type != "PUMP" &&
                obj_type != "ORIFICE" && obj_type != "WEIR" && obj_type != "OUTLET")
                return -1;
            k++;

            // Parse link name
            if (k >= static_cast<int>(toks.size())) return -1;
            int link_idx = ctx.link_names.find(toks[static_cast<size_t>(k)]);
            if (link_idx < 0) return -1;
            k++;

            // Parse attribute (STATUS or SETTING)
            if (k >= static_cast<int>(toks.size())) return -1;
            std::string attr = to_upper(toks[static_cast<size_t>(k)]);
            k++;

            // Skip '=' token
            if (k >= static_cast<int>(toks.size())) return -1;
            if (toks[static_cast<size_t>(k)] != "=") return -1;
            k++;

            // Parse value / control type
            if (k >= static_cast<int>(toks.size())) return -1;

            Action action;
            action.link_idx = link_idx;

            std::string val_tok = to_upper(toks[static_cast<size_t>(k)]);

            if (val_tok == "CURVE") {
                // Modulated by curve: next token is curve name
                k++;
                if (k >= static_cast<int>(toks.size())) return -1;
                int ci = ctx.table_names.find(toks[static_cast<size_t>(k)]);
                if (ci < 0) return -1;
                action.type = ActionType::CURVE;
                action.curve_idx = ci;
            } else if (val_tok == "TIMESERIES") {
                // Modulated by timeseries: next token is timeseries name
                k++;
                if (k >= static_cast<int>(toks.size())) return -1;
                int ti = ctx.table_names.find(toks[static_cast<size_t>(k)]);
                if (ti < 0) return -1;
                action.type = ActionType::TIMESERIES;
                action.tseries_idx = ti;
            } else if (val_tok == "PID") {
                // PID controller: next 3 tokens are Kp Ki Kd
                action.type = ActionType::PID;
                k++;
                if (k + 2 >= static_cast<int>(toks.size())) return -1;
                double kp = 0.0, ki = 0.0, kd = 0.0;
                if (!tryParseDouble(toks[static_cast<size_t>(k)], kp)) return -1;
                if (!tryParseDouble(toks[static_cast<size_t>(k + 1)], ki)) return -1;
                if (!tryParseDouble(toks[static_cast<size_t>(k + 2)], kd)) return -1;
                PIDState pid;
                pid.kp = kp;
                pid.ki = ki;
                pid.kd = kd;
                pid.action_idx = static_cast<int>(pid_states_.size());
                pid_states_.push_back(pid);
                action.value = 0.0;
            } else {
                // Direct numeric or status value
                action.type = ActionType::NUMERIC;
                if (attr == "STATUS") {
                    if (!parseStatusValue(val_tok, action.value)) return -1;
                } else {
                    double v = 0.0;
                    if (!tryParseDouble(toks[static_cast<size_t>(k)], v)) return -1;
                    action.value = v;
                }
            }

            if (state == ParseState::IN_THEN)
                current_rule.then_actions.push_back(action);
            else
                current_rule.else_actions.push_back(action);
            continue;
        }
    }

    // Finish the last rule
    finishRule();

    // Re-index rule_idx on all actions
    for (int r = 0; r < static_cast<int>(rules_.size()); ++r) {
        for (auto& a : rules_[static_cast<size_t>(r)].then_actions) a.rule_idx = r;
        for (auto& a : rules_[static_cast<size_t>(r)].else_actions) a.rule_idx = r;
    }

    // Rebuild SoA index for batch evaluation
    buildPremiseSoA();

    return rules_parsed;
}

} // namespace controls
} // namespace openswmm

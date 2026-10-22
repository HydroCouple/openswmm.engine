// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      Controls.hpp
 * @brief     Rule-based control evaluation, parsing and action arbitration.
 *
 * @details Implements the following legacy controls.c capabilities:
 *   - All condition variable types (node/link/gage/system attributes)
 *   - All action types (direct, curve-based, timeseries-based, PID)
 *   - Named variables and math expression evaluation
 *   - Time-aware comparison with half-step tolerance
 *   - Priority-based action deduplication
 *   - Ordered AND/OR reduction over pre-evaluated premise values
 *
 * @note Legacy reference: src/legacy/engine/controls.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_controls Control rules
 * @ingroup new_engine
 * @brief Parse, evaluate and arbitrate simulation control conditions and actions.
 *
 * Interfaces describe borrowed simulation data, clock conventions, rule state
 * and the ordering of action publication.
 */

#ifndef OPENSWMM_CONTROLS_HPP
#define OPENSWMM_CONTROLS_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include "../core/StringCase.hpp"
#include "../math/MathExpr.hpp"

namespace openswmm {

struct SimulationContext;
class SWMMEngine;

namespace controls {

// ============================================================================
// Comparison and logic operators
// ============================================================================

enum class CompareOp : int { EQ = 0, NE = 1, LT = 2, LE = 3, GT = 4, GE = 5 };
enum class LogicOp : int { AND = 0, OR = 1 };

// ============================================================================
// Condition variable types (matching ALL legacy RuleAttrib values)
// ============================================================================

enum class ConditionVar : int {
    // Node attributes
    NODE_DEPTH = 0,
    NODE_MAXDEPTH = 1,
    NODE_HEAD = 2,
    NODE_VOLUME = 3,
    NODE_INFLOW = 4,

    // Link attributes
    LINK_FLOW = 10,
    LINK_DEPTH = 11,
    LINK_SETTING = 12,
    LINK_STATUS = 13,
    LINK_FULLFLOW = 14,
    LINK_FULLDEPTH = 15,
    LINK_VELOCITY = 16,
    LINK_LENGTH = 17,
    LINK_SLOPE = 18,
    LINK_TIMEOPEN = 19,
    LINK_TIMECLOSED = 20,

    // Gage attributes
    GAGE_RAIN = 30,      ///< Current rainfall intensity
    GAGE_RAIN_PAST = 31, ///< Past n-hours rainfall (n stored in parameter field)

    // System / time
    SIM_TIME = 40,
    SIM_DATE = 41,
    CLOCK_TIME = 42,
    SIM_DAY = 43,      ///< Day of week (1-7, Sun=1)
    SIM_MONTH = 44,    ///< Month (1-12)
    SIM_DAYOFYEAR = 45 ///< Day of year (1-365)
};

// ============================================================================
// Action attribute (what is being set on the link)
// ============================================================================

enum class ActionType : int {
    NUMERIC = 0,    ///< Direct numeric setting value
    CURVE = 1,      ///< Setting from curve lookup(ControlValue)
    TIMESERIES = 2, ///< Setting from timeseries lookup(currentTime)
    PID = 3         ///< PID controller output
};

// ============================================================================
// Rule premise (condition)
// ============================================================================

struct Premise {
    LogicOp logic = LogicOp::AND;
    bool is_expression = false; ///< True if LHS is a math expression
    int expr_idx = -1;          ///< Index into expressions_ (-1 if N/A)

    ConditionVar lhs_var = ConditionVar::NODE_DEPTH;
    int lhs_idx = -1;  ///< Object index (node/link/gage)
    int lhs_param = 0; ///< Extra parameter (e.g. n-hours for GAGE_RAIN_PAST)

    CompareOp op = CompareOp::EQ;

    bool rhs_is_variable = false;
    ConditionVar rhs_var = ConditionVar::NODE_DEPTH;
    int rhs_idx = -1;
    int rhs_param = 0; ///< Extra parameter for RHS gage depth
    double rhs_value = 0.0;
};

// ============================================================================
// Rule action
// ============================================================================

struct Action {
    int link_idx = -1; ///< Link being controlled
    int rule_idx = -1; ///< Parent rule index (for priority lookup)
    ActionType type = ActionType::NUMERIC;
    double value = 0.0;   ///< Direct value or computed result
    int curve_idx = -1;   ///< Curve index (for CURVE type)
    int tseries_idx = -1; ///< Timeseries index (for TIMESERIES type)
    int pid_idx = -1;     ///< PID state index (for PID type)
};

// ============================================================================
// PID controller state (per action)
// ============================================================================

struct PIDState {
    double kp = 0.0;
    double ki = 0.0; ///< Integral time (minutes, 0=disable)
    double kd = 0.0;
    double setpoint = 0.0;
    double e1 = 0.0;     ///< Previous error
    double e2 = 0.0;     ///< Error two steps back
    int action_idx = -1; ///< Associated action index
};

// ============================================================================
// Named variable (for rule expressions)
// ============================================================================

struct NamedVariable {
    std::string name;
    ConditionVar var;
    int idx = -1;
    int param = 0;
};

// ============================================================================
// Control rule
// ============================================================================

struct Rule {
    std::string name;
    std::vector<Premise> premises;
    std::vector<Action> then_actions;
    std::vector<Action> else_actions;
    double priority = 0.0;
};

// ============================================================================
// Control engine
// ============================================================================

/**
 * @brief Diagnostic for the most recent parseRuleText() rejection.
 *
 * @details `line` is 1-based and indexes the *original* rule text, counting
 *          blank lines, so it can be shown directly against what the caller
 *          submitted. Both fields are only meaningful after parseRuleText()
 *          returns < 0.
 */
struct ParseError {
    int line = -1;       ///< 1-based line in the submitted text; -1 if unknown
    std::string message; ///< Human-readable reason for the rejection
    int code = 0;        ///< Legacy error code for the same rejection
    std::string token;   ///< Offending token legacy names in its message
};

/** @brief Mutable compiled control rules and per-action PID history.
 * Use one owning simulation thread. Rule/context edits, parsing and evaluation
 * must be serialized. No method catches allocation failures or rolls back all
 * preceding mutations on exception. Hydraulic quantities use project display
 * units; serial dates/time comparisons use SWMM days unless stated otherwise. */
class ControlEngine {
public:
    /** @brief Copy compiled rules, reindex actions and rebuild premise caches.
     * @param rules Borrowed rules copied into this engine; object/table indices must
     * match the simulation subsequently evaluated and rule/premise counts must fit int.
     * Clears PID history and the obsolete private link clock. Retains named variables,
     * expressions, diagnostics and last-action count. Copied PID indices do not create
     * PID states; use parsed rules when PID controller state is required. */
    void init(const std::vector<Rule>& rules);

    /** @brief Evaluate premises, arbitrate actions and update link target settings.
     * @param[in,out] ctx Compatible initialized context with aligned object arrays,
     * valid flow-unit enum, tables and current link state. Updates target settings,
     * table cursors, optional action log and interned rule names; routing owns actual
     * settings and link open/closed transition timestamps.
     * @param current_date Finite absolute SWMM date in days at step start plus 1 ms,
     * as supplied by routing. Its floor, fraction and difference from start_date form
     * the date, clock and elapsed-time variables respectively.
     * @param dt Finite positive routing step in seconds; half this duration in days
     * is the equality window, and PID integration converts it to minutes.
     * @param rule_time_reached Caller-computed RULE_STEP gate. False returns zero and
     * resets lastActionCount(), retaining previous clocks, caches and PID state.
     * @return Number of targets whose exact numeric value changed, not evaluated or
     * queued actions. Rebuild premise caches after changing rules. All ordinary and
     * scalar premises are evaluated before ordered Boolean reduction (a failed AND
     * stops that reduction). Actions run last-authored first within a selected branch;
     * first candidate per link wins ties, strictly higher priority replaces it.
     * Modulators update before arbitration, so losing PID actions can advance history.
     * Numeric targets are not clamped here. Exceptions can leave partial state. */
    int evaluate(SimulationContext& ctx, double current_date, double dt,
                 bool rule_time_reached = true);

    // Rule parsing (from [CONTROLS] text)
    /** @brief Append rules, named variables and expressions from control-section text.
     * @param text Borrowed whitespace-tokenized body without a [CONTROLS] header;
     * keywords/named lookups are case insensitive, object names use context tables.
     * Time constants accept hours or colon time; date/calendar/status parsing follows
     * the helper contracts. No quoted-name or inline-comment grammar is added here.
     * @param ctx Borrowed model used to resolve object, curve and time-series indices;
     * this method does not modify its hydraulic state.
     * @return Newly appended rule count, zero for blank input, or -1 with lastParseError().
     * Clears the prior diagnostic first. Completed rules/variables/expressions/PID
     * states appended before rejection remain; failure does not rebuild caches, so
     * clear/reparse before evaluation after a rejected append. Successful nonempty
     * input reindexes all actions and rebuilds caches. Extra tokens and otherwise
     * unhandled lines can be ignored; this is not full structural model validation. */
    int parseRuleText(const std::string& text, SimulationContext& ctx);

    /** @brief Borrow the most recent parser diagnostic without clearing it.
     * @return Member reference valid until destruction; contents change on parse/clear.
     * Line/code/token/message describe rejection only after parseRuleText() returned -1.
     * Copy before subsequent parsing if the diagnostic must be retained. */
    const ParseError& lastParseError() const noexcept { return last_parse_error_; }

    /** @brief Discard rules, PID/named-expression state, caches and parser diagnostic.
     * Use before recompiling a complete rule-text store because parseRuleText appends.
     * Does not reset lastActionCount(), evaluation clocks, tracked modulation values
     * or context target settings/logs. Existing borrowed rule elements are invalidated. */
    void clearRules();

    // Named variables
    /** @brief Append a named binding without resolving or validating its object.
     * @param name Copied variable name; lookup is case insensitive, first duplicate wins.
     * @param var Attribute returned when expressions/premises refer to the binding.
     * @param idx Zero-based matching node/link/gage index; system variables ignore it.
     * @param param Rain-history hours in [1,48] for GAGE_RAIN_PAST; otherwise ignored.
     * Invalid indices are handled by getVariableValue(), not rejected at registration. */
    void addNamedVariable(const std::string& name, ConditionVar var, int idx, int param = 0);

    // Math expressions
    /** @brief Parse and register a named arithmetic expression.
     * @param name Copied case-insensitive lookup key. A duplicate redirects the name to
     * the new expression while existing compiled expression indices remain valid.
     * @param formula Borrowed MathExpr syntax; symbol values are resolved at evaluation.
     * @return New zero-based expression index or -1 for parser rejection, which does not
     * change the registry. Allocation failures propagate and are not transactional. */
    int addExpression(const std::string& name, const std::string& formula);

    /** @brief Borrow the mutable compiled rule sequence.
     * @return Owned vector valid until destruction; structural edits invalidate element
     * references/caches. Caller reindexes action rule_idx and rebuilds premise caches
     * before evaluation, and keeps PID/expression/object indices compatible. */
    std::vector<Rule>& rules() { return rules_; }

    /** @brief Read the target-change count of the last returned evaluate() call.
     * @return Stored count, initially zero; a skipped RULE_STEP call stores zero.
     * init(), clearRules() and parsing do not reset this count. */
    int lastActionCount() const { return last_action_count_; }

    // ========================================================================
    // SoA batch evaluation index (AD-14)
    // ========================================================================

    /**
     * @brief Pre-sorted premise index for batch evaluation.
     *
     * @details Built at init() time. Groups all premises by their LHS
     *          condition variable type into contiguous SoA arrays. At
     *          evaluation time, each variable-type group is processed
     *          in a single vectorisable pass:
     *
     *          1. Batch gather: lhs[i] = ctx.nodes.depth[obj_idx[i]]
     *          2. Batch compare: result[i] = lhs[i] > threshold[i]
     *          3. Scatter results to per-rule boolean array
     *
     *          This eliminates per-premise branching on variable type
     *          and enables SIMD comparison of contiguous arrays.
     */
    struct PremiseSoA {
        ConditionVar var_type; ///< All premises in this group test this variable
        int count = 0;

        // Per-premise SoA (contiguous, aligned for SIMD)
        std::vector<int> rule_idx;     ///< Which rule this premise belongs to
        std::vector<int> premise_idx;  ///< Position within the rule's premise list
        std::vector<int> obj_idx;      ///< Object index (node/link/gage)
        std::vector<int> param;        ///< Requested gage history hours
        std::vector<int> flat_idx;     ///< Position in flat premise_results_ cache
        std::vector<int> op;           ///< CompareOp as int
        std::vector<double> rhs_value; ///< RHS threshold value

        // Per-premise flags
        std::vector<bool> rhs_is_variable; ///< True if RHS is a variable (not batch-able)
        std::vector<bool> is_expression;   ///< True if LHS is an expression

        // Working buffer (reused each evaluate call)
        std::vector<double> lhs_values; ///< Gathered LHS values
        std::vector<bool> results;      ///< Comparison results
    };

    /** @brief Rebuild flattened premise offsets, variable-type groups and work buffers.
     * Call after any premise/rule structural change, with all counts and cumulative
     * offsets representable as int. Expression LHS values are evaluated separately;
     * variable RHS entries retain placeholder slots for the scalar pass. Group order
     * is unspecified. Does not reset PID history, pending actions or diagnostics. */
    void buildPremiseSoA();

    /** @brief Gather and compare one homogeneous ordinary-variable premise group.
     * @param[in,out] group Valid group created by buildPremiseSoA(), with nonnegative
     * count and all parallel/work arrays at least count long, valid flat cache indices.
     * Writes lhs_values/results and this engine's flat cache; variable RHS slots are
     * set false for later scalar evaluation. Missing LHS also yields false.
     * @param ctx Compatible initialized context with aligned arrays and valid units.
     * @param current_time Absolute SWMM date in days used by link-duration variables;
     * system clock values come from this engine's evaluate() clock members.
     * @param half_step Finite nonnegative comparison tolerance in days.
     * No rule reduction, action application or control-value tracking is done here. */
    void batchEvaluateGroup(PremiseSoA& group, const SimulationContext& ctx, double current_time,
                            double half_step);

private:
    friend class openswmm::SWMMEngine;
    int last_action_count_ = 0; ///< Actions taken in last evaluate() call
    std::vector<Rule> rules_;
    std::vector<PIDState> pid_states_;
    std::vector<NamedVariable> named_vars_;
    std::vector<mathexpr::Expression> expressions_;
    /// EXPRESSION name → index; case-insensitive (legacy-parity name match)
    std::unordered_map<std::string, int, CiHash, CiEqual> expr_index_;
    ParseError last_parse_error_; ///< Set on parseRuleText() reject

    // SoA premise groups (one per variable type that has premises)
    std::vector<PremiseSoA> premise_groups_;

    // ------------------------------------------------------------------
    // Two-phase evaluator state (§7 of CONTROL_RULES_LEGACY_PARITY_AUDIT)
    // ------------------------------------------------------------------
    // Phase 1 batch-evaluates premises (group-major) into the flat cache.
    // Phase 2 walks each rule's premises in declaration order and applies
    // legacy AND/OR semantics by reading the flat cache.
    //
    // Layout: premise_results_[ rule_premise_offset_[r] + p ] holds the
    // result of rule r's premise p.  uint8_t (not bool) so Phase 1's
    // compare loops remain SIMD-writable.

    std::vector<uint8_t> premise_results_; ///< Flat per-(rule,premise) cache
    std::vector<int> rule_premise_offset_; ///< Size rules.size()+1; cumulative offsets
    int total_premises_ = 0;               ///< Sum of premises across all rules

    // Per-rule premise result tracking (for combining AND/OR)
    std::vector<bool> rule_results_; ///< [rule_idx] → current result

    // TIMEOPEN/TIMECLOSED tracking: when each link's setting last changed.
    // Stored as absolute date (decimal days) matching legacy Link[j].timeLastSet.
    // Step 6 of the parity remediation will move this to ctx.links.time_last_set.
    std::vector<double> link_time_last_set_;

    // Tracked across a single evaluate() call
    double control_value_ = 0.0;
    double set_point_ = 0.0;
    /// legacy controls.c shared variables for the call: the currentDate
    /// argument, CurrentDate = floor(currentDate), CurrentTime = the
    /// fraction, ElapsedTime = currentDate - StartDateTime (days).
    double eval_date_ = 0.0;
    double cur_date_ = 0.0;
    double cur_time_ = 0.0;
    double elapsed_days_ = 0.0;

    // Pending action list (for priority deduplication)
    struct PendingAction {
        int link_idx;
        double value;
        double priority;
        int rule_idx;
        ActionType type = ActionType::NUMERIC; ///< For report filtering (P1-C08).
    };
    std::vector<PendingAction> pending_actions_;
    /// Legacy ActionList (controls.c) slot order, head first. Its nodes
    /// persist across evaluations and are refilled from the head; a new node
    /// is PREPENDED only when every node is in use. Holds the index into
    /// pending_actions_ of each slot's action, -1 when empty.
    std::vector<int> action_slots_;

    /** @brief Read one premise attribute in control-rule units.
     * @param ctx Compatible initialized context with aligned arrays and a valid [0,5]
     * flow-unit enum. Hydraulic lengths, volumes, flow and velocity use project display
     * units; slope is a fraction, settings dimensionless. NODE_INFLOW is lateral flow.
     * @param var Attribute selector; unknown values return MISSING.
     * @param idx Zero-based relevant object index; system/time selectors ignore it.
     * @param current_time Absolute SWMM days used to subtract link time_last_set;
     * SIM_TIME/DATE/CLOCK and calendar fields read stored evaluate() clocks instead.
     * @param param History hours in [1,48] for GAGE_RAIN_PAST; ignored otherwise.
     * @return Attribute or MISSING for invalid objects/unsupported status, closed/open
     * mismatch or invalid history hours. Unused gages return zero. Link durations are
     * days, elapsed/clock time days; calendar fields are numeric. Missing conduit rows
     * return zero for full-flow/length/slope/velocity. No finite-value filtering occurs. */
    double getVariableValue(const SimulationContext& ctx, ConditionVar var, int idx,
                            double current_time, int param = 0) const;

    /** @brief Evaluate an expression/attribute premise and track modulator inputs.
     * @param ctx Compatible borrowed context; expression symbols use named bindings.
     * @param p Premise with compatible expression/object indices and operator.
     * @param current_time Absolute SWMM date in days forwarded to value getters.
     * @param half_step Finite nonnegative tolerance in days for time EQ/NE.
     * @return False for a MISSING operand, otherwise the comparison. Missing operands
     * retain previous control_value/set_point. Valid operands replace both; link-open/
     * closed duration control_value is then converted from days to hours. An invalid
     * expression index falls back to the premise's ordinary LHS selector. */
    bool evaluatePremise(const SimulationContext& ctx, const Premise& p, double current_time,
                         double half_step);

    /** @brief Apply one exact native floating-point comparison.
     * @param lhs Left operand; no tolerance or finite-value validation.
     * @param op EQ/NE/LT/LE/GT/GE; unknown enum yields false.
     * @param rhs Right operand in the same units as lhs.
     * @return Native comparison result, including ordinary NaN behavior. */
    bool compareValues(double lhs, CompareOp op, double rhs) const;
    /** @brief Compare day-valued times with the legacy half-step equality window.
     * @param lhs Left time in days.
     * @param op Comparison selector; only EQ/NE use the window.
     * @param rhs Reference time in days.
     * @param half_step Finite nonnegative day tolerance.
     * @return EQ accepts [rhs-half_step,rhs+half_step); NE is its complement.
     * Other operators use compareValues(). A zero-width EQ window is empty. */
    bool compareTimes(double lhs, CompareOp op, double rhs, double half_step) const;

    /** @brief Advance recursive PID history and compute a bounded link setting.
     * @param[in,out] pid Controller gains, setpoint and previous relative errors; ki/kd
     * are minute-based integral/derivative times, ki zero disables integral action.
     * @param control_value Current observed value in the setpoint's units.
     * @param current_setting Existing dimensionless target to which the update is added.
     * @param is_pump True permits settings above one; false caps at one.
     * @param dt Routing duration in seconds, converted through days to minutes.
     * @return Existing setting unchanged for dt<=0, otherwise computed setting clamped
     * to a minimum of zero (and maximum one for nonpumps). Updates e1/e2, with the
     * legacy near-steady error reset. Caller supplies finite meaningful gains/values;
     * NaN/overflow is not rejected or repaired. */
    double computePIDSetting(PIDState& pid, double control_value, double current_setting,
                             bool is_pump, double dt);

    /** @brief Compute a selected action's numeric candidate before priority arbitration.
     * @param[in,out] a Action whose value is replaced by a valid curve/time-series/PID
     * lookup; direct numeric actions and invalid modulation indices retain the value.
     * @param[in,out] ctx Compatible tables/link state; lookup can update table cursors.
     * @param current_time Unused; time-series lookup uses stored eval_date_ instead.
     * @param dt Routing seconds forwarded to PID integration.
     * Curve/PID input comes from tracked control_value_/set_point_. PID uses current
     * target_setting (zero for an invalid link) and advances owned controller history. */
    void updateActionValue(Action& a, SimulationContext& ctx, double current_time, double dt);

    /** @brief Resolve candidates per link and write winning target settings.
     * @param[in,out] ctx Compatible initialized links; updates target_setting and optional
     * numeric-action log/rule-name intern table. Does not change actual setting or
     * open/closed timestamps. Invalid link candidates are skipped; values are unclamped.
     * @param current_time Unused; logs carry stored eval_date_.
     * @return Number of exact target-value changes. First candidate wins equal priority;
     * only strictly greater priority replaces it. Persistent head-first slot order
     * controls execution/log order. Allocation/log exceptions can follow earlier writes. */
    int applyPendingActions(SimulationContext& ctx, double current_time);

    /** @brief Resolve the first case-insensitive named binding and read its value.
     * @param name Borrowed exact variable name; no prefix match.
     * @param ctx Compatible initialized context for the referenced attribute.
     * @param current_time Absolute SWMM date in days forwarded to getVariableValue().
     * @return Resolved value (possibly MISSING), or zero for an unknown name.
     * Does not consult the expression-name registry or validate names on registration. */
    double getNamedVariableValue(const std::string& name, const SimulationContext& ctx,
                                 double current_time) const;
};

} // namespace controls
} // namespace openswmm

#endif // OPENSWMM_CONTROLS_HPP

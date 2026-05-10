/**
 * @file Controls.hpp
 * @brief Rule-based control engine — full legacy parity.
 *
 * @details Complete reimplementation matching legacy controls.c capabilities:
 *   - All condition variable types (node/link/gage/system attributes)
 *   - All action types (direct, curve-based, timeseries-based, PID)
 *   - Named variables and math expression evaluation
 *   - Time-aware comparison with half-step tolerance
 *   - Priority-based action deduplication
 *   - Short-circuit AND evaluation
 *
 * @note Legacy reference: src/legacy/engine/controls.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_CONTROLS_HPP
#define OPENSWMM_CONTROLS_HPP

#include <vector>
#include <string>
#include <unordered_map>
#include "../math/MathExpr.hpp"

namespace openswmm {

struct SimulationContext;

namespace controls {

// ============================================================================
// Comparison and logic operators
// ============================================================================

enum class CompareOp : int { EQ = 0, NE = 1, LT = 2, LE = 3, GT = 4, GE = 5 };
enum class LogicOp   : int { AND = 0, OR = 1 };

// ============================================================================
// Condition variable types (matching ALL legacy RuleAttrib values)
// ============================================================================

enum class ConditionVar : int {
    // Node attributes
    NODE_DEPTH     = 0,
    NODE_MAXDEPTH  = 1,
    NODE_HEAD      = 2,
    NODE_VOLUME    = 3,
    NODE_INFLOW    = 4,

    // Link attributes
    LINK_FLOW      = 10,
    LINK_DEPTH     = 11,
    LINK_SETTING   = 12,
    LINK_STATUS    = 13,
    LINK_FULLFLOW  = 14,
    LINK_FULLDEPTH = 15,
    LINK_VELOCITY  = 16,
    LINK_LENGTH    = 17,
    LINK_SLOPE     = 18,
    LINK_TIMEOPEN  = 19,
    LINK_TIMECLOSED= 20,

    // Gage attributes
    GAGE_RAIN      = 30,      ///< Current rainfall intensity
    GAGE_RAIN_PAST = 31,      ///< Past n-hours rainfall (n stored in idx field)

    // System / time
    SIM_TIME       = 40,
    SIM_DATE       = 41,
    CLOCK_TIME     = 42,
    SIM_DAY        = 43,      ///< Day of week (1-7, Sun=1)
    SIM_MONTH      = 44,      ///< Month (1-12)
    SIM_DAYOFYEAR  = 45       ///< Day of year (1-365)
};

// ============================================================================
// Action attribute (what is being set on the link)
// ============================================================================

enum class ActionType : int {
    NUMERIC    = 0,    ///< Direct numeric setting value
    CURVE      = 1,    ///< Setting from curve lookup(ControlValue)
    TIMESERIES = 2,    ///< Setting from timeseries lookup(currentTime)
    PID        = 3     ///< PID controller output
};

// ============================================================================
// Rule premise (condition)
// ============================================================================

struct Premise {
    LogicOp      logic         = LogicOp::AND;
    bool         is_expression = false;      ///< True if LHS is a math expression
    int          expr_idx      = -1;         ///< Index into expressions_ (-1 if N/A)

    ConditionVar lhs_var       = ConditionVar::NODE_DEPTH;
    int          lhs_idx       = -1;         ///< Object index (node/link/gage)
    int          lhs_param     = 0;          ///< Extra parameter (e.g. n-hours for GAGE_RAIN_PAST)

    CompareOp    op            = CompareOp::EQ;

    bool         rhs_is_variable = false;
    ConditionVar rhs_var       = ConditionVar::NODE_DEPTH;
    int          rhs_idx       = -1;
    double       rhs_value     = 0.0;
};

// ============================================================================
// Rule action
// ============================================================================

struct Action {
    int        link_idx      = -1;       ///< Link being controlled
    int        rule_idx      = -1;       ///< Parent rule index (for priority lookup)
    ActionType type          = ActionType::NUMERIC;
    double     value         = 0.0;      ///< Direct value or computed result
    int        curve_idx     = -1;       ///< Curve index (for CURVE type)
    int        tseries_idx   = -1;       ///< Timeseries index (for TIMESERIES type)
    int        pid_idx       = -1;       ///< PID state index (for PID type)
};

// ============================================================================
// PID controller state (per action)
// ============================================================================

struct PIDState {
    double kp        = 0.0;
    double ki        = 0.0;   ///< Integral time (minutes, 0=disable)
    double kd        = 0.0;
    double setpoint  = 0.0;
    double e1        = 0.0;   ///< Previous error
    double e2        = 0.0;   ///< Error two steps back
    int    action_idx = -1;   ///< Associated action index
};

// ============================================================================
// Named variable (for rule expressions)
// ============================================================================

struct NamedVariable {
    std::string  name;
    ConditionVar var;
    int          idx = -1;
};

// ============================================================================
// Control rule
// ============================================================================

struct Rule {
    std::string          name;
    std::vector<Premise> premises;
    std::vector<Action>  then_actions;
    std::vector<Action>  else_actions;
    double               priority = 0.0;
};

// ============================================================================
// Control engine
// ============================================================================

class ControlEngine {
public:
    void init(const std::vector<Rule>& rules);

    /**
     * @brief Evaluate all control rules and set link target settings.
     *
     * @details Implements:
     *   - Short-circuit AND evaluation
     *   - Time-aware EQ/NE comparison with half-step tolerance
     *   - Priority-based action deduplication per link
     *   - Curve/timeseries/PID modulated control
     *   - ControlValue tracking for modulated actions
     *
     * @param ctx           Simulation context.
     * @param current_time  Current simulation time (seconds).
     * @param dt            Routing timestep (seconds).
     * @returns Number of setting changes made.
     */
    int evaluate(SimulationContext& ctx, double current_time, double dt);

    // Rule parsing (from [CONTROLS] text)
    int parseRuleText(const std::string& text, SimulationContext& ctx);

    // Named variables
    void addNamedVariable(const std::string& name, ConditionVar var, int idx);

    // Math expressions
    int addExpression(const std::string& name, const std::string& formula);

    std::vector<Rule>& rules() { return rules_; }

    /// Number of actions taken in the last evaluate() call.
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
        ConditionVar var_type;       ///< All premises in this group test this variable
        int count = 0;

        // Per-premise SoA (contiguous, aligned for SIMD)
        std::vector<int>    rule_idx;     ///< Which rule this premise belongs to
        std::vector<int>    premise_idx;  ///< Position within the rule's premise list
        std::vector<int>    obj_idx;      ///< Object index (node/link/gage)
        std::vector<int>    op;           ///< CompareOp as int
        std::vector<double> rhs_value;    ///< RHS threshold value

        // Per-premise flags
        std::vector<bool>   rhs_is_variable; ///< True if RHS is a variable (not batch-able)
        std::vector<bool>   is_expression;   ///< True if LHS is an expression

        // Working buffer (reused each evaluate call)
        std::vector<double> lhs_values;  ///< Gathered LHS values
        std::vector<bool>   results;     ///< Comparison results
    };

    /// Build SoA index from rules. Called once at init().
    void buildPremiseSoA();

    /// Batch evaluate all premises of one variable type.
    void batchEvaluateGroup(PremiseSoA& group,
                            const SimulationContext& ctx,
                            double current_time, double half_step);

private:
    int last_action_count_ = 0;     ///< Actions taken in last evaluate() call
    std::vector<Rule>        rules_;
    std::vector<PIDState>    pid_states_;
    std::vector<NamedVariable> named_vars_;
    std::vector<mathexpr::Expression> expressions_;
    std::unordered_map<std::string, int> expr_index_;

    // SoA premise groups (one per variable type that has premises)
    std::vector<PremiseSoA>  premise_groups_;

    // Per-rule premise result tracking (for combining AND/OR)
    std::vector<bool>        rule_results_;   ///< [rule_idx] → current result

    // TIMEOPEN/TIMECLOSED tracking: when each link's setting last changed.
    // Stored as absolute date (decimal days) matching legacy Link[j].timeLastSet.
    std::vector<double> link_time_last_set_;

    // Tracked across a single evaluate() call
    double control_value_ = 0.0;
    double set_point_     = 0.0;

    // Pending action list (for priority deduplication)
    struct PendingAction {
        int    link_idx;
        double value;
        double priority;
        int    rule_idx;
    };
    std::vector<PendingAction> pending_actions_;

    double getVariableValue(const SimulationContext& ctx,
                            ConditionVar var, int idx,
                            double current_time, int param = 0) const;

    bool evaluatePremise(const SimulationContext& ctx,
                         const Premise& p, double current_time, double half_step);

    bool compareValues(double lhs, CompareOp op, double rhs) const;
    bool compareTimes(double lhs, CompareOp op, double rhs, double half_step) const;

    double computePIDSetting(PIDState& pid, double control_value,
                             double current_setting, bool is_pump, double dt);

    void updateActionValue(Action& a, SimulationContext& ctx,
                           double current_time, double dt);

    /// Apply pending actions (highest priority per link wins).
    int applyPendingActions(SimulationContext& ctx, double current_time);

    /// Named variable value lookup (for math expressions).
    double getNamedVariableValue(const std::string& name,
                                 const SimulationContext& ctx,
                                 double current_time) const;
};

} // namespace controls
} // namespace openswmm

#endif // OPENSWMM_CONTROLS_HPP

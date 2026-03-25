/**
 * @file PluginState.hpp
 * @brief Plugin lifecycle state enumerator.
 *
 * @details Defines the states that an output or report plugin can occupy.
 *          The PluginFactory enforces valid state transitions and calls the
 *          appropriate lifecycle methods in order.
 *
 * ### State machine diagram
 *
 * @code
 *  UNLOADED ──── (dlopen) ─────► LOADED
 *                                   │
 *                            initialize()
 *                                   │
 *                                   ▼
 *                             INITIALIZED
 *                                   │
 *                             validate()
 *                                   │
 *                                   ▼
 *                              VALIDATED
 *                                   │
 *                              prepare()
 *                                   │
 *                                   ▼
 *                               PREPARED ◄─────────────┐
 *                                   │                   │
 *                              update() ────────► UPDATING
 *                                   │                   │
 *                              finalize()         (return)
 *                                   │
 *                                   ▼
 *                              FINALIZED
 *                                   │
 *                             (dlclose)
 *                                   │
 *                                   ▼
 *                               CLOSED
 *
 *          Any state ──── (error) ──────► ERROR
 * @endcode
 *
 * @defgroup engine_plugins Plugin System
 * @ingroup  new_engine
 *
 * @see IOutputPlugin.hpp
 * @see IReportPlugin.hpp
 * @see PluginFactory.hpp (in src/engine/plugins/)
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_PLUGIN_STATE_HPP
#define OPENSWMM_PLUGIN_STATE_HPP

#include <cstdint>

namespace openswmm {

/**
 * @brief Plugin lifecycle states.
 * @ingroup engine_plugins
 */
enum class PluginState : std::int32_t {
    UNLOADED    =  0, ///< Library not yet loaded (or was closed).
    LOADED      =  1, ///< Shared library opened via dlopen/LoadLibrary.
    INITIALIZED =  2, ///< initialize() returned success.
    VALIDATED   =  3, ///< validate() returned success.
    PREPARED    =  4, ///< prepare() returned success; ready to accept update() calls.
    UPDATING    =  5, ///< Currently executing an update() call (transient; not normally observed).
    FINALIZED   =  6, ///< finalize() returned success; no more update() calls.
    CLOSED      =  7, ///< Library closed and handle released.
    ERROR       = -1  ///< Plugin is in an error state; not usable.
};

/**
 * @brief Convert a PluginState to a human-readable string.
 * @param state  Plugin state.
 * @returns Static null-terminated string (never NULL).
 */
inline const char* plugin_state_to_string(PluginState state) noexcept {
    switch (state) {
        case PluginState::UNLOADED:    return "UNLOADED";
        case PluginState::LOADED:      return "LOADED";
        case PluginState::INITIALIZED: return "INITIALIZED";
        case PluginState::VALIDATED:   return "VALIDATED";
        case PluginState::PREPARED:    return "PREPARED";
        case PluginState::UPDATING:    return "UPDATING";
        case PluginState::FINALIZED:   return "FINALIZED";
        case PluginState::CLOSED:      return "CLOSED";
        case PluginState::ERROR:       return "ERROR";
        default:                       return "UNKNOWN";
    }
}

/**
 * @brief Check whether a state transition is valid.
 * @param from  Current state.
 * @param to    Target state.
 * @returns true if the transition is allowed.
 */
inline bool plugin_state_transition_valid(PluginState from, PluginState to) noexcept {
    using PS = PluginState;
    if (to == PS::ERROR) return true;   // any state can transition to ERROR
    switch (from) {
        case PS::UNLOADED:    return to == PS::LOADED;
        case PS::LOADED:      return to == PS::INITIALIZED || to == PS::CLOSED;
        case PS::INITIALIZED: return to == PS::VALIDATED   || to == PS::CLOSED;
        case PS::VALIDATED:   return to == PS::PREPARED    || to == PS::CLOSED;
        case PS::PREPARED:    return to == PS::UPDATING    || to == PS::FINALIZED;
        case PS::UPDATING:    return to == PS::PREPARED;
        case PS::FINALIZED:   return to == PS::CLOSED;
        case PS::CLOSED:      return false;
        case PS::ERROR:       return to == PS::CLOSED;
        default:              return false;
    }
}

} /* namespace openswmm */

#endif /* OPENSWMM_PLUGIN_STATE_HPP */

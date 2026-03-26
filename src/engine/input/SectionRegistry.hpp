/**
 * @file SectionRegistry.hpp
 * @brief Registry that maps SWMM section tags to handler functions.
 *
 * @details The SectionRegistry enables optional sections (R09): user code
 *          can register custom handler functions for new section tags.
 *          Built-in sections (NODES, LINKS, etc.) are registered at startup.
 *
 * Handler signature:
 * @code{.cpp}
 * using SectionHandler = std::function<void(
 *     SimulationContext&,
 *     const std::vector<std::string>& lines
 * )>;
 * @endcode
 *
 * @see Legacy reference: src/solver/input.c — findSection(), which uses a
 *      static keyword array and switch statement. The new registry replaces
 *      that with a hash map of function pointers.
 *
 * @see tests/unit/test_section_registry.cpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_SECTION_REGISTRY_HPP
#define OPENSWMM_ENGINE_SECTION_REGISTRY_HPP

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <unordered_map>

namespace openswmm {
    struct SimulationContext;
}

namespace openswmm::input {

/**
 * @brief Handler function type for input file sections.
 *
 * @details Called once per section with all non-comment, non-empty lines
 *          in that section. The handler is responsible for parsing each line
 *          and populating the SimulationContext.
 *
 * @param ctx    Simulation context to populate.
 * @param lines  All non-blank, non-comment lines in the section (after
 *               comment stripping but before tokenization).
 */
using SectionHandler = std::function<void(
    SimulationContext&,
    const std::vector<std::string>& /*lines*/
)>;

/**
 * @brief Registry that maps section tags (e.g., "[JUNCTIONS]") to handlers.
 *
 * @details Two types of sections:
 * - **Built-in sections:** Registered by InputReader at startup for all
 *   standard SWMM 5.x sections.
 * - **Custom sections:** Registered by user code via register_custom() for
 *   new or extension sections. Custom sections can override built-ins.
 *
 * @ingroup engine_input
 */
class SectionRegistry {
public:
    SectionRegistry() = default;
    ~SectionRegistry() = default;

    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------

    /**
     * @brief Register a built-in section handler.
     *
     * @param tag      Section tag without brackets (e.g., "JUNCTIONS").
     *                 Case is normalized to uppercase before storage.
     * @param handler  Handler function.
     */
    void register_builtin(std::string_view tag, SectionHandler handler);

    /**
     * @brief Register a custom (user-defined) section handler. (R09)
     *
     * @details Custom handlers override built-in handlers with the same tag.
     *          Use this to:
     *          - Add entirely new section types to the input file.
     *          - Override built-in parsing with custom logic.
     *
     * @param tag      Section tag without brackets (e.g., "MY_SECTION").
     * @param handler  Custom handler function.
     *
     * Example:
     * @code{.cpp}
     * registry.register_custom("MY_CUSTOM", [](SimulationContext& ctx,
     *     const std::vector<std::string>& lines) {
     *     for (const auto& line : lines) {
     *         // parse custom section content
     *     }
     * });
     * @endcode
     */
    void register_custom(std::string_view tag, SectionHandler handler);

    // -----------------------------------------------------------------------
    // Query
    // -----------------------------------------------------------------------

    /**
     * @brief Check if a section tag has a registered handler.
     * @param tag  Section tag (without brackets, any case).
     * @returns    true if a handler is registered.
     */
    bool has(std::string_view tag) const;

    /**
     * @brief Check if a section tag is a custom (non-builtin) handler.
     * @param tag  Section tag (without brackets).
     * @returns    true if registered via register_custom().
     */
    bool is_custom(std::string_view tag) const;

    // -----------------------------------------------------------------------
    // Dispatch
    // -----------------------------------------------------------------------

    /**
     * @brief Dispatch a section to its handler.
     *
     * @details If no handler is registered for `tag`, the section is silently
     *          skipped (and the caller may issue a warning).
     *
     * @param tag    Section tag (without brackets, any case).
     * @param ctx    Simulation context.
     * @param lines  Lines in the section.
     */
    void dispatch(
        std::string_view                  tag,
        SimulationContext&                ctx,
        const std::vector<std::string>&   lines
    ) const;

    // -----------------------------------------------------------------------
    // Enumeration
    // -----------------------------------------------------------------------

    /**
     * @brief List all registered section tags (built-in + custom).
     */
    std::vector<std::string> registered_tags() const;

private:
    std::unordered_map<std::string, SectionHandler> builtin_;
    std::unordered_map<std::string, SectionHandler> custom_;

    std::string normalize_tag(std::string_view tag) const;
};

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_SECTION_REGISTRY_HPP */

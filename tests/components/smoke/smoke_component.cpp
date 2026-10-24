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
 * @file      smoke_component.cpp
 * @brief     D2 (program plan §C.1) — the test double for component discovery.
 *
 * A REAL shared library, loaded through dlopen exactly as a third-party
 * component would be. Not a mock: it implements every pure virtual of an
 * ABI-4 `HydroCouple::IModelComponentInfo` (22 of them) against the same
 * headers the engine compiles against.
 *
 * One source, three builds, selected by compile definitions:
 *
 *   SMOKE_MODE 0  stamped — `HYDROCOUPLE_DECLARE_COMPONENT`, the convention
 *   SMOKE_MODE 1  MIS-stamped — identical code, but its stamp claims iface=2.
 *                 That is the exact lie the pre-D0 header told for every
 *                 ABI-3/4 component; the engine must refuse it and say so.
 *   SMOKE_MODE 2  legacy — only `CreateComponentInfo`, the unstamped Python
 *                 loader convention; the engine must load it and report it
 *                 as unstamped, never as verified.
 *
 * `SMOKE_COMPONENT_ID` differs per build so that one discovery pass can see
 * every variant without the duplicate-id rule masking any of them.
 *
 * `createComponentInstance()` returns an empty pointer in this round, and it
 * is the one thing here that is not a real implementation. D2 gates
 * DISCOVERY — find, stamp-check, index — and never instantiates. A real
 * instance needs the plan's SDK-free `ComponentBase` (src/couplers, phase
 * A1/F1), which does not exist yet; hand-writing an ABI-4 IModelComponent
 * lifecycle here would be a second copy of that base, the pattern this
 * sequence keeps having to undo.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <hydrocouple.h>
#include <hydrocouplecomponentabi.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef SMOKE_COMPONENT_ID
#error "SMOKE_COMPONENT_ID must be defined by the build"
#endif
#ifndef SMOKE_MODE
#error "SMOKE_MODE must be defined by the build (0 stamped, 1 mis-stamped, 2 legacy)"
#endif

namespace {

class SmokeInfo final : public HydroCouple::IModelComponentInfo {
public:
    // ── ISignal<std::string> (IPropertyChanged) ───────────────────────────
    /**
     * @brief Satisfy the discovery test signal interface without registering a slot.
     *
     * Intentional no-op: discovery gates exercise interface compatibility and metadata, not signal
     * delivery.
     *
     * @param[in] slot Borrowed shared-pointer reference; ignored, not retained and not invoked.
     */
    void connect(const std::shared_ptr<HydroCouple::ISlot<std::string>>& slot) override {}
    /**
     * @brief Satisfy the discovery signal interface without maintaining connections.
     *
     * Intentional no-op paired with the no-op connect; does not alter slot ownership.
     *
     * @param[in] slot Borrowed shared-pointer reference, ignored.
     */
    void disconnect(const std::shared_ptr<HydroCouple::ISlot<std::string>>& slot) override {}
    /**
     * @brief Accept a signal-block request without storing it in this discovery double.
     *
     * @param[in] blocked Ignored requested signal-block state; neither value changes behavior.
     */
    void blockSignals(bool blocked) override {}
    /**
     * @brief Discard a signal payload in the discovery-only component double.
     *
     * @param[in] value Owned payload value for this call; destroyed without delivery or retention.
     */
    void emit(std::string value) override {}

    // ── IDescription / IIdentity ──────────────────────────────────────────
    /**
     * @brief Borrow the component display caption.
     * @return Reference to caption_, valid while this SmokeInfo lives; setCaption can replace its
     *     contents and invalidate borrowed character pointers.
     */
    const std::string& caption() const override { return caption_; }
    /**
     * @brief Replace the discovery component caption without emitting a signal.
     *
     * @param[in] c Borrowed new caption copied into owned member storage; allocation failure
     *     propagates.
     */
    void setCaption(const std::string& c) override { caption_ = c; }
    /**
     * @brief Borrow the component discovery description.
     * @return Member string reference valid while this object lives; contents can be replaced by
     *     setDescription.
     */
    const std::string& description() const override { return description_; }
    /**
     * @brief Replace the discovery description without emitting a signal.
     *
     * @param[in] d Borrowed text copied into the component owned string; allocation failure propagates.
     */
    void setDescription(const std::string& d) override { description_ = d; }
    /**
     * @brief Borrow the build-selected unique smoke component ID.
     * @return Member string initialized from SMOKE_COMPONENT_ID; stable while the object lives, with no
     *     public ID setter.
     */
    const std::string& id() const override { return id_; }

    // ── IComponentInfo ────────────────────────────────────────────────────
    /**
     * @brief Copy the path assigned by the component discovery loader.
     * @return Owned path string, initially empty; copied without filesystem validation.
     */
    std::string libraryFilePath() const override { return library_path_; }
    /**
     * @brief Store a copy of the loader-supplied library path.
     *
     * @param[in] p Borrowed path string, retained as an owned copy without checking existence.
     */
    void setLibraryFilePath(const std::string& p) override { library_path_ = p; }
    /**
     * @brief Report that this test component has no icon.
     * @return Empty owned string.
     */
    std::string iconFilePath() const override { return {}; }
    /**
     * @brief Identify the smoke-library developer for discovery metadata.
     * @return Owned string openswmm test suite.
     */
    std::string developer() const override { return "openswmm test suite"; }
    /**
     * @brief Report that the smoke library publishes no documentation links.
     * @return Empty owned vector.
     */
    std::vector<std::string> documentation() const override { return {}; }
    /**
     * @brief Report the smoke component license identifier.
     * @return Owned string Apache-2.0.
     */
    std::string license() const override { return "Apache-2.0"; }
    /**
     * @brief Copy the smoke component copyright attribution.
     * @return Owned string Copyright 2026 Caleb Buahin.
     */
    std::string copyright() const override { return "Copyright 2026 Caleb Buahin"; }
    /**
     * @brief Report that the smoke component has no website metadata.
     * @return Empty owned string.
     */
    std::string url() const override { return {}; }
    /**
     * @brief Report that the smoke component has no contact email metadata.
     * @return Empty owned string.
     */
    std::string email() const override { return {}; }
    /**
     * @brief Copy the fixed smoke component version metadata.
     * @return Owned string 0.0.1.
     */
    std::string version() const override { return "0.0.1"; }
    /**
     * @brief Copy the fixed discovery tags for the smoke component.
     * @return Owned set containing smoke and test.
     */
    std::set<std::string> tags() const override { return {"test", "smoke"}; }

    // ── IModelComponentInfo ───────────────────────────────────────────────
    /**
     * @brief Deliberately decline runtime instance creation for this discovery-only double.
     *
     * The library is a real metadata/interface test, but its instance factory is intentionally
     * incomplete. Discovery tests must not interpret loading this info object as runnable component
     * coverage.
     * @return Empty unique_ptr; no component allocation or lifecycle execution occurs.
     */
    std::unique_ptr<HydroCouple::IModelComponent> createComponentInstance() override {
        return {}; // see the file comment: D2 never instantiates
    }
    /**
     * @brief Report no adapted-output factories for this discovery component.
     * @return Empty owned vector; no factory pointers or ownership transfers.
     */
    std::vector<HydroCouple::IAdaptedOutputFactory*> adaptedOutputFactories() const override {
        return {};
    }

private:
    std::string id_ = SMOKE_COMPONENT_ID;
    std::string caption_ = "Smoke (" SMOKE_COMPONENT_ID ")";
    std::string description_ = "D2 discovery test double";
    std::string library_path_;
};

} // namespace

#if SMOKE_MODE == 0

HYDROCOUPLE_DECLARE_COMPONENT(SmokeInfo)

#elif SMOKE_MODE == 1

// Everything the real stamp says, except the interface ABI — the precise
// failure mode of the Composer-era header. Assembled from the same pieces so
// the ONLY difference from the engine's stamp is `iface`.
#define SMOKE_FORGED_STAMP                                                                         \
    "hc-abi/" HYDROCOUPLE_COMPONENT_ABI_REVISION ";iface=2"                                        \
    ";cxx=" HYDROCOUPLE_ABI_COMPILER ";stdlib=" HYDROCOUPLE_ABI_STDLIB                             \
    ";bits=" HYDROCOUPLE_ABI_STRINGIFY(__SIZEOF_POINTER__)

/**
 * @brief Expose the intentionally forged ABI stamp in smoke mode one.
 *
 * Caller must not free or modify the literal. This export exists to test that discovery rejects an
 * interface mismatch before loading metadata; compiled only with SMOKE_MODE == 1.
 * @return Borrowed static SMOKE_FORGED_STAMP string, claiming interface ABI 2 while other stamp
 *     fields come from the actual toolchain.
 */
extern "C" HYDROCOUPLE_COMPONENT_EXPORT const char* hydrocouple_component_abi_v1(void) {
    return SMOKE_FORGED_STAMP;
}
// Present, so that a loader which skipped the stamp comparison WOULD load
// this library — which is what makes the refusal falsifiable.
/**
 * @brief Expose metadata behind the deliberately mis-stamped mode-one library.
 *
 * Present so a loader skipping stamp validation could obtain metadata, making rejection meaningful.
 * Static construction may allocate on first access; subsequent calls return the same object.
 * @return Borrowed pointer to a function-static SmokeInfo; caller must not delete it and must
 *     unload the library only after discarding the pointer.
 */
extern "C" HYDROCOUPLE_COMPONENT_EXPORT HydroCouple::IComponentInfo*
hydrocouple_component_info_v1(void) {
    static SmokeInfo s_info;
    return &s_info;
}

#elif SMOKE_MODE == 2

/**
 * @brief Expose the unstamped legacy metadata factory in smoke mode two.
 *
 * Compiled only with SMOKE_MODE == 2. It supplies no verified ABI-stamp export; discovery should
 * mark this library unstamped. Repeated calls share the same metadata object.
 * @return Borrowed function-static SmokeInfo pointer, owned by this library and valid until library
 *     teardown; caller must not delete it.
 */
extern "C" HYDROCOUPLE_COMPONENT_EXPORT HydroCouple::IModelComponentInfo*
CreateComponentInfo(void) {
    static SmokeInfo s_info;
    return &s_info;
}

#else
#error "unknown SMOKE_MODE"
#endif

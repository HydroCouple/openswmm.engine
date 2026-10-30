// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      ComponentBase.hpp
 * @brief     Common lifecycle support for OpenSWMM component wrappers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <hydrocouple.h>
#include <hydrocouplehelpers.h>
#include <hydrocoupletemporal.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace openswmm::couplers {
namespace HC = HydroCouple;
template <class T> class Signal : public virtual HC::ISignal<T> {
public:
    /**
     * @brief Retain one nonnull slot connection unless the same shared pointer is already
     * present.
     * @param[in] slot Shared slot owner copied into list, extending slot lifetime until
     * disconnect/destruction.
     * @note No callback invoked. Vector allocation may throw without append; no locking,
     * exclusive connection access.
     */
    void connect(const std::shared_ptr<HC::ISlot<T>>& slot) override {
        if (slot && std::find(slots_.begin(), slots_.end(), slot) == slots_.end())
            slots_.push_back(slot);
    }
    /**
     * @brief Remove all matching slot pointers and release their retained shared ownership.
     * @param[in] slot Shared pointer identity to remove, null harmless.
     * @note Snapshot emit already in progress retains its own copy and can still invoke the
     * disconnected slot. No notification/locking; caller serializes owner access.
     */
    void disconnect(const std::shared_ptr<HC::ISlot<T>>& slot) override {
        std::erase(slots_, slot);
    }
    /**
     * @brief Enable/disable synchronous slot dispatch without changing connections.
     * @param[in] value True suppresses future emit dispatch, false permits it.
     * @note No replay of blocked events or allocation; caller serializes flag/connection
     * changes.
     */
    void blockSignals(bool value) override { blocked_ = value; }
    /**
     * @brief Invoke a snapshot of connected slots synchronously unless blocked.
     * @param[in] value Event payload passed to each slot with this signal as sender. T controls
     * copying; reference payload types borrow caller-owned event for this synchronous call only.
     * @note Retains snapshot shared slots, so callbacks may connect/disconnect without
     * invalidating iteration. Exceptions from individual slot invocation swallowed, later slots
     * continue. Snapshot allocation/copy exceptions propagate before dispatch. No cross-thread
     * synchronization; callback must not destroy sender.
     */
    void emit(T value) override {
        if (!blocked_) {
            const auto slots = slots_;
            for (const auto& slot : slots) {
                try {
                    (*slot)(*this, value);
                } catch (...) {
                }
            }
        }
    }

private:
    bool blocked_ = false;
    std::vector<std::shared_ptr<HC::ISlot<T>>> slots_;
};
class Identity : public virtual HC::IIdentity, public Signal<std::string> {
public:
    /**
     * @brief Construct identity metadata with caption fallback to ID.
     * @param[in] id Owned ID text, moved into owner; no uniqueness/nonempty validation.
     * @param[in] caption Optional owned caption; empty selects stored ID.
     * @note Description initially empty. String allocation can throw, no signal emitted.
     */
    explicit Identity(std::string id, std::string caption = {})
        : id_(std::move(id)), caption_(caption.empty() ? id_ : std::move(caption)) {}
    /**
     * @brief Read stored id metadata.
     * @return Borrowed const ID string reference, owner lifetime; setInstanceId can replace
     * content.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    const std::string& id() const override { return id_; }
    /**
     * @brief Read stored caption metadata.
     * @return Borrowed const caption reference, owner lifetime; setter replaces content.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    const std::string& caption() const override { return caption_; }
    /**
     * @brief Read stored description metadata.
     * @return Borrowed const description reference, owner lifetime; setter replaces content.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    const std::string& description() const override { return description_; }
    /**
     * @brief Replace caption and synchronously emit the caption property name.
     * @param[in] v Text copied; may be empty, no validation.
     * @note Assignment/snapshot can throw after metadata change; slot exceptions contained.
     * Exclusive owner mutation.
     */
    void setCaption(const std::string& v) override {
        caption_ = v;
        Signal<std::string>::emit("caption");
    }
    /**
     * @brief Replace description and synchronously emit the description property name.
     * @param[in] v Text copied into owner, no validation.
     * @note Signal runs after assignment; allocation may leave changed metadata. Exclusive owner
     * mutation.
     */
    void setDescription(const std::string& v) override {
        description_ = v;
        Signal<std::string>::emit("description");
    }
    /**
     * @brief Replace the raw stored identity without emitting a property event.
     * @param[in] v New ID copied verbatim; no empty/lifecycle/once-only check here.
     * @note Host-facing InstanceIdentity argument enforces policy; direct caller must preserve
     * uniqueness/ stability contract. String assignment can throw; exclusive owner access.
     */
    void setInstanceId(const std::string& v) { id_ = v; }

private:
    std::string id_, caption_, description_;
};
class Dimension final : public Identity, public HC::IDimension {
public:
    /**
     * @brief Construct the scalar item's owned dimension metadata.
     * @param[in] id Owned dimension ID moved to Identity, caption falls back ID.
     * @note Static length/Other role, no length count stored; construction may allocate.
     */
    explicit Dimension(std::string id) : Identity(std::move(id)) {}
    /**
     * @brief Read stored lengthType metadata.
     * @return LengthType::Static, with item scalar extent provided separately.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    LengthType lengthType() const override { return LengthType::Static; }
    /**
     * @brief Read stored role metadata.
     * @return DimensionRole::Other, no temporal/spatial role inferred.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    DimensionRole role() const override { return DimensionRole::Other; }
};
class UnitDimensions final : public Identity, public HC::IUnitDimensions {
public:
    /**
     * @brief Store length/mass/time exponents for unit conversion metadata.
     * @param[in] l Length power, normally integral but double stored without validation.
     * @param[in] m Mass power.
     * @param[in] t Time power.
     * @note Other fundamental dimensions read0. Does not perform numerical value conversion.
     */
    UnitDimensions(double l = 0, double m = 0, double t = 0)
        : Identity("dimensions"), l_(l), m_(m), t_(t) {}
    /**
     * @brief Read a fundamental unit-dimension exponent.
     * @param[in] d Dimension selector.
     * @return Stored length/mass/time power,0 for all others. Pure/no allocation, no range
     * validation.
     */
    double power(FundamentalUnitDimension d) const override {
        return d == FundamentalUnitDimension::Length ? l_
               : d == FundamentalUnitDimension::Mass ? m_
               : d == FundamentalUnitDimension::Time ? t_
                                                     : 0;
    }

private:
    double l_, m_, t_;
};
class Unit final : public Identity, public HC::IUnit {
public:
    /**
     * @brief Construct owned unit metadata with linear SI scale and zero offset.
     * @param[in] caption Unit text copied to Identity ID/caption, no unit registry lookup.
     * @param[in] l Length exponent.
     * @param[in] m Mass exponent.
     * @param[in] t Time exponent.
     * @param[in] factor Value-to-SI multiplicative factor, copied without finite/sign
     * validation.
     * @note Does not convert item values or supply affine temperature offsets; construction may
     * allocate.
     */
    Unit(std::string caption, double l = 0, double m = 0, double t = 0, double factor = 1)
        : Identity(caption), dimensions_(l, m, t), factor_(factor) {}
    /**
     * @brief Read stored dimensions metadata.
     * @return Borrowed pointer to owned dimension metadata; pointer lifetime is this Unit, no
     * transfer.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    HC::IUnitDimensions* dimensions() const override {
        return const_cast<UnitDimensions*>(&dimensions_);
    }
    /**
     * @brief Read stored conversionFactorToSI metadata.
     * @return Stored multiplicative scale, no finite/sign validation or numerical conversion.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    double conversionFactorToSI() const override { return factor_; }
    /**
     * @brief Read stored offsetToSI metadata.
     * @return 0; this simple unit has no affine offset.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    double offsetToSI() const override { return 0; }

private:
    UnitDimensions dimensions_;
    double factor_;
};
class Definition : public Identity, public HC::IQuantity {
public:
    /**
     * @brief Construct scalar quantity semantics, units and default unbounded range metadata.
     * @param[in] unit Unit label, no parsing/registry lookup.
     * @param[in] l Length exponent.
     * @param[in] m Mass exponent.
     * @param[in] t Time exponent.
     * @param[in] kind Intensive/extensive/unknown value semantics retained verbatim.
     * @param[in] factor Multiplicative value-to-SI scale, offset always0.
     * @note Missing NaN, default0 and infinite extrema are metadata, not setter validation.
     * Construction may allocate.
     */
    Definition(std::string unit = "unitless", double l = 0, double m = 0, double t = 0,
               HC::ValueKind kind = HC::ValueKind::Extensive, double factor = 1)
        : Identity("value"), unit_(unit, l, m, t, factor), kind_(kind) {}
    /**
     * @brief Read stored valueKind metadata.
     * @return Stored quantity intensive/extensive/unknown semantics.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    HC::ValueKind valueKind() const override { return kind_; }
    /**
     * @brief Read stored missingValue metadata.
     * @return Quiet NaN marker, not stored physical value.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    double missingValue() const override { return std::numeric_limits<double>::quiet_NaN(); }
    /**
     * @brief Read stored defaultValue metadata.
     * @return 0, independent of current item value.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    double defaultValue() const override { return 0; }
    /**
     * @brief Read stored unit metadata.
     * @return Borrowed pointer to owned Unit; lifetime follows definition, no transfer.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    HC::IUnit* unit() const override { return const_cast<Unit*>(&unit_); }
    /**
     * @brief Read stored minValue metadata.
     * @return Negative infinity metadata bound, no runtime clipping.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    double minValue() const override { return -std::numeric_limits<double>::infinity(); }
    /**
     * @brief Read stored maxValue metadata.
     * @return Positive infinity metadata bound, no runtime clipping.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    double maxValue() const override { return std::numeric_limits<double>::infinity(); }

private:
    Unit unit_;
    HC::ValueKind kind_;
};
using ValueEvent = const std::shared_ptr<HC::IComponentDataItemValueChanged>&;
class Item : public Identity, public virtual HC::IComponentDataItem, public Signal<ValueEvent> {
public:
    /**
     * @brief Construct one owned scalar data-item metadata wrapper with nonowned model
     * association.
     * @param[in] id Item ID, moved into Identity.
     * @param[in] component Borrowed model pointer, nullable; caller keeps model alive while item
     * used.
     * @param[in] kind Declared buffer element kind; actual storage supports String or double
     * numeric branch.
     * @param[in] units Unit label retained verbatim.
     * @param[in] l Length exponent.
     * @param[in] m Mass exponent.
     * @param[in] t Time exponent.
     * @param[in] valueKind Quantity semantics; String always forces Unknown.
     * @param[in] factor Multiplicative value-to-SI scale, no automatic value conversion.
     * @note Owns one dimension and definition, initializes numeric0/string empty. No kind/range
     * validation or model lifecycle mutation; construction may allocate.
     */
    Item(std::string id, HC::IModelComponent* component, HC::DataKind kind = HC::DataKind::Float64,
         std::string units = "unitless", double l = 0, double m = 0, double t = 0,
         HC::ValueKind valueKind = HC::ValueKind::Extensive, double factor = 1)
        : Identity(std::move(id)), component_(component), kind_(kind), dimension_("value"),
          definition_(units, l, m, t,
                      kind == HC::DataKind::String ? HC::ValueKind::Unknown : valueKind, factor) {}
    /**
     * @brief Block both property and value event dispatch on this scalar item.
     * @param[in] b True suppresses both signal specializations, false reenables; no queued
     * replay.
     * @note Does not mutate item value or notify model; exclusive owner access, allocation-free.
     */
    void blockSignals(bool b) override {
        Signal<std::string>::blockSignals(b);
        Signal<ValueEvent>::blockSignals(b);
    }
    /**
     * @brief Read stored modelComponent metadata.
     * @return Nonowned associated model pointer, possibly null; caller manages lifetime.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    HC::IModelComponent* modelComponent() const override { return component_; }
    /**
     * @brief Read stored dimensions metadata.
     * @return Owning vector containing one borrowed owned-dimension pointer; item lifetime
     * bounds pointer.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::vector<HC::IDimension*> dimensions() const override {
        return {const_cast<Dimension*>(&dimension_)};
    }
    /**
     * @brief Read stored shape metadata.
     * @return Owning vector[1], scalar logical extent.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::vector<int64_t> shape() const override { return {1}; }
    /**
     * @brief Read stored dataKind metadata.
     * @return Declared buffer kind, no dynamic value inspection.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    HC::DataKind dataKind() const override { return kind_; }
    /**
     * @brief Read stored valueDefinition metadata.
     * @return Borrowed pointer to owned scalar Definition; item lifetime bounds pointer, no
     * transfer.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    HC::IValueDefinition* valueDefinition() const override {
        return const_cast<Definition*>(&definition_);
    }
    /**
     * @brief Copy the scalar value into a validated caller host buffer.
     * @param[in] d Writable descriptor with exact declared kind, Host/None backend, rank1,
     * shape[count0], valid item size and data nonnull for nonempty request. String data points
     * to live std::string; numeric branch copies sizeof(double), so supported numeric kind is
     * Float64.
     * @param[in] start Exactly one scalar offset0 or1, fitting requested range.
     * @param[in] count Exactly one extent0 or1 with start+count<=1; zero request succeeds
     * without data write.
     * @param[out] message Optional diagnostic output, empty for descriptor success; caught
     * std::exception text on copy failure.
     * @return True on validated copy/empty request, false on invalid buffer or caught copy
     * exception. No allocation/ownership transfer of destination storage; string assignment can
     * allocate. Does not advance model or emit events; caller serializes public text/number
     * changes and stable metadata.
     */
    bool getValuesInto(const HC::BufferDescriptor& d, std::span<const int64_t> start,
                       std::span<const int64_t> count,
                       std::string* message = nullptr) const override {
        if (!valid(d, start, count, message))
            return false;
        if (count[0] == 0)
            return true;
        try {
            if (kind_ == HC::DataKind::String)
                *static_cast<std::string*>(d.data) = text;
            else
                std::memcpy(d.data, &number, sizeof(number));
            return true;
        } catch (const std::exception& e) {
            if (message)
                *message = e.what();
            return false;
        }
    }
    /**
     * @brief Refuse writes to the output-only scalar base item.
     * @param[out] message Optional receives read-only diagnostic.
     * @return Always false; unnamed descriptor/start/count intentionally ignored, no buffer
     * reads, value, model lifecycle or clock change. Diagnostic assignment may allocate.
     */
    bool setValuesFrom(const HC::BufferDescriptor&, std::span<const int64_t>,
                       std::span<const int64_t>, std::string* message = nullptr) override {
        if (message)
            *message = "This output is read only";
        return false;
    }
    std::string text;
    double number = 0;

protected:
    /**
     * @brief Validate a scalar host-buffer slice against this item's declared kind.
     * @param[in] d Descriptor, requires Host, no device backend, rank1, shape pointer matching
     * n0 and valid item size; data may be null for zero extent. Does not validate strides/actual
     * allocation extent.
     * @param[in] s Exactly one start index0..1.
     * @param[in] n Exactly one extent0..1 with n0<=1-s0.
     * @param[out] m Optional diagnostic, reset empty on true or replaced on false.
     * @return Descriptor/slice admissibility only; no buffer write, value mutation or lifecycle
     * advance. Caller owns actual host memory and stable descriptor pointers; diagnostic string
     * may allocate.
     */
    bool valid(const HC::BufferDescriptor& d, std::span<const int64_t> s,
               std::span<const int64_t> n, std::string* m) const {
        const bool ok = s.size() == 1 && n.size() == 1 && s[0] >= 0 && s[0] <= 1 && n[0] >= 0 &&
                        n[0] <= 1 - s[0] && d.kind == kind_ && d.space == HC::MemorySpace::Host &&
                        d.backend == HC::DeviceBackend::None && d.rank == 1 && d.shape &&
                        d.shape[0] == n[0] && (!n[0] || d.data) && HC::Helpers::hasValidItemSize(d);
        if (m)
            *m = ok ? "" : "Expected a host scalar in a one-element buffer of the declared type";
        return ok;
    }

private:
    HC::IModelComponent* component_;
    HC::DataKind kind_;
    Dimension dimension_;
    Definition definition_;
};
class Output final : public Item, public HC::IOutput {
public:
    /**
     * @brief Inherit scalar Item construction and metadata/borrowed-model ownership.
     * @copydetails openswmm::couplers::Item::Item
     */
    using Item::Item;
    /**
     * @brief Read stored consumers metadata.
     * @return Owning vector copy of nonowned consumer pointers; connections/pointees can later
     * change.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::vector<HC::IInput*> consumers() const override { return consumers_; }
    /**
     * @brief Wire a nonowned compatible input as consumer, avoiding null/duplicate entries.
     * @param[in,out] i Borrowed input pointer, alive until explicitly removed or output
     * destroyed; compatibility query and setProvider(this) called before retained raw-list
     * append.
     * @throws std::invalid_argument Input refuses compatibility/provider; may include
     * diagnostic.
     * @note Append allocation can fail after input provider changed, no rollback. No output
     * value/model advance; caller serializes reciprocal wiring and lifetime, raw pointer
     * ownership not transferred.
     */
    void addConsumer(HC::IInput* i) override {
        if (!i || std::find(consumers_.begin(), consumers_.end(), i) != consumers_.end())
            return;
        std::string message;
        if (!i->canConsume(this, message) || !i->setProvider(this))
            throw std::invalid_argument("Cannot wire output: " + message);
        consumers_.push_back(i);
    }
    /**
     * @brief Remove one borrowed consumer association and conditionally clear its provider.
     * @param[in,out] i Borrowed input pointer identity; if still provider==this, calls
     * setProvider(nullptr).
     * @return True when list entry removed, false absent. Provider refusal ignored after
     * removal, no rollback. No deletion or physical update; external input must still be alive,
     * exclusive wiring lifecycle.
     */
    bool removeConsumer(HC::IInput* i) override {
        if (std::erase(consumers_, i) == 0)
            return false;
        if (i && i->provider() == this)
            (void)i->setProvider(nullptr);
        return true;
    }
    /**
     * @brief Read stored adaptedOutputs metadata.
     * @return Owning vector copy of nonowned adapted-output pointers; no pointer lifetime
     * extension.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::vector<HC::IAdaptedOutput*> adaptedOutputs() const override { return adapted_; }
    /**
     * @brief Retain one nonnull adapted output identity without taking ownership.
     * @param[in] o Borrowed adapted-output pointer, caller keeps alive and removes before
     * destruction.
     * @note Duplicate pointer skipped; no compatibility/provider setup or update. Vector
     * allocation may throw, exclusive connection lifecycle required.
     */
    void addAdaptedOutput(HC::IAdaptedOutput* o) override {
        if (o && std::find(adapted_.begin(), adapted_.end(), o) == adapted_.end())
            adapted_.push_back(o);
    }
    /**
     * @brief Remove a borrowed adapted output pointer from the association list.
     * @param[in] o Pointer identity, may be null; never dereferenced/deleted.
     * @return True if any matching pointer removed, false absent; no reciprocal detach or model
     * update.
     */
    bool removeAdaptedOutput(HC::IAdaptedOutput* o) override { return std::erase(adapted_, o) > 0; }
    // The host schedules physical updates. Reading an export must never advance
    // the donor, including repeated reads by multiple consumers or recorders.
    /**
     * @brief Preserve host-scheduled physical state when an export is requested.
     * @note Intentionally no-op; unnamed querying input is ignored. Repeated consumer/recorder
     * reads never advance donor or refresh values. Host must schedule update() before reading
     * desired snapshot.
     */
    void updateValues(const HC::IInput*) override {}

private:
    std::vector<HC::IInput*> consumers_;
    std::vector<HC::IAdaptedOutput*> adapted_;
};
using StatusEvent = const std::shared_ptr<HC::IComponentStatusChangeEventArgs>&;
class StatusChange final : public HC::IComponentStatusChangeEventArgs {
public:
    /**
     * @brief Capture one synchronous component lifecycle notification payload.
     * @param[in] c Nonowned sender pointer, must outlive event consumption.
     * @param[in] p Previous lifecycle status.
     * @param[in] s New lifecycle status.
     * @param[in] message Owned diagnostic text moved into event.
     * @note Does not validate transition or mutate component itself; allocation may throw during
     * construction.
     */
    StatusChange(HC::IModelComponent* c, HC::IModelComponent::ComponentStatus p,
                 HC::IModelComponent::ComponentStatus s, std::string message)
        : c_(c), p_(p), s_(s), message_(std::move(message)) {}
    /**
     * @brief Read stored component metadata.
     * @return Borrowed sender pointer captured by event, no lifetime extension.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    HC::IModelComponent* component() const override { return c_; }
    /**
     * @brief Read stored previousStatus metadata.
     * @return Captured previous lifecycle status.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    HC::IModelComponent::ComponentStatus previousStatus() const override { return p_; }
    /**
     * @brief Read stored status metadata.
     * @return Captured new lifecycle status.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    HC::IModelComponent::ComponentStatus status() const override { return s_; }
    /**
     * @brief Read stored message metadata.
     * @return Owning diagnostic text copy.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::string message() const override { return message_; }
    /**
     * @brief Read stored hasProgressMonitor metadata.
     * @return False; this event supplies no progress monitor.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    bool hasProgressMonitor() const override { return false; }
    /**
     * @brief Read stored percentProgress metadata.
     * @return 0 percent placeholder, no measured progress.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    float percentProgress() const override { return 0; }

private:
    HC::IModelComponent* c_;
    HC::IModelComponent::ComponentStatus p_, s_;
    std::string message_;
};
class ComponentBase : public Identity,
                      public virtual HC::IModelComponent,
                      public Signal<StatusEvent> {
public:
    using CS = HC::IModelComponent::ComponentStatus;
    /**
     * @brief Construct Created component metadata and owned instance-identity argument.
     * @param[in] id Initial component ID, not yet host-assigned identity.
     * @param[in] info Borrowed component-info pointer, nullable; caller retains lifetime through
     * component use.
     * @note Owns identity argument, no workflow/default reference directory/physical engine yet.
     * Allocation may throw.
     */
    ComponentBase(std::string id, HC::IModelComponentInfo* info);
    /**
     * @brief Read stored arguments metadata.
     * @return Owning vector containing borrowed owned identity argument pointer; component
     * lifetime bounds it.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::vector<HC::IArgument*> arguments() const override { return {identityArgument_.get()}; }
    /**
     * @brief Read stored hasAssignedInstanceIdentity metadata.
     * @return Once-assigned flag only, no uniqueness validation.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    bool hasAssignedInstanceIdentity() const { return identityAssigned_; }
    /**
     * @brief Block both property and lifecycle event specializations.
     * @param[in] b True suppresses future synchronous notifications, false permits; no event
     * replay.
     * @note Lifecycle/value state still changes while blocked; no allocation, caller serializes
     * owner.
     */
    void blockSignals(bool b) override {
        Signal<std::string>::blockSignals(b);
        Signal<StatusEvent>::blockSignals(b);
    }
    /**
     * @brief Read stored componentInfo metadata.
     * @return Borrowed factory metadata pointer captured at construction, possibly null.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    HC::IModelComponentInfo* componentInfo() const override { return info_; }
    /**
     * @brief Read stored status metadata.
     * @return Current lifecycle status, initially Created.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    CS status() const override { return status_; }
    /**
     * @brief Read stored inputs metadata.
     * @return Empty owning vector; derived components expose inputs if supported.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::vector<HC::IInput*> inputs() const override { return {}; }
    /**
     * @brief Read stored results metadata.
     * @return Empty owning vector, no data item construction.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::vector<HC::IComponentDataItem*> results() const override { return {}; }
    /**
     * @brief Read stored states metadata.
     * @return Empty owning vector, no checkpoint objects exposed here.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::vector<HC::IComponentDataItem*> states() const override { return {}; }
    /**
     * @brief Read stored workflow metadata.
     * @return Borrowed workflow pointer, nullable; host owns lifetime.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    const HC::IWorkflowComponent* workflow() const override { return workflow_; }
    /**
     * @brief Associate a host workflow pointer without ownership or notification.
     * @param[in] w Borrowed workflow, nullable to detach; lifetime must cover association.
     * @note No wiring validation, allocation or model lifecycle/clock change; host serializes
     * access.
     */
    void setWorkflow(const HC::IWorkflowComponent* w) override { workflow_ = w; }
    /**
     * @brief Read stored capabilities metadata.
     * @return Empty capability set, derived component may supply supported features.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::set<HC::Capability> capabilities() const override { return {}; }
    /**
     * @brief Copy retained component diagnostics, optionally consuming them.
     * @param[in] clear True clears retained list only after successful copy; false keeps
     * entries.
     * @return Owning copy of severity/code/component/message entries, empty if none. Does not
     * change lifecycle status or emit event; allocation can throw before clear. Exclusive access
     * when consuming.
     */
    std::vector<HC::ErrorEntry> errors(bool clear = false) override {
        auto e = errors_;
        if (clear)
            errors_.clear();
        return e;
    }
    /**
     * @brief Read stored referenceDirectory metadata.
     * @return Owning directory text copy, no filesystem normalization or access.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::string referenceDirectory() const override { return reference_; }
    /**
     * @brief Replace the host base directory used by derived path resolution.
     * @param[in] s Directory text copied verbatim, may be empty/relative; no filesystem
     * validation/access.
     * @note No signal, open engine change or lifecycle guard here. Allocation may throw, host
     * serializes configuration.
     */
    void setReferenceDirectory(const std::string& s) override { reference_ = s; }
    /**
     * @brief Append a nonfatal-severity component diagnostic without changing lifecycle status.
     * @param[in] message Diagnostic copied with Error severity,code1,current instance ID.
     * @note No event/physical update. Vector/string allocation may throw; caller serializes
     * shared error state.
     */
    void reportError(const std::string& message) {
        errors_.push_back({HC::ErrorEntry::Severity::Error, 1, id(), message});
    }

protected:
    /**
     * @brief Validate and install a lifecycle status, then synchronously notify host slots.
     * @param[in] s Requested new status, validated by HydroCouple transition policy.
     * @param[in] message Owned diagnostic text copied into event and fatal error entry when
     * Failed.
     * @throws std::logic_error Invalid transition, before status mutation.
     * @note Allocates event before installing status; Failed diagnostic/signal snapshot
     * allocation can fail after status changes. Slot callback exceptions contained by Signal. No
     * physical engine/clock advance by this base helper; exclusive lifecycle access and sender
     * lifetime through callbacks required.
     */
    void transition(CS s, std::string message = {}) {
        if (!HC::Helpers::isValidComponentStatusTransition(status_, s))
            throw std::logic_error("Invalid component lifecycle transition");
        auto event = std::make_shared<StatusChange>(this, status_, s, message);
        status_ = s;
        if (s == CS::Failed)
            errors_.push_back({HC::ErrorEntry::Severity::Fatal, 1, id(), message});
        Signal<StatusEvent>::emit(event);
    }

private:
    HC::IModelComponentInfo* info_;
    CS status_ = CS::Created;
    const HC::IWorkflowComponent* workflow_ = nullptr;
    std::string reference_;
    std::vector<HC::ErrorEntry> errors_;
    class InstanceIdentity;
    std::unique_ptr<HC::IArgument> identityArgument_;
    bool identityAssigned_ = false;
};

// A configuration argument keeps identity assignment portable across hosts
// without adding a virtual method to the shared HydroCouple ABI.
class ComponentBase::InstanceIdentity final : public Item, public HC::IArgument {
public:
    /**
     * @brief Construct the host configuration argument for a component's stable instance
     * identity.
     * @param[in,out] component Nonowned containing component, must outlive argument; seeds
     * argument text with initial ID and metadata description. Does not mark identity assigned or
     * advance lifecycle.
     */
    explicit InstanceIdentity(ComponentBase& component)
        : Item("hydrocouple.instance_id", &component, HC::DataKind::String), component_(component) {
        text = component.id();
        setCaption("Component instance identity");
        setDescription(
            "Assigned by the host before initialization; stable for the lifetime of the instance.");
    }
    /**
     * @brief Read stored role metadata.
     * @return Configuration role for host identity assignment.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    ArgumentRole role() const override { return ArgumentRole::Configuration; }
    /**
     * @brief Read stored isOptional metadata.
     * @return True, initial component ID can remain unassigned.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    bool isOptional() const override { return true; }
    /**
     * @brief Read stored isReadOnly metadata.
     * @return True once assigned OR status differs from Created; repeat same-ID initialize is
     * still allowed while Created.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    bool isReadOnly() const override {
        return component_.identityAssigned_ || component_.status() != CS::Created;
    }
    /**
     * @brief Read stored toString metadata.
     * @return Owning current component ID copy.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::string toString() const override { return component_.id(); }
    /**
     * @brief Copy current component identity into a validated scalar string host buffer.
     * @param[in] buffer Writable rank1 Host/None String descriptor with live std::string data
     * for nonempty slice.
     * @param[in] start One scalar offset0..1.
     * @param[in] count One extent0..1, fitting scalar slice;0 succeeds without write.
     * @param[out] message Optional buffer-validation diagnostic, empty on validation success.
     * @return False for invalid descriptor/slice, true otherwise. String assignment can throw
     * (not caught here); no model/identity mutation, clock advance or event emission. Host owns
     * stable buffer/lifecycle.
     */
    bool getValuesInto(const HC::BufferDescriptor& buffer, std::span<const int64_t> start,
                       std::span<const int64_t> count,
                       std::string* message = nullptr) const override {
        if (!valid(buffer, start, count, message))
            return false;
        if (count[0])
            *static_cast<std::string*>(buffer.data) = component_.id();
        return true;
    }
    /**
     * @brief Perform the identity argument persistence hook.
     * @note No-op; identity persistence is host serialization, no file write. No allocation or
     * lifecycle/clock update.
     */
    void saveData() override {}
    /**
     * @brief Read stored fileFilters metadata.
     * @return Empty vector; file input is unsupported.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::vector<std::string> fileFilters() const override { return {}; }
    /**
     * @brief Read stored validComponentDataItemTypes metadata.
     * @return Empty vector; data-item identity initialization is unsupported.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    std::vector<const std::type_info*> validComponentDataItemTypes() const override { return {}; }
    /**
     * @brief Test supported identity assignment/serialization formats.
     * @param[in] type Argument representation selector.
     * @return True for String or JSON only; does not inspect values, change lifecycle or
     * allocate.
     */
    bool isValidArgType(ArgumentInputType type) const override {
        return type == ArgumentInputType::String || type == ArgumentInputType::JSON;
    }
    /**
     * @brief Read stored currentArgumentInputType metadata.
     * @return Last successful String/JSON representation, default String.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    ArgumentInputType currentArgumentInputType() const override { return type_; }
    /**
     * @brief Assign a nonempty stable host identity once while containing component is Created.
     * @param[in] value Bare String ID or JSON one-string array/object with values array.
     * @param[in] type String/JSON only, no file/data-item input.
     * @param[out] message Empty on success, caught std::exception diagnostic otherwise.
     * @return True after assignment, false on unsupported/malformed/empty/lifecycle/conflicting
     * repeated ID. Repeated identical ID allowed while Created despite read-only metadata flag.
     * On success replaces ID, assignment latch, mirrored text and stored input type, no property
     * event/physical clock advance. Exceptions can occur after partial ID/latch changes; catch
     * records Error diagnostic (which can itself throw). Host owns exclusive initial
     * configuration and lifetime stability.
     */
    bool initialize(const std::string& value, ArgumentInputType type,
                    std::string& message) override {
        try {
            if (!isValidArgType(type))
                throw std::invalid_argument("Instance identity accepts String or JSON");
            std::string id = value;
            if (type == ArgumentInputType::JSON) {
                const auto parsed = nlohmann::json::parse(value);
                const auto values = parsed.is_object() ? parsed.at("values") : parsed;
                if (!values.is_array() || values.size() != 1 || !values[0].is_string())
                    throw std::invalid_argument(
                        "Instance identity requires exactly one string value");
                id = values[0].get<std::string>();
            }
            if (id.empty() || component_.status() != CS::Created ||
                (component_.identityAssigned_ && id != component_.id()))
                throw std::invalid_argument(
                    "Assign a nonempty instance identity once, before initialization");
            component_.setInstanceId(id);
            component_.identityAssigned_ = true;
            text = id;
            type_ = type;
            message.clear();
            return true;
        } catch (const std::exception& error) {
            message = error.what();
            component_.reportError(message);
            return false;
        }
    }
    /**
     * @brief Refuse identity initialization from another component data item.
     * @param[out] message Receives instruction to use String/JSON; unnamed source data item
     * ignored.
     * @return Always false, appends component Error diagnostic. No source reads,
     * identity/lifecycle/clock change; diagnostic allocation may throw, exclusive error-list
     * access.
     */
    bool initialize(const HC::IComponentDataItem&, std::string& message) override {
        message = "Set instance identity with String or JSON";
        component_.reportError(message);
        return false;
    }
    /**
     * @brief Assign stable identity from one validated scalar string host buffer.
     * @param[in] buffer Readable rank1 Host/None String descriptor, live std::string for
     * nonempty slice.
     * @param[in] start One scalar offset0..1.
     * @param[in] count One fitting extent0..1; zero validates only, no
     * assignment/lifecycle-policy check.
     * @param[out] message Optional final diagnostic, empty on success.
     * @return True after permitted assignment/empty slice, false on descriptor or initialize
     * refusal; records component Error diagnostic on failure. No physical clock advance.
     * String/diagnostic exceptions may propagate after partial ID assignment; exclusive Created
     * configuration phase and stable buffer required.
     */
    bool setValuesFrom(const HC::BufferDescriptor& buffer, std::span<const int64_t> start,
                       std::span<const int64_t> count, std::string* message = nullptr) override {
        std::string diagnostic;
        bool ok = valid(buffer, start, count, &diagnostic);
        if (!ok)
            component_.reportError(diagnostic);
        if (ok && count[0])
            ok = initialize(*static_cast<const std::string*>(buffer.data),
                            ArgumentInputType::String, diagnostic);
        if (message)
            *message = diagnostic;
        return ok;
    }
    /**
     * @brief Serialize the current stable component identity in requested supported
     * representation.
     * @param[in] type String returns bare ID; JSON returns object with one-element values string
     * array.
     * @param[out] value Owning serialized text, preserved on unsupported type.
     * @param[out] message Empty success or unsupported-format diagnostic.
     * @return True for accepted format, false unsupported. Does not require assigned
     * flag/lifecycle state; no engine/clock mutation. String/JSON allocation exceptions
     * propagate; caller holds stable identity.
     */
    bool serialize(ArgumentInputType type, std::string& value,
                   std::string& message) const override {
        if (!isValidArgType(type)) {
            message = "Instance identity accepts String or JSON";
            return false;
        }
        value = type == ArgumentInputType::String
                    ? component_.id()
                    : nlohmann::json{{"values", {component_.id()}}}.dump();
        message.clear();
        return true;
    }

private:
    ComponentBase& component_;
    ArgumentInputType type_ = ArgumentInputType::String;
};

inline ComponentBase::ComponentBase(std::string id, HC::IModelComponentInfo* info)
    : Identity(std::move(id)), info_(info),
      identityArgument_(std::make_unique<InstanceIdentity>(*this)) {}
class Clock final : public Signal<std::string>, public HC::Temporal::ITimeSpan {
public:
    double day = 2440587.5, span = 0;
    /**
     * @brief Read scalar clock metadata.
     * @return Stored absolute astronomical Julian day (days).
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    double julianDay() const override { return day; }
    /**
     * @brief Read scalar clock metadata.
     * @return Julian day minus2400000.5 (days).
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    double modifiedJulianDay() const override { return day - 2400000.5; }
    /**
     * @brief Read scalar clock metadata.
     * @return Julian day minus2415018.5, SWMM serial date days.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    double serialDate() const override { return day - 2415018.5; }
    /**
     * @brief Read scalar clock metadata.
     * @return Stored interval length days, no positivity validation.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    double duration() const override { return span; }
    /**
     * @brief Read scalar clock metadata.
     * @return Stored absolute start day plus span days.
     * @note No physical model/clock advance or notifications; copied strings/containers may
     * allocate. Caller keeps owner/pointees stable during access.
     */
    double endJulianDay() const override { return day + span; }
};
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
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
template<class T> class Signal : public virtual HC::ISignal<T> {
public:
    void connect(const std::shared_ptr<HC::ISlot<T>>& slot) override {
        if (slot && std::find(slots_.begin(),slots_.end(),slot)==slots_.end()) slots_.push_back(slot);
    }
    void disconnect(const std::shared_ptr<HC::ISlot<T>>& slot) override { std::erase(slots_,slot); }
    void blockSignals(bool value) override { blocked_=value; }
    void emit(T value) override { if (!blocked_) { const auto slots=slots_; for (const auto& slot:slots) { try { (*slot)(*this,value); } catch (...) {} } } }
private:
    bool blocked_=false;
    std::vector<std::shared_ptr<HC::ISlot<T>>> slots_;
};
class Identity : public virtual HC::IIdentity, public Signal<std::string> {
public:
    explicit Identity(std::string id,std::string caption={}) : id_(std::move(id)),caption_(caption.empty()?id_:std::move(caption)) {}
    const std::string& id() const override { return id_; }
    const std::string& caption() const override { return caption_; }
    const std::string& description() const override { return description_; }
    void setCaption(const std::string& v) override { caption_=v; Signal<std::string>::emit("caption"); }
    void setDescription(const std::string& v) override { description_=v; Signal<std::string>::emit("description"); }
    void setInstanceId(const std::string& v) { id_=v; }
private:
    std::string id_,caption_,description_;
};
class Dimension final : public Identity, public HC::IDimension {
public:
    explicit Dimension(std::string id) : Identity(std::move(id)) {}
    LengthType lengthType() const override { return LengthType::Static; }
    DimensionRole role() const override { return DimensionRole::Other; }
};
class UnitDimensions final : public Identity, public HC::IUnitDimensions {
public:
    UnitDimensions(double l=0,double m=0,double t=0) : Identity("dimensions"),l_(l),m_(m),t_(t) {}
    double power(FundamentalUnitDimension d) const override {
        return d==FundamentalUnitDimension::Length?l_:d==FundamentalUnitDimension::Mass?m_:d==FundamentalUnitDimension::Time?t_:0;
    }
private: double l_,m_,t_;
};
class Unit final : public Identity, public HC::IUnit {
public:
    Unit(std::string caption,double l=0,double m=0,double t=0,double factor=1) : Identity(caption),dimensions_(l,m,t),factor_(factor) {}
    HC::IUnitDimensions* dimensions() const override { return const_cast<UnitDimensions*>(&dimensions_); }
    double conversionFactorToSI() const override { return factor_; }
    double offsetToSI() const override { return 0; }
private: UnitDimensions dimensions_; double factor_;
};
class Definition : public Identity, public HC::IQuantity {
public:
    Definition(std::string unit="unitless",double l=0,double m=0,double t=0,HC::ValueKind kind=HC::ValueKind::Extensive,double factor=1) : Identity("value"),unit_(unit,l,m,t,factor),kind_(kind) {}
    HC::ValueKind valueKind() const override { return kind_; }
    double missingValue() const override { return std::numeric_limits<double>::quiet_NaN(); }
    double defaultValue() const override { return 0; }
    HC::IUnit* unit() const override { return const_cast<Unit*>(&unit_); }
    double minValue() const override { return -std::numeric_limits<double>::infinity(); }
    double maxValue() const override { return std::numeric_limits<double>::infinity(); }
private: Unit unit_; HC::ValueKind kind_;
};
using ValueEvent = const std::shared_ptr<HC::IComponentDataItemValueChanged>&;
class Item : public Identity, public virtual HC::IComponentDataItem, public Signal<ValueEvent> {
public:
    Item(std::string id,HC::IModelComponent* component,HC::DataKind kind=HC::DataKind::Float64,
         std::string units="unitless",double l=0,double m=0,double t=0,
         HC::ValueKind valueKind=HC::ValueKind::Extensive,double factor=1)
        : Identity(std::move(id)),component_(component),kind_(kind),dimension_("value"),definition_(units,l,m,t,kind==HC::DataKind::String?HC::ValueKind::Unknown:valueKind,factor) {}
    void blockSignals(bool b) override { Signal<std::string>::blockSignals(b); Signal<ValueEvent>::blockSignals(b); }
    HC::IModelComponent* modelComponent() const override { return component_; }
    std::vector<HC::IDimension*> dimensions() const override { return {const_cast<Dimension*>(&dimension_)}; }
    std::vector<int64_t> shape() const override { return {1}; }
    HC::DataKind dataKind() const override { return kind_; }
    HC::IValueDefinition* valueDefinition() const override { return const_cast<Definition*>(&definition_); }
    bool getValuesInto(const HC::BufferDescriptor& d,std::span<const int64_t> start,
        std::span<const int64_t> count,std::string* message=nullptr) const override {
        if (!valid(d,start,count,message)) return false;
        if (count[0]==0) return true;
        try {
            if (kind_==HC::DataKind::String) *static_cast<std::string*>(d.data)=text;
            else std::memcpy(d.data,&number,sizeof(number));
            return true;
        } catch(const std::exception& e) { if(message)*message=e.what();return false; }
    }
    bool setValuesFrom(const HC::BufferDescriptor&,std::span<const int64_t>,
        std::span<const int64_t>,std::string* message=nullptr) override {
        if(message)*message="This output is read only"; return false;
    }
    std::string text;
    double number=0;
protected:
    bool valid(const HC::BufferDescriptor& d,std::span<const int64_t> s,std::span<const int64_t> n,std::string* m) const {
        const bool ok=s.size()==1 && n.size()==1 && s[0]>=0 && s[0]<=1 && n[0]>=0 && n[0]<=1-s[0] &&
            d.kind==kind_ && d.space==HC::MemorySpace::Host && d.backend==HC::DeviceBackend::None &&
            d.rank==1 && d.shape && d.shape[0]==n[0] && (!n[0] || d.data) &&
            HC::Helpers::hasValidItemSize(d);
        if(m)*m=ok?"":"Expected a host scalar in a one-element buffer of the declared type";
        return ok;
    }
private:
    HC::IModelComponent* component_; HC::DataKind kind_;
    Dimension dimension_; Definition definition_;
};
class Output final : public Item, public HC::IOutput {
public:
    using Item::Item;
    std::vector<HC::IInput*> consumers() const override { return consumers_; }
    void addConsumer(HC::IInput* i) override {
        if(!i || std::find(consumers_.begin(),consumers_.end(),i)!=consumers_.end())return;
        std::string message;
        if(!i->canConsume(this,message) || !i->setProvider(this))throw std::invalid_argument("Cannot wire output: "+message);
        consumers_.push_back(i);
    }
    bool removeConsumer(HC::IInput* i) override {
        if(std::erase(consumers_,i)==0)return false;
        if(i && i->provider()==this)(void)i->setProvider(nullptr);
        return true;
    }
    std::vector<HC::IAdaptedOutput*> adaptedOutputs() const override { return adapted_; }
    void addAdaptedOutput(HC::IAdaptedOutput* o) override { if(o&&std::find(adapted_.begin(),adapted_.end(),o)==adapted_.end()) adapted_.push_back(o); }
    bool removeAdaptedOutput(HC::IAdaptedOutput* o) override { return std::erase(adapted_,o)>0; }
    // The host schedules physical updates. Reading an export must never advance
    // the donor, including repeated reads by multiple consumers or recorders.
    void updateValues(const HC::IInput*) override {}
private: std::vector<HC::IInput*> consumers_; std::vector<HC::IAdaptedOutput*> adapted_;
};
using StatusEvent = const std::shared_ptr<HC::IComponentStatusChangeEventArgs>&;
class StatusChange final : public HC::IComponentStatusChangeEventArgs {
public:
    StatusChange(HC::IModelComponent* c,HC::IModelComponent::ComponentStatus p,
        HC::IModelComponent::ComponentStatus s,std::string message) : c_(c),p_(p),s_(s),message_(std::move(message)) {}
    HC::IModelComponent* component() const override { return c_; }
    HC::IModelComponent::ComponentStatus previousStatus() const override { return p_; }
    HC::IModelComponent::ComponentStatus status() const override { return s_; }
    std::string message() const override { return message_; }
    bool hasProgressMonitor() const override { return false; }
    float percentProgress() const override { return 0; }
private: HC::IModelComponent* c_; HC::IModelComponent::ComponentStatus p_,s_; std::string message_;
};
class ComponentBase : public Identity, public virtual HC::IModelComponent, public Signal<StatusEvent> {
public:
    using CS=HC::IModelComponent::ComponentStatus;
    ComponentBase(std::string id,HC::IModelComponentInfo* info);
    std::vector<HC::IArgument*> arguments() const override { return {identityArgument_.get()}; }
    bool hasAssignedInstanceIdentity() const { return identityAssigned_; }
    void blockSignals(bool b) override { Signal<std::string>::blockSignals(b); Signal<StatusEvent>::blockSignals(b); }
    HC::IModelComponentInfo* componentInfo() const override { return info_; }
    CS status() const override { return status_; }
    std::vector<HC::IInput*> inputs() const override { return {}; }
    std::vector<HC::IComponentDataItem*> results() const override { return {}; }
    std::vector<HC::IComponentDataItem*> states() const override { return {}; }
    const HC::IWorkflowComponent* workflow() const override { return workflow_; }
    void setWorkflow(const HC::IWorkflowComponent* w) override { workflow_=w; }
    std::set<HC::Capability> capabilities() const override { return {}; }
    std::vector<HC::ErrorEntry> errors(bool clear=false) override { auto e=errors_; if(clear)errors_.clear(); return e; }
    std::string referenceDirectory() const override { return reference_; }
    void setReferenceDirectory(const std::string& s) override { reference_=s; }
    void reportError(const std::string& message) {
        errors_.push_back({HC::ErrorEntry::Severity::Error,1,id(),message});
    }
protected:
    void transition(CS s,std::string message={}) {
        if(!HC::Helpers::isValidComponentStatusTransition(status_,s)) throw std::logic_error("Invalid component lifecycle transition");
        auto event=std::make_shared<StatusChange>(this,status_,s,message); status_=s;
        if(s==CS::Failed) errors_.push_back({HC::ErrorEntry::Severity::Fatal,1,id(),message});
        Signal<StatusEvent>::emit(event);
    }
private:
    HC::IModelComponentInfo* info_; CS status_=CS::Created;
    const HC::IWorkflowComponent* workflow_=nullptr; std::string reference_;
    std::vector<HC::ErrorEntry> errors_;
    class InstanceIdentity;
    std::unique_ptr<HC::IArgument> identityArgument_;
    bool identityAssigned_=false;
};

// A configuration argument keeps identity assignment portable across hosts
// without adding a virtual method to the shared HydroCouple ABI.
class ComponentBase::InstanceIdentity final : public Item, public HC::IArgument {
public:
    explicit InstanceIdentity(ComponentBase& component)
        : Item("hydrocouple.instance_id",&component,HC::DataKind::String),component_(component) {
        text=component.id(); setCaption("Component instance identity");
        setDescription("Assigned by the host before initialization; stable for the lifetime of the instance.");
    }
    ArgumentRole role() const override { return ArgumentRole::Configuration; }
    bool isOptional() const override { return true; }
    bool isReadOnly() const override { return component_.identityAssigned_ || component_.status()!=CS::Created; }
    std::string toString() const override { return component_.id(); }
    bool getValuesInto(const HC::BufferDescriptor& buffer,std::span<const int64_t> start,
        std::span<const int64_t> count,std::string* message=nullptr) const override {
        if(!valid(buffer,start,count,message))return false;
        if(count[0])*static_cast<std::string*>(buffer.data)=component_.id();
        return true;
    }
    void saveData() override {}
    std::vector<std::string> fileFilters() const override { return {}; }
    std::vector<const std::type_info*> validComponentDataItemTypes() const override { return {}; }
    bool isValidArgType(ArgumentInputType type) const override { return type==ArgumentInputType::String || type==ArgumentInputType::JSON; }
    ArgumentInputType currentArgumentInputType() const override { return type_; }
    bool initialize(const std::string& value,ArgumentInputType type,std::string& message) override {
        try {
            if(!isValidArgType(type)) throw std::invalid_argument("Instance identity accepts String or JSON");
            std::string id=value;
            if(type==ArgumentInputType::JSON) {
                const auto parsed=nlohmann::json::parse(value);
                const auto values=parsed.is_object()?parsed.at("values"):parsed;
                if(!values.is_array() || values.size()!=1 || !values[0].is_string())
                    throw std::invalid_argument("Instance identity requires exactly one string value");
                id=values[0].get<std::string>();
            }
            if(id.empty() || component_.status()!=CS::Created ||
               (component_.identityAssigned_ && id!=component_.id()))
                throw std::invalid_argument("Assign a nonempty instance identity once, before initialization");
            component_.setInstanceId(id); component_.identityAssigned_=true; text=id; type_=type;
            message.clear(); return true;
        } catch(const std::exception& error) { message=error.what(); component_.reportError(message); return false; }
    }
    bool initialize(const HC::IComponentDataItem&,std::string& message) override {
        message="Set instance identity with String or JSON"; component_.reportError(message); return false;
    }
    bool setValuesFrom(const HC::BufferDescriptor& buffer,std::span<const int64_t> start,
        std::span<const int64_t> count,std::string* message=nullptr) override {
        std::string diagnostic;
        bool ok=valid(buffer,start,count,&diagnostic);
        if(!ok)component_.reportError(diagnostic);
        if(ok && count[0]) ok=initialize(*static_cast<const std::string*>(buffer.data),ArgumentInputType::String,diagnostic);
        if(message)*message=diagnostic; return ok;
    }
    bool serialize(ArgumentInputType type,std::string& value,std::string& message) const override {
        if(!isValidArgType(type)) { message="Instance identity accepts String or JSON"; return false; }
        value=type==ArgumentInputType::String?component_.id():nlohmann::json{{"values",{component_.id()}}}.dump();
        message.clear(); return true;
    }
private:
    ComponentBase& component_; ArgumentInputType type_=ArgumentInputType::String;
};

inline ComponentBase::ComponentBase(std::string id,HC::IModelComponentInfo* info)
    : Identity(std::move(id)),info_(info),identityArgument_(std::make_unique<InstanceIdentity>(*this)) {}
class Clock final : public Signal<std::string>, public HC::Temporal::ITimeSpan {
public:
    double day=2440587.5,span=0;
    double julianDay() const override { return day; }
    double modifiedJulianDay() const override { return day-2400000.5; }
    double serialDate() const override { return day-2415018.5; }
    double duration() const override { return span; }
    double endJulianDay() const override { return day+span; }
};
}

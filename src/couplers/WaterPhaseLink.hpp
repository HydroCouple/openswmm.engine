// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ComponentBase.hpp"
#include <openswmm/engine/openswmm_split_packet.h>
#include <functional>

namespace openswmm::couplers {
inline constexpr char waterPhasePort[] = "openswmm.node-surface-water.v1";

class PhaseRequest final : public Item, public HC::IInput {
public:
    explicit PhaseRequest(HC::IModelComponent* owner)
        : Item(waterPhasePort, owner, HC::DataKind::String) {}
    HC::IOutput* provider() const override { return provider_; }
    bool canConsume(HC::IOutput* output, std::string& message) const override {
        if (!output || dynamic_cast<HC::IAdaptedOutput*>(output) || !output->modelComponent() ||
            output->modelComponent()->id() == modelComponent()->id() || output->id() != id() ||
            output->dataKind() != HC::DataKind::String || output->shape() != shape()) {
            message = "Choose the node/surface water phase provider on a distinct component";
            return false;
        }
        message.clear();
        return true;
    }
    bool setProvider(HC::IOutput* output) override {
        std::string why;
        if (output && ((provider_ && provider_ != output) || !canConsume(output, why)))
            return false;
        provider_ = output;
        return true;
    }
    std::function<void(double)> finalizeAt;
    bool setValuesFrom(const HC::BufferDescriptor& buffer, std::span<const int64_t> start,
                       std::span<const int64_t> count, std::string* message) override {
        if (!valid(buffer, start, count, message))
            return false;
        if (count.front() == 0)
            return true;
        try {
            if (!finalizeAt || !provider_ || !provider_->modelComponent() ||
                provider_->modelComponent()->status() != ComponentBase::CS::Finishing)
                throw std::invalid_argument(
                    "Only a finishing surface may request final settlement");
            const auto signal =
                nlohmann::json::parse(*static_cast<const std::string*>(buffer.data));
            if (signal.at("schema") != "openswmm.node-surface-finish/1" ||
                signal.at("network") != modelComponent()->id() ||
                signal.at("surface") != provider_->modelComponent()->id())
                throw std::invalid_argument("Invalid split finalization participants");
            finalizeAt(signal.at("time_s").get<double>());
            return true;
        } catch (const std::exception& ex) {
            if (message)
                *message = ex.what();
            return false;
        }
    }

private:
    HC::IOutput* provider_ = nullptr;
};

class PhaseReceipt final : public Output {
public:
    using Output::Output;
    std::function<void(const HC::IInput*)> serve;
    void updateValues(const HC::IInput* query) override {
        const auto connected = consumers();
        if (!query || connected.size() != 1 || connected.front() != query ||
            query->provider() != this || !adaptedOutputs().empty())
            throw std::invalid_argument("Water phase requires its single directly connected query");
        serve(query);
    }
};

class WaterPhaseLink {
    struct Frame {
        SWMM_SplitFrame value = nullptr;
        ~Frame() { swmm_split_frame_destroy(value); }
    };
    static void checked(SWMM_Engine e, int rc) {
        if (rc)
            throw std::runtime_error("Split water phase: " + std::to_string(rc) + " " +
                                     swmm_get_last_error_msg(e));
    }
    static std::string encode(SWMM_Engine e, SWMM_SplitFrame frame) {
        char* raw = nullptr;
        checked(e, swmm_split_packet_encode(e, frame, &raw));
        std::unique_ptr<char, decltype(&swmm_split_packet_free)> result(raw,
                                                                        swmm_split_packet_free);
        return raw;
    }
    static std::string read(const HC::IComponentDataItem& item) {
        std::string text;
        const int64_t zero = 0;
        if (!HC::Helpers::getValue(item, text, {&zero, 1}) || text.empty())
            throw std::runtime_error("Water phase provider has no prepared packet");
        return text;
    }
    static int exchange(SWMM_Engine e, int, SWMM_SplitFrame in, SWMM_SplitFrame* out, void* user) {
        auto& link = *static_cast<WaterPhaseLink*>(user);
        try {
            link.validate();
            auto* provider = link.request.provider();
            if (provider != link.boundProvider_)
                throw std::runtime_error("Water phase connection changed during execution");
            link.request.text = encode(e, in);
            provider->updateValues(&link.request);
            const auto text = read(*provider);
            checked(e, swmm_split_packet_decode(e, text.data(), text.size(), out));
            return SWMM_OK;
        } catch (const std::exception& error) {
            link.error = error.what();
            return SWMM_ERR_PLUGIN;
        }
    }

public:
    WaterPhaseLink(HC::IModelComponent* owner, bool surface)
        : surface(surface), request(owner), receipt(waterPhasePort, owner, HC::DataKind::String) {}
    const bool surface;
    PhaseRequest request;
    PhaseReceipt receipt;
    bool started = false, ended = false;
    std::string error;

    void validate() const {
        const auto* output =
            surface ? static_cast<const HC::IOutput*>(&receipt) : request.provider();
        if (!output || !output->adaptedOutputs().empty())
            throw std::invalid_argument("Connect one direct water phase provider and consumer");
        const auto consumers = output->consumers();
        if (consumers.size() != 1 || !consumers.front() ||
            consumers.front()->provider() != output || consumers.front()->id() != waterPhasePort ||
            (!surface && consumers.front() != &request))
            throw std::invalid_argument("Water phase requires one reciprocal connection");
    }
    void prepareSurface(SWMM_Engine e) {
        Frame geometry;
        checked(e, swmm_split_geometry(e, &geometry.value));
        receipt.text = encode(e, geometry.value);
        checked(e, swmm_engine_start(e, 0));
        started = true;
    }
    void startNetwork(SWMM_Engine e) {
        if (started)
            return;
        validate();
        boundProvider_ = request.provider();
        const auto geometry = read(*boundProvider_);
        Frame frame;
        checked(e, swmm_split_packet_decode(e, geometry.data(), geometry.size(), &frame.value));
        checked(e, swmm_split_apply_geometry(e, frame.value));
        checked(e, swmm_split_set_callback(e, exchange, this));
        checked(e, swmm_engine_start(e, 1));
        started = true;
    }
    void evaluate(SWMM_Engine e, const HC::IInput* query, double& time) {
        if (!started || ended || !e)
            throw std::runtime_error("Surface phase outside active session");
        auto text = read(*query);
        Frame in, out;
        checked(e, swmm_split_packet_decode(e, text.data(), text.size(), &in.value));
        const int phase = swmm_split_packet_phase(in.value);
        checked(e, swmm_split_evaluate(e, phase, in.value, &out.value));
        ended = phase == SWMM_SPLIT_FINISH;
        receipt.text = encode(e, out.value);
        checked(e, swmm_engine_get_elapsed_seconds(e, &time));
    }
    void finishSurface(double time) {
        if (ended)
            return;
        validate();
        auto* input = receipt.consumers().front();
        // Host may finish participants in either order. Ask the network clock
        // owner to issue FINISH before this surface releases its pending batch.
        nlohmann::json signal = {{"schema", "openswmm.node-surface-finish/1"},
                                 {"time_s", time},
                                 {"surface", receipt.modelComponent()->id()},
                                 {"network", input->modelComponent()->id()}};
        const int64_t zero = 0;
        if (!HC::Helpers::setValue(*input, signal.dump(), {&zero, 1}) || !ended)
            throw std::runtime_error("Network did not settle the final surface batch");
    }
    void finishNetwork(SWMM_Engine e) {
        if (ended)
            return;
        startNetwork(e);
        checked(e, swmm_engine_end(e));
        ended = true;
    }

private:
    HC::IOutput* boundProvider_ = nullptr;
};
}

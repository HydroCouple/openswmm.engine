// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace openswmm {
class PluginFactory;
struct ProcessComponentSpec;
struct SimulationContext;
namespace components {

/** A native host owns foreign instances; builtin process appliers stay separate. */
bool foreign_component(const ProcessComponentSpec& spec);

/**
 * SDK-free host for the qualified, already-debited external-outfall profile.
 * All exceptions are caught by the enclosing native lifecycle boundary. A
 * failed post-advance acceptance stops the run; it never retries native physics.
 */
class ProcessComponentHost {
public:
    ProcessComponentHost(void* engine, const SimulationContext& context,
                         PluginFactory& libraries, const std::string& model_directory);
    ~ProcessComponentHost();
    void start();
    double nextBoundary() const;
    void acceptBoundary(double seconds);
    void finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
}

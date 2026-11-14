// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace openswmm {
struct SplitFrame;
struct SimulationContext;
namespace quality { struct PeerFundingView; class PeerFundingReply; }
void validateMaterialSplitInput(const SimulationContext&);
std::string materialBoundaryPacket(const SplitFrame&, const SimulationContext&);
std::unique_ptr<SplitFrame> materialBoundaryFromPacket(const char*, std::size_t, const SimulationContext&);
std::string materialOfferPacket(const quality::PeerFundingView&, std::uint64_t sequence, int phase);
std::string materialProofPacket(const quality::PeerFundingReply&, std::uint64_t sequence, int phase,
    const std::vector<double>& queue, const std::vector<double>& direct, double remaining);
std::string materialAcknowledgementPacket(const quality::PeerFundingView&, std::uint64_t sequence, int phase,
    const std::vector<double>& queue, double remaining);
void matchMaterialPacket(const std::string& issued, const char* returned, std::size_t, const SimulationContext&);
}

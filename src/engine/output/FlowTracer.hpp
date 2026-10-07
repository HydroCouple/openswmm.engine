// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <openswmm/engine/openswmm_trace.h>
#include <string>
#include <vector>

namespace openswmm::trace
{
struct Node
{
    std::string id;
    int type{}, flags{};
};
struct Link
{
    std::string id;
    int from{}, to{}, type{};
    double length{};
};
class FlowTracer
{
  public:
    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<SWMM_TraceNodeAverage> na;
    std::vector<SWMM_TraceLinkAverage> la;
    SWMM_TraceOptions options{};
    SWMM_TraceInfo info{};
    std::string error;
    bool prepared = false;
    int derive();
    int prepare(const char *, const char *, const char *, SWMM_TraceProgress, void *);
    int estimate(int direction, int seed, SWMM_TraceValue *, SWMM_TraceValue *, SWMM_TraceSummary &,
                 SWMM_TraceProgress, void *);
    std::string cacheKey(const char *digest) const;
    bool readCache(const char *path, const std::string &key);
    bool writeCache(const char *path, const std::string &key);
};
} // namespace openswmm::trace

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "envoy/runtime/runtime.h"
#include "envoy/upstream/resource_manager.h"

#include "common/common/assert.h"

namespace Envoy {
namespace Upstream {

/**
 * Implementation of ResourceManager.
 * NOTE: This implementation makes some assumptions which favor simplicity over correctness.
 * 1) Primarily, it assumes that traffic will be mostly balanced over all the worker threads since
 *    no attempt is made to balance resources between them. It is possible that starvation can
 *    occur during high contention.
 * 2) Though atomics are used, it is possible for resources to temporarily go above the supplied
 *    maximums. This should not effect overall behavior.
 */
class ResourceManagerImpl : public ResourceManager {
public:
  ResourceManagerImpl(Runtime::Loader& runtime, const std::string& runtime_key,
                      uint64_t max_connections, uint64_t max_pending_requests,
                      uint64_t max_requests, uint64_t max_retries,
                      ClusterCircuitBreakersStats cb_stats)
      : connections_(max_connections, runtime, runtime_key + "max_connections", cb_stats.cx_open_),
        pending_requests_(max_pending_requests, runtime, runtime_key + "max_pending_requests",
                          cb_stats.rq_pending_open_),
        requests_(max_requests, runtime, runtime_key + "max_requests", cb_stats.rq_open_),
        retries_(max_retries, runtime, runtime_key + "max_retries", cb_stats.rq_retry_open_) {}

  // Upstream::ResourceManager
  Resource& connections() override { return connections_; }
  Resource& pendingRequests() override { return pending_requests_; }
  Resource& requests() override { return requests_; }
  Resource& retries() override { return retries_; }

private:
  struct ResourceImpl : public Resource {
    ResourceImpl(uint64_t max, Runtime::Loader& runtime, const std::string& runtime_key,
                 Stats::Gauge& open_gauge)
        : max_(max), runtime_(runtime), runtime_key_(runtime_key), open_gauge_(open_gauge) {}
    ~ResourceImpl() { ASSERT(current_ == 0); }

    // Upstream::Resource
    bool canCreate() override { return current_ < max(); }
    void inc() override {
      current_++;
      open_gauge_.set(canCreate() ? 0 : 1);
    }
    void dec() override {
      ASSERT(current_ > 0);
      current_--;
      open_gauge_.set(canCreate() ? 0 : 1);
    }
    uint64_t max() override { return runtime_.snapshot().getInteger(runtime_key_, max_); }

    const uint64_t max_;
    std::atomic<uint64_t> current_{};
    Runtime::Loader& runtime_;
    const std::string runtime_key_;

    /**
     * A gauge to notify the live circuit breaker state. The gauge is set to 0
     * to notify that the circuit breaker is closed, or to 1 to notify that it
     * is open.
     */
    Stats::Gauge& open_gauge_;
  };

  ResourceImpl connections_;
  ResourceImpl pending_requests_;
  ResourceImpl requests_;
  ResourceImpl retries_;
};

typedef std::unique_ptr<ResourceManagerImpl> ResourceManagerImplPtr;

} // namespace Upstream
} // namespace Envoy

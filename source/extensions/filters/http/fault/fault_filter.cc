#include "extensions/filters/http/fault/fault_filter.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "envoy/event/timer.h"
#include "envoy/http/codes.h"
#include "envoy/http/header_map.h"
#include "envoy/stats/stats.h"

#include "common/common/assert.h"
#include "common/common/empty_string.h"
#include "common/common/fmt.h"
#include "common/config/well_known_names.h"
#include "common/http/codes.h"
#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/http/utility.h"
#include "common/protobuf/utility.h"
#include "common/router/config_impl.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Fault {

const std::string FaultFilter::DELAY_PERCENT_KEY = "fault.http.delay.fixed_delay_percent";
const std::string FaultFilter::ABORT_PERCENT_KEY = "fault.http.abort.abort_percent";
const std::string FaultFilter::DELAY_DURATION_KEY = "fault.http.delay.fixed_duration_ms";
const std::string FaultFilter::ABORT_HTTP_STATUS_KEY = "fault.http.abort.http_status";

FaultFilterConfig::FaultFilterConfig(const envoy::config::filter::http::fault::v2::HTTPFault& fault,
                                     Runtime::Loader& runtime, const std::string& stats_prefix,
                                     Stats::Scope& scope)
    : runtime_(runtime), stats_(generateStats(stats_prefix, scope)), stats_prefix_(stats_prefix),
      scope_(scope) {

  if (fault.has_abort()) {
    abort_percent_ = fault.abort().percent();
    http_status_ = fault.abort().http_status();
  }

  if (fault.has_delay()) {
    fixed_delay_percent_ = fault.delay().percent();
    const auto& delay = fault.delay();
    fixed_duration_ms_ = PROTOBUF_GET_MS_OR_DEFAULT(delay, fixed_delay, 0);
  }

  for (const Router::ConfigUtility::HeaderData& header_map : fault.headers()) {
    fault_filter_headers_.push_back(header_map);
  }

  upstream_cluster_ = fault.upstream_cluster();

  for (const auto& node : fault.downstream_nodes()) {
    downstream_nodes_.insert(node);
  }
}

FaultFilter::FaultFilter(FaultFilterConfigSharedPtr config) : config_(config) {}

FaultFilter::~FaultFilter() { ASSERT(!delay_timer_); }

// Delays and aborts are independent events. One can inject a delay
// followed by an abort or inject just a delay or abort. In this callback,
// if we inject a delay, then we will inject the abort in the delay timer
// callback.
Http::FilterHeadersStatus FaultFilter::decodeHeaders(Http::HeaderMap& headers, bool) {
  // Route-level configuration overrides filter-level configuration
  // TODO (rshriram): We should not be using Runtimes when reading from route-level
  // faults until we parameterize the runtime keys for faults based on the route name.
  // Its okay for the moment, since the only use case of faults with runtimes is
  // by folks using filter level configuration in the fault filter (mainly Lyft folks).
  if (callbacks_->route() && callbacks_->route()->routeEntry()) {
    const auto metadata = callbacks_->route()->routeEntry()->metadata();
    const auto filter_it =
        metadata.filter_metadata().find(Envoy::Config::HttpFilterNames::get().FAULT);
    if (filter_it != metadata.filter_metadata().end()) {
      envoy::config::filter::http::fault::v2::HTTPFault proto_config;
      MessageUtil::jsonConvert(filter_it->second, proto_config);
      config_.reset(new FaultFilterConfig(proto_config, config_->runtime(), config_->statsPrefix(),
                                          config_->scope()));
    }
  }

  if (!matchesTargetUpstreamCluster()) {
    return Http::FilterHeadersStatus::Continue;
  }

  if (!matchesDownstreamNodes(headers)) {
    return Http::FilterHeadersStatus::Continue;
  }

  // Check for header matches
  if (!Router::ConfigUtility::matchHeaders(headers, config_->filterHeaders())) {
    return Http::FilterHeadersStatus::Continue;
  }

  if (headers.EnvoyDownstreamServiceCluster()) {
    downstream_cluster_ = headers.EnvoyDownstreamServiceCluster()->value().c_str();

    downstream_cluster_delay_percent_key_ =
        fmt::format("fault.http.{}.delay.fixed_delay_percent", downstream_cluster_);
    downstream_cluster_abort_percent_key_ =
        fmt::format("fault.http.{}.abort.abort_percent", downstream_cluster_);
    downstream_cluster_delay_duration_key_ =
        fmt::format("fault.http.{}.delay.fixed_duration_ms", downstream_cluster_);
    downstream_cluster_abort_http_status_key_ =
        fmt::format("fault.http.{}.abort.http_status", downstream_cluster_);
  }

  absl::optional<uint64_t> duration_ms = delayDuration();
  if (duration_ms) {
    delay_timer_ = callbacks_->dispatcher().createTimer([this]() -> void { postDelayInjection(); });
    delay_timer_->enableTimer(std::chrono::milliseconds(duration_ms.value()));
    recordDelaysInjectedStats();
    callbacks_->requestInfo().setResponseFlag(RequestInfo::ResponseFlag::DelayInjected);
    return Http::FilterHeadersStatus::StopIteration;
  }

  if (isAbortEnabled()) {
    abortWithHTTPStatus();
    return Http::FilterHeadersStatus::StopIteration;
  }

  return Http::FilterHeadersStatus::Continue;
}

bool FaultFilter::isDelayEnabled() {
  bool enabled =
      config_->runtime().snapshot().featureEnabled(DELAY_PERCENT_KEY, config_->delayPercent());

  if (!downstream_cluster_delay_percent_key_.empty()) {
    enabled |= config_->runtime().snapshot().featureEnabled(downstream_cluster_delay_percent_key_,
                                                            config_->delayPercent());
  }

  return enabled;
}

bool FaultFilter::isAbortEnabled() {
  bool enabled =
      config_->runtime().snapshot().featureEnabled(ABORT_PERCENT_KEY, config_->abortPercent());

  if (!downstream_cluster_abort_percent_key_.empty()) {
    enabled |= config_->runtime().snapshot().featureEnabled(downstream_cluster_abort_percent_key_,
                                                            config_->abortPercent());
  }

  return enabled;
}

absl::optional<uint64_t> FaultFilter::delayDuration() {
  absl::optional<uint64_t> ret;

  if (!isDelayEnabled()) {
    return ret;
  }

  uint64_t duration =
      config_->runtime().snapshot().getInteger(DELAY_DURATION_KEY, config_->delayDuration());
  if (!downstream_cluster_delay_duration_key_.empty()) {
    duration =
        config_->runtime().snapshot().getInteger(downstream_cluster_delay_duration_key_, duration);
  }

  // Delay only if the duration is >0ms
  if (duration > 0) {
    ret = duration;
  }

  return ret;
}

uint64_t FaultFilter::abortHttpStatus() {
  // TODO(mattklein123): check http status codes obtained from runtime.
  uint64_t http_status =
      config_->runtime().snapshot().getInteger(ABORT_HTTP_STATUS_KEY, config_->abortCode());

  if (!downstream_cluster_abort_http_status_key_.empty()) {
    http_status = config_->runtime().snapshot().getInteger(
        downstream_cluster_abort_http_status_key_, http_status);
  }

  return http_status;
}

void FaultFilter::recordDelaysInjectedStats() {
  // Downstream specific stats.
  if (!downstream_cluster_.empty()) {
    const std::string stats_counter =
        fmt::format("{}fault.{}.delays_injected", config_->statsPrefix(), downstream_cluster_);

    config_->scope().counter(stats_counter).inc();
  }

  // General stats.
  config_->stats().delays_injected_.inc();
}

void FaultFilter::recordAbortsInjectedStats() {
  // Downstream specific stats.
  if (!downstream_cluster_.empty()) {
    const std::string stats_counter =
        fmt::format("{}fault.{}.aborts_injected", config_->statsPrefix(), downstream_cluster_);

    config_->scope().counter(stats_counter).inc();
  }

  // General stats.
  config_->stats().aborts_injected_.inc();
}

Http::FilterDataStatus FaultFilter::decodeData(Buffer::Instance&, bool) {
  if (delay_timer_ == nullptr) {
    return Http::FilterDataStatus::Continue;
  }
  // If the request is too large, stop reading new data until the buffer drains.
  return Http::FilterDataStatus::StopIterationAndWatermark;
}

Http::FilterTrailersStatus FaultFilter::decodeTrailers(Http::HeaderMap&) {
  return delay_timer_ == nullptr ? Http::FilterTrailersStatus::Continue
                                 : Http::FilterTrailersStatus::StopIteration;
}

FaultFilterStats FaultFilterConfig::generateStats(const std::string& prefix, Stats::Scope& scope) {
  std::string final_prefix = prefix + "fault.";
  return {ALL_FAULT_FILTER_STATS(POOL_COUNTER_PREFIX(scope, final_prefix))};
}

void FaultFilter::onDestroy() {
  resetTimerState();
  stream_destroyed_ = true;
}

void FaultFilter::postDelayInjection() {
  resetTimerState();

  // Delays can be followed by aborts
  if (isAbortEnabled()) {
    abortWithHTTPStatus();
  } else {
    // Continue request processing.
    callbacks_->continueDecoding();
  }
}

void FaultFilter::abortWithHTTPStatus() {
  callbacks_->requestInfo().setResponseFlag(RequestInfo::ResponseFlag::FaultInjected);
  Http::Utility::sendLocalReply(*callbacks_, stream_destroyed_,
                                static_cast<Http::Code>(abortHttpStatus()), "fault filter abort");
  recordAbortsInjectedStats();
}

bool FaultFilter::matchesTargetUpstreamCluster() {
  bool matches = true;

  if (!config_->upstreamCluster().empty()) {
    Router::RouteConstSharedPtr route = callbacks_->route();
    matches = route && route->routeEntry() &&
              (route->routeEntry()->clusterName() == config_->upstreamCluster());
  }

  return matches;
}

bool FaultFilter::matchesDownstreamNodes(const Http::HeaderMap& headers) {
  if (config_->downstreamNodes().empty()) {
    return true;
  }

  if (!headers.EnvoyDownstreamServiceNode()) {
    return false;
  }

  const std::string downstream_node = headers.EnvoyDownstreamServiceNode()->value().c_str();
  return config_->downstreamNodes().find(downstream_node) != config_->downstreamNodes().end();
}

void FaultFilter::resetTimerState() {
  if (delay_timer_) {
    delay_timer_->disableTimer();
    delay_timer_.reset();
  }
}

void FaultFilter::setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) {
  callbacks_ = &callbacks;
}

} // namespace Fault
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

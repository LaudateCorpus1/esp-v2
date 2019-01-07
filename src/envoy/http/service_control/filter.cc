// Copyright 2018 Google Cloud Platform Proxy Authors
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

#include "src/envoy/http/service_control/filter.h"
#include "common/http/utility.h"
#include "envoy/http/header_map.h"
#include "src/envoy/http/service_control/http_call.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ServiceControl {

using ::google::api::envoy::http::service_control::APIKey;
using ::google::protobuf::util::Status;
using Http::HeaderMap;
using Http::LowerCaseString;
using std::string;

void Filter::ExtractAPIKeyFromQuery(const HeaderMap &headers,
                                    const string &query) {
  if (!params_parsed_) {
    parsed_params_ =
        Http::Utility::parseQueryString(headers.Path()->value().c_str());
    params_parsed_ = true;
  }
  const auto &it = parsed_params_.find(query);
  if (it != parsed_params_.end()) {
    api_key_ = it->second;
  } else {
    ENVOY_LOG(debug, "API key not found by query '{}' in path: '{}'", query,
              headers.Path()->value().c_str());
  }
}

void Filter::ExtractAPIKeyFromHeader(const HeaderMap &headers,
                                     const string &header) {
  auto *entry = headers.get(LowerCaseString(header));
  if (entry) {
    api_key_ = std::string(entry->value().c_str(), entry->value().size());
  } else {
    ENVOY_LOG(debug, "API key not found by header '{}' in headerMap '{}'",
              header, headers);
  }
}

void Filter::ExtractAPIKeyFromCookie(const HeaderMap &headers,
                                     const string &cookie) {
  std::string api_key = Http::Utility::parseCookieValue(headers, cookie);
  if (!api_key.empty()) {
    api_key_ = api_key;
  } else {
    ENVOY_LOG(debug, "API key not found by cookie '{}' in headerMap '{}'",
              cookie, headers);
  }
}

Http::FilterHeadersStatus Filter::decodeHeaders(HeaderMap &headers, bool) {
  ENVOY_LOG(debug, "Called ServiceControl Filter : {}", __func__);

  uuid_ = config_->random().uuid();
  requirement_ = config_->cfg_parser().FindRequirement(
      headers.Method()->value().c_str(), headers.Path()->value().c_str());
  if (!requirement_) {
    ENVOY_LOG(debug, "No requirement matched!");
    rejectRequest(Http::Code(404),
                  "Path does not match any requirement uri_template.");
    return Http::FilterHeadersStatus::StopIteration;
  }

  service_ctx_ =
      config_->cfg_parser().FindService(requirement_->service_name());
  if (!service_ctx_) {
    ENVOY_LOG(debug, "No service matched!");
    rejectRequest(Http::Code(404), "required service is not configured.");
    return Http::FilterHeadersStatus::StopIteration;
  }

  // TODO add integration tests
  if (requirement_->api_key().allow_without_api_key()) {
    ENVOY_LOG(debug, "Service control check is not needed");
    return Http::FilterHeadersStatus::Continue;
  }

  operation_name_ = requirement_->operation_name();

  // Extract API key
  for (const auto &api_key : requirement_->api_key().api_keys()) {
    switch (api_key.key_case()) {
      case APIKey::kQuery:
        ExtractAPIKeyFromQuery(headers, api_key.query());
        break;
      case APIKey::kHeader:
        ExtractAPIKeyFromHeader(headers, api_key.header());
        break;
      case APIKey::kCookie:
        ExtractAPIKeyFromCookie(headers, api_key.cookie());
        break;
      case APIKey::KEY_NOT_SET:
        break;
    }
  }
  api_name_ = requirement_->api_name();
  api_version_ = requirement_->api_version();
  state_ = Calling;
  stopped_ = false;
  token_fetcher_ =
      config_->getCache()
          .getTokenCacheByServiceName(requirement_->service_name())
          ->getToken([this](const Status &status, const string &result) {
            onTokenDone(status, result);
          });

  if (state_ == Complete) {
    return Http::FilterHeadersStatus::Continue;
  }
  ENVOY_LOG(debug, "Called ServiceControl filter : Stop");
  stopped_ = true;
  return Http::FilterHeadersStatus::StopIteration;
}

void Filter::onDestroy() {
  if (token_fetcher_) {
    token_fetcher_();
    token_fetcher_ = nullptr;
  }
  if (check_call_) {
    check_call_->cancel();
    check_call_ = nullptr;
  }
}

void Filter::onTokenDone(const Status &status, const string &token) {
  // This stream has been reset, abort the callback.
  token_fetcher_ = nullptr;
  if (state_ == Responded) {
    return;
  }

  if (!status.ok()) {
    rejectRequest(Http::Code(401), "Failed to fetch access_token");
    return;
  }

  token_ = token;
  // Make a check call
  ::google::api_proxy::service_control::CheckRequestInfo info;
  info.operation_id = uuid_;
  info.operation_name = operation_name_;
  info.producer_project_id = service_ctx_->config().producer_project_id();
  info.api_key = api_key_;
  info.request_start_time = std::chrono::system_clock::now();

  ::google::api::servicecontrol::v1::CheckRequest check_request;
  service_ctx_->builder().FillCheckRequest(info, &check_request);
  ENVOY_LOG(debug, "Sending check : {}", check_request.DebugString());

  string suffix_uri = service_ctx_->config().service_name() + ":check";
  auto on_done = [this](const Status &status, const string &body) {
    onCheckResponse(status, body);
  };
  check_call_ = HttpCall::create(config_->cm(),
                                 service_ctx_->config().service_control_uri());
  check_call_->call(suffix_uri, token_, check_request, on_done);
}

void Filter::rejectRequest(Http::Code code, absl::string_view error_msg) {
  config_->stats().denied_.inc();
  state_ = Responded;

  decoder_callbacks_->sendLocalReply(code, error_msg, nullptr, absl::nullopt);
  decoder_callbacks_->streamInfo().setResponseFlag(
      StreamInfo::ResponseFlag::UnauthorizedExternalService);
}

void Filter::onCheckResponse(const Status &status,
                             const string &response_json) {
  ENVOY_LOG(debug, "Check response with : {}, body {}", status.ToString(),
            response_json);
  // This stream has been reset, abort the callback.
  check_call_ = nullptr;
  if (state_ == Responded) {
    return;
  }

  if (!status.ok()) {
    rejectRequest(Http::Code(401), "Check failed");
    return;
  }

  ::google::api::servicecontrol::v1::CheckResponse response_pb;
  Protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;
  const auto json_status =
      Protobuf::util::JsonStringToMessage(response_json, &response_pb, options);
  if (!json_status.ok()) {
    rejectRequest(Http::Code(401), "Check failed");
    return;
  }

  check_status_ = ::google::api_proxy::service_control::RequestBuilder::
      ConvertCheckResponse(response_pb, service_ctx_->config().service_name(),
                           &check_response_info_);
  if (!check_status_.ok()) {
    rejectRequest(Http::Code(401), "Check failed");
    return;
  }

  config_->stats().allowed_.inc();
  state_ = Complete;
  if (stopped_) {
    decoder_callbacks_->continueDecoding();
  }
}

Http::FilterDataStatus Filter::decodeData(Buffer::Instance &, bool) {
  ENVOY_LOG(debug, "Called ServiceControl Filter : {}", __func__);
  if (state_ == Calling) {
    return Http::FilterDataStatus::StopIterationAndWatermark;
  }
  return Http::FilterDataStatus::Continue;
}

Http::FilterTrailersStatus Filter::decodeTrailers(HeaderMap &) {
  ENVOY_LOG(debug, "Called ServiceControl Filter : {}", __func__);
  if (state_ == Calling) {
    return Http::FilterTrailersStatus::StopIteration;
  }
  return Http::FilterTrailersStatus::Continue;
}

void Filter::setDecoderFilterCallbacks(
    Http::StreamDecoderFilterCallbacks &callbacks) {
  decoder_callbacks_ = &callbacks;
}

void Filter::log(const HeaderMap * /*request_headers*/,
                 const HeaderMap * /*response_headers*/,
                 const HeaderMap * /*response_trailers*/,
                 const StreamInfo::StreamInfo &stream_info) {
  ENVOY_LOG(debug, "Called ServiceControl Filter : {}", __func__);

  ::google::api_proxy::service_control::ReportRequestInfo info;
  info.operation_id = uuid_;
  info.operation_name = operation_name_;
  info.producer_project_id = service_ctx_->config().producer_project_id();

  if (check_response_info_.is_api_key_valid &&
      check_response_info_.service_is_activated) {
    info.api_key = api_key_;
  }

  info.request_start_time = std::chrono::system_clock::now();
  info.api_method = operation_name_;
  info.api_name = api_name_;
  info.api_version = api_version_;
  info.log_message = operation_name_ + " is called";

  info.url = operation_name_;
  info.method = http_method_;

  info.check_response_info = check_response_info_;
  info.response_code = stream_info.responseCode().value_or(500);
  info.status = check_status_;

  info.response_code = stream_info.responseCode().value_or(500);
  info.request_size = stream_info.bytesReceived();
  info.response_size = stream_info.bytesSent();

  ::google::api::servicecontrol::v1::ReportRequest report_request;
  service_ctx_->builder().FillReportRequest(info, &report_request);
  ENVOY_LOG(debug, "Sending report : {}", report_request.DebugString());

  string suffix_uri = service_ctx_->config().service_name() + ":report";
  auto dummy_on_done = [](const Status &, const string &) {};
  HttpCall *http_call = HttpCall::create(
      config_->cm(), service_ctx_->config().service_control_uri());
  http_call->call(suffix_uri, token_, report_request, dummy_on_done);
}

}  // namespace ServiceControl
}  // namespace HttpFilters
}  // namespace Extensions
}  // namespace Envoy

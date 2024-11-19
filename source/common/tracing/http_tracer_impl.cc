#include "source/common/tracing/http_tracer_impl.h"

#include <string>

#include "envoy/config/core/v3/base.pb.h"
#include "envoy/network/address.h"
#include "envoy/tracing/http_tracer.h"
#include "envoy/type/metadata/v3/metadata.pb.h"
#include "envoy/type/tracing/v3/custom_tag.pb.h"

#include "source/common/config/metadata.h"
#include "source/common/common/assert.h"
#include "source/common/common/fmt.h"
#include "source/common/common/macros.h"
#include "source/common/common/utility.h"
#include "source/common/formatter/substitution_formatter.h"
#include "source/common/grpc/common.h"
#include "source/common/http/codes.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/header_utility.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"
#include "source/common/protobuf/utility.h"
#include "source/common/stream_info/utility.h"

#include "absl/strings/str_cat.h"


namespace Envoy {
namespace Tracing {

// TODO(perf): Avoid string creations/copies in this entire file.
static std::string buildResponseCode(const StreamInfo::StreamInfo& info) {
  return info.responseCode() ? std::to_string(info.responseCode().value()) : "0";
}

static absl::string_view valueOrDefault(const Http::HeaderEntry* header,
                                        const char* default_value) {
  return header ? header->value().getStringView() : default_value;
}

const std::string HttpTracerUtility::IngressOperation = "ingress";
const std::string HttpTracerUtility::EgressOperation = "egress";

const std::string& HttpTracerUtility::toString(OperationName operation_name) {
  switch (operation_name) {
  case OperationName::Ingress:
    return IngressOperation;
  case OperationName::Egress:
    return EgressOperation;
  }

  return EMPTY_STRING; // Make the compiler happy.
}

Decision HttpTracerUtility::shouldTraceRequest(const StreamInfo::StreamInfo& stream_info) {
  // Exclude health check requests immediately.
  if (stream_info.healthCheck()) {
    return {Reason::HealthCheck, false};
  }

  const Tracing::Reason trace_reason = stream_info.traceReason();
  switch (trace_reason) {
  case Reason::ClientForced:
  case Reason::ServiceForced:
  case Reason::Sampling:
    return {trace_reason, true};
  default:
    return {trace_reason, false};
  }
}

static void addTagIfNotNull(Span& span, const std::string& tag, const Http::HeaderEntry* entry) {
  if (entry != nullptr) {
    span.setTag(tag, entry->value().getStringView());
  }
}

static void addGrpcRequestTags(Span& span, const Http::RequestHeaderMap& headers) {
  addTagIfNotNull(span, Tracing::Tags::get().GrpcPath, headers.Path());
  addTagIfNotNull(span, Tracing::Tags::get().GrpcAuthority, headers.Host());
  addTagIfNotNull(span, Tracing::Tags::get().GrpcContentType, headers.ContentType());
  addTagIfNotNull(span, Tracing::Tags::get().GrpcTimeout, headers.GrpcTimeout());
}

template <class T> static void addGrpcResponseTags(Span& span, const T& headers) {
  addTagIfNotNull(span, Tracing::Tags::get().GrpcStatusCode, headers.GrpcStatus());
  addTagIfNotNull(span, Tracing::Tags::get().GrpcMessage, headers.GrpcMessage());
  // Set error tag when Grpc status code represents an upstream error. See
  // https://github.com/envoyproxy/envoy/issues/18877.
  absl::optional<Grpc::Status::GrpcStatus> grpc_status_code = Grpc::Common::getGrpcStatus(headers);
  if (grpc_status_code.has_value()) {
    const auto& status = grpc_status_code.value();
    if (status != Grpc::Status::WellKnownGrpcStatus::InvalidCode) {
      switch (status) {
      // Each case below is considered to be a client side error, therefore should not be
      // tagged as an upstream error. See https://grpc.github.io/grpc/core/md_doc_statuscodes.html
      // for more details about how each Grpc status code is defined and whether it is an
      // upstream error or a client error.
      case Grpc::Status::WellKnownGrpcStatus::Ok:
      case Grpc::Status::WellKnownGrpcStatus::Canceled:
      case Grpc::Status::WellKnownGrpcStatus::InvalidArgument:
      case Grpc::Status::WellKnownGrpcStatus::NotFound:
      case Grpc::Status::WellKnownGrpcStatus::AlreadyExists:
      case Grpc::Status::WellKnownGrpcStatus::PermissionDenied:
      case Grpc::Status::WellKnownGrpcStatus::FailedPrecondition:
      case Grpc::Status::WellKnownGrpcStatus::Aborted:
      case Grpc::Status::WellKnownGrpcStatus::OutOfRange:
      case Grpc::Status::WellKnownGrpcStatus::Unauthenticated:
        break;
      case Grpc::Status::WellKnownGrpcStatus::Unknown:
      case Grpc::Status::WellKnownGrpcStatus::DeadlineExceeded:
      case Grpc::Status::WellKnownGrpcStatus::Unimplemented:
      case Grpc::Status::WellKnownGrpcStatus::ResourceExhausted:
      case Grpc::Status::WellKnownGrpcStatus::Internal:
      case Grpc::Status::WellKnownGrpcStatus::Unavailable:
      case Grpc::Status::WellKnownGrpcStatus::DataLoss:
        span.setTag(Tracing::Tags::get().Error, Tracing::Tags::get().True);
        break;
      }
    }
  }
}

static void annotateVerbose(Span& span, const StreamInfo::StreamInfo& stream_info) {
  const auto start_time = stream_info.startTime();
  StreamInfo::TimingUtility timing(stream_info);
  if (timing.lastDownstreamRxByteReceived()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *timing.lastDownstreamRxByteReceived()),
             Tracing::Logs::get().LastDownstreamRxByteReceived);
  }
  if (timing.firstUpstreamTxByteSent()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *timing.firstUpstreamTxByteSent()),
             Tracing::Logs::get().FirstUpstreamTxByteSent);
  }
  if (timing.lastUpstreamTxByteSent()) {
    span.log(start_time +
                 std::chrono::duration_cast<SystemTime::duration>(*timing.lastUpstreamTxByteSent()),
             Tracing::Logs::get().LastUpstreamTxByteSent);
  }
  if (timing.firstUpstreamRxByteReceived()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *timing.firstUpstreamRxByteReceived()),
             Tracing::Logs::get().FirstUpstreamRxByteReceived);
  }
  if (timing.lastUpstreamRxByteReceived()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *timing.lastUpstreamRxByteReceived()),
             Tracing::Logs::get().LastUpstreamRxByteReceived);
  }
  if (timing.firstDownstreamTxByteSent()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *timing.firstDownstreamTxByteSent()),
             Tracing::Logs::get().FirstDownstreamTxByteSent);
  }
  if (timing.lastDownstreamTxByteSent()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *timing.lastDownstreamTxByteSent()),
             Tracing::Logs::get().LastDownstreamTxByteSent);
  }
}

std::string dumpRequestHeaders(const Envoy::Http::HeaderMap& headers)  {
    std::stringstream ss;
    
    // 定义回调函数来遍历头部条目
    headers.iterate([&ss](const Envoy::Http::HeaderEntry& header) -> Envoy::Http::HeaderMap::Iterate {
        // 获取键和值
        const std::string key = std::string(header.key().getStringView());
        const std::string value = std::string(header.value().getStringView());

        // 将其追加为 K=V 格式到字符串流中
        ss << key << "=" << value << "; ";

        return Envoy::Http::HeaderMap::Iterate::Continue;
    });
    
    // 使用 ENVOY_LOG 打印最终的字符串
    return ss.str();
}

void HttpTracerUtility::finalizeDownstreamSpan(Span& span,
                                               const Http::RequestHeaderMap* request_headers,
                                               const Http::ResponseHeaderMap* response_headers,
                                               const Http::ResponseTrailerMap* response_trailers,
                                               const StreamInfo::StreamInfo& stream_info,
                                               const Config& tracing_config) {
  // Pre response data.
  const auto start_time = stream_info.startTime();

  if (request_headers) {
    if (request_headers->RequestId()) {
      span.setTag(Tracing::Tags::get().GuidXRequestId, request_headers->getRequestIdValue());
    }
    span.setTag(
        Tracing::Tags::get().HttpUrl,
        Http::Utility::buildOriginalUri(*request_headers, tracing_config.maxPathTagLength()));
    span.setTag(Tracing::Tags::get().HttpMethod, request_headers->getMethodValue());
    span.setTag(Tracing::Tags::get().DownstreamCluster,
                valueOrDefault(request_headers->EnvoyDownstreamServiceCluster(), "-"));
    span.setTag(Tracing::Tags::get().UserAgent, valueOrDefault(request_headers->UserAgent(), "-"));
    span.setTag(
        Tracing::Tags::get().HttpProtocol,
        Formatter::SubstitutionFormatUtils::protocolToStringOrDefault(stream_info.protocol()));

    const auto& remote_address = stream_info.downstreamAddressProvider().directRemoteAddress();

    if (remote_address->type() == Network::Address::Type::Ip) {
      const auto remote_ip = remote_address->ip();
      span.setTag(Tracing::Tags::get().PeerAddress, remote_ip->addressAsString());
    } else {
      span.setTag(Tracing::Tags::get().PeerAddress, remote_address->logicalName());
    }

    if (request_headers->ClientTraceId()) {
      span.setTag(Tracing::Tags::get().GuidXClientTraceId,
                  request_headers->getClientTraceIdValue());
    }

    if (Grpc::Common::isGrpcRequestHeaders(*request_headers)) {
      addGrpcRequestTags(span, *request_headers);
    }

    std::string request_headers_str = dumpRequestHeaders(*request_headers);
    auto req_header_length = request_headers_str.length();
    ENVOY_LOG(debug, "Add downstream request http headers, length={}", req_header_length);
    span.setTag("request_headers", dumpRequestHeaders(*request_headers));
    span.setTag("request_headers.length", std::to_string(req_header_length));

    // TODO: 由于dumpState的数据可读性不佳，因此将headers中的数据展开
    // 例如：request_headers.x-powered-by = "Servlet/3.1"
  }

  span.setTag(Tracing::Tags::get().RequestSize, std::to_string(stream_info.bytesReceived()));
  span.setTag(Tracing::Tags::get().ResponseSize, std::to_string(stream_info.bytesSent()));

  setCommonTags(span, stream_info, tracing_config);
  onUpstreamResponseHeaders(span, response_headers);
  onUpstreamResponseTrailers(span, response_trailers);

  std::string req_body = Envoy::Config::Metadata::metadataValue(&stream_info.dynamicMetadata(), "cle.log.req.lua", "body").string_value();
  auto req_body_length = req_body.length();
  ENVOY_LOG(debug, "Add downstream request http body, length={}", req_body_length);
  span.setTag("request_body", req_body);
  span.setTag("request_body.length", std::to_string(req_body_length));
  
  std::string rsp_body = Envoy::Config::Metadata::metadataValue(&stream_info.dynamicMetadata(), "cle.log.rsp.lua", "body").string_value();
  auto rsp_body_length = rsp_body.length();
  ENVOY_LOG(debug, "Add downstream response http body, length={}", rsp_body_length);
  span.setTag("response_body", rsp_body);
  span.setTag("response_body.length", std::to_string(rsp_body_length));

    // 提取 RequestId
  std::string request_id = extractRequestIdFromJson(rsp_body);
  
  if (!request_id.empty()) {
      span.setTag("RequestId", request_id);
  }
    
  if(response_headers) {
    auto rsp_header = dumpRequestHeaders(*response_headers);
    auto rsp_header_length = rsp_header.length();
    ENVOY_LOG(debug, "Add downstream response http headers, length={}", rsp_header_length);
    span.setTag("response_headers", rsp_header);
    span.setTag("response_headers.length", std::to_string(rsp_header_length));
  }

  span.finishSpan();
}

// 用于解析云API的 JSON数据 并提取 RequestId
std::string HttpTracerUtility::extractRequestIdFromJson(const std::string& json_body) {
    // 空检查
    if (json_body.empty()) {
        return "";
    }

    // 使用 Protobuf 的 JSON 解析
    google::protobuf::Struct parsed_json;
    google::protobuf::util::JsonParseOptions options;
    options.ignore_unknown_fields = true;

    auto status = google::protobuf::util::JsonStringToMessage(json_body, &parsed_json, options);
    
    if (!status.ok()) {
        // JSON 解析失败的处理
        ENVOY_LOG(debug, "Failed to parse JSON response body: {}, error: {}", 
                  json_body, status.message());
        return "";
    }

    // 定义可能的路径
    std::vector<std::vector<std::string>> possible_paths = {
        {"data", "Response", "RequestId"},
        {"Response", "RequestId"}
    };

    // 遍历路径尝试查找
    for (const auto& path : possible_paths) {
        std::string request_id = findNestedValue(parsed_json, path);
        if (!request_id.empty()) {
            return request_id;
        }
    }

    return "";
}

// 递归查找嵌套的 JSON 值
std::string HttpTracerUtility::findNestedValue(const google::protobuf::Struct& current_struct, 
                            const std::vector<std::string>& path) {
    if (path.empty()) {
        return "";
    }

    auto it = current_struct.fields().find(path[0]);
    if (it == current_struct.fields().end()) {
        return "";
    }

    if (path.size() == 1) {
        if (it->second.kind_case() == google::protobuf::Value::kStringValue) {
            return it->second.string_value();
        }
        return "";
    }

    if (it->second.kind_case() == google::protobuf::Value::kStructValue) {
        return findNestedValue(it->second.struct_value(), 
                               std::vector<std::string>(path.begin() + 1, path.end()));
    }

    return "";
}


void HttpTracerUtility::finalizeUpstreamSpan(Span& span, const StreamInfo::StreamInfo& stream_info,
                                             const Config& tracing_config) {

  const auto start_time = stream_info.startTime();                                              

  span.setTag(
      Tracing::Tags::get().HttpProtocol,
      Formatter::SubstitutionFormatUtils::protocolToStringOrDefault(stream_info.protocol()));

  if (stream_info.upstreamInfo() && stream_info.upstreamInfo()->upstreamHost()) {
    auto upstream_address = stream_info.upstreamInfo()->upstreamHost()->address();
    // TODO(wbpcode): separated `upstream_address` may be meaningful to the downstream span.
    // But for the upstream span, `peer.address` should be used.
    span.setTag(Tracing::Tags::get().UpstreamAddress, upstream_address->asStringView());
    // TODO(wbpcode): may be set this tag in the setCommonTags.
    span.setTag(Tracing::Tags::get().PeerAddress, upstream_address->asStringView());
  }

  setCommonTags(span, stream_info, tracing_config);

  std::string req_body = Envoy::Config::Metadata::metadataValue(&stream_info.dynamicMetadata(), "cle.log.req.lua", "body").string_value();
  ENVOY_LOG(debug, "Add upstream request http body");
  span.setTag("request_body", req_body);
  
  std::string rsp_body = Envoy::Config::Metadata::metadataValue(&stream_info.dynamicMetadata(), "cle.log.rsp.lua", "body").string_value();
  ENVOY_LOG(debug, "Add upstream response http body");
  span.setTag("response_body", rsp_body);

  span.finishSpan();
}

void HttpTracerUtility::onUpstreamResponseHeaders(Span& span,
                                                  const Http::ResponseHeaderMap* response_headers) {
  if (response_headers && response_headers->GrpcStatus() != nullptr) {
    addGrpcResponseTags(span, *response_headers);
  }
}

void HttpTracerUtility::onUpstreamResponseTrailers(
    Span& span, const Http::ResponseTrailerMap* response_trailers) {
  if (response_trailers && response_trailers->GrpcStatus() != nullptr) {
    addGrpcResponseTags(span, *response_trailers);
  }
}

void HttpTracerUtility::setCommonTags(Span& span, const StreamInfo::StreamInfo& stream_info,
                                      const Config& tracing_config) {

  span.setTag(Tracing::Tags::get().Component, Tracing::Tags::get().Proxy);

  if (stream_info.upstreamInfo() && stream_info.upstreamInfo()->upstreamHost()) {
    span.setTag(Tracing::Tags::get().UpstreamCluster,
                stream_info.upstreamInfo()->upstreamHost()->cluster().name());
    span.setTag(Tracing::Tags::get().UpstreamClusterName,
                stream_info.upstreamInfo()->upstreamHost()->cluster().observabilityName());
  }

  // Post response data.
  span.setTag(Tracing::Tags::get().HttpStatusCode, buildResponseCode(stream_info));
  span.setTag(Tracing::Tags::get().ResponseFlags,
              StreamInfo::ResponseFlagUtils::toShortString(stream_info));

  if (tracing_config.verbose()) {
    annotateVerbose(span, stream_info);
  }

  if (!stream_info.responseCode() || Http::CodeUtility::is5xx(stream_info.responseCode().value())) {
    span.setTag(Tracing::Tags::get().Error, Tracing::Tags::get().True);
  }

  CustomTagContext ctx{stream_info.getRequestHeaders(), stream_info};
  if (const CustomTagMap* custom_tag_map = tracing_config.customTags(); custom_tag_map) {
    for (const auto& it : *custom_tag_map) {
      it.second->applySpan(span, ctx);
    }
  }
}

HttpTracerImpl::HttpTracerImpl(DriverSharedPtr driver, const LocalInfo::LocalInfo& local_info)
    : driver_(std::move(driver)), local_info_(local_info) {}

SpanPtr HttpTracerImpl::startSpan(const Config& config, Http::RequestHeaderMap& request_headers,
                                  const StreamInfo::StreamInfo& stream_info,
                                  const Tracing::Decision tracing_decision) {
  std::string span_name = HttpTracerUtility::toString(config.operationName());

  if (config.operationName() == OperationName::Egress) {
    span_name.append(" ");
    span_name.append(std::string(request_headers.getHostValue()));
  }

  SpanPtr active_span = driver_->startSpan(config, request_headers, span_name,
                                           stream_info.startTime(), tracing_decision);

  // Set tags related to the local environment
  if (active_span) {
    active_span->setTag(Tracing::Tags::get().NodeId, local_info_.nodeName());
    active_span->setTag(Tracing::Tags::get().Zone, local_info_.zoneName());
  }

  return active_span;
}

} // namespace Tracing
} // namespace Envoy

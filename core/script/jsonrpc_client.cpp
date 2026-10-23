// Script-facing JSON-RPC 2.0 client. See include/berry/jsonrpc_client.h.
//
// The transport is the existing blocking HTTP path (`HttpRequestBlocking`), so
// the SSRF gate, the host grant and the redirect re-check are exactly the ones
// `network.post` uses. This unit only builds the request envelope and validates
// the response envelope.
#include "berry/jsonrpc_client.h"

#include "berry/async.h"
#include "berry/logger.h"
#include "berry/net.h"
#include "berry/url.h"

#include "json_min.h"
#include "json_write.h"

#include <atomic>
#include <cmath>
#include <string>

namespace Berry {

namespace {

using detail::json::Value;

/// Recursively converts a parsed JSON value into a ScriptValue. Depth is
/// bounded by the parser's own limit, so no extra guard is needed here.
ScriptValue Convert(const Value& v) {
  switch (v.type) {
    case detail::json::Type::Null:
      return ScriptValue::Nil();
    case detail::json::Type::Bool:
      return ScriptValue::Bool(v.boolean);
    case detail::json::Type::Number:
      return ScriptValue::Num(v.number);
    case detail::json::Type::String:
      return ScriptValue::Str(v.str);
    case detail::json::Type::Array: {
      ScriptTable t;
      for (size_t i = 0; i < v.array.size(); ++i) {
        t[std::to_string(i)] = Convert(v.array[i]);
      }
      return ScriptValue::Tbl(std::move(t));
    }
    case detail::json::Type::Object: {
      ScriptTable t;
      for (const auto& kv : v.object) t[kv.first] = Convert(kv.second);
      return ScriptValue::Tbl(std::move(t));
    }
  }
  return ScriptValue::Nil();
}

}  // namespace

ScriptValue JsonDocumentToScriptValue(const void* jsonValue) {
  if (!jsonValue) return ScriptValue::Nil();
  return Convert(*static_cast<const Value*>(jsonValue));
}

ScriptValue JsonTextToScriptValue(const std::string& text, std::string* error) {
  Value parsed;
  std::string perr;
  if (!detail::json::Parse(text, &parsed, &perr)) {
    if (error) *error = perr;
    return ScriptValue::Nil();
  }
  if (error) error->clear();
  return Convert(parsed);
}

JsonRpcOutcome JsonRpcCallBlocking(ScriptContext& ctx, const std::string& url,
                                   const std::string& method,
                                   const std::string& paramsJson,
                                   long timeoutMs, const UrlPolicy& policy) {
  JsonRpcOutcome out;
  if (method.empty()) {
    out.transport_error = "json_rpc.call: method is empty";
    return out;
  }

  // A fresh, monotonically increasing id per call so a reply can be matched.
  // The counter is process-wide and never reused, which is enough for a client
  // that issues one call at a time per script.
  static std::atomic<u64> nextId{1};
  const u64 id = nextId.fetch_add(1);
  const std::string idJson = std::to_string(id);

  std::string body = "{\"jsonrpc\":\"2.0\",\"id\":";
  body += idJson;
  body += ",\"method\":";
  detail::jsonw::String(&body, method);
  if (!paramsJson.empty()) {
    body += ",\"params\":";
    // The params are passed through as raw JSON, but they must be valid JSON or
    // the server will reject the whole frame; validate here so the failure is
    // attributed to the caller rather than to the transport.
    Value parsedParams;
    std::string perr;
    if (!detail::json::Parse(paramsJson, &parsedParams, &perr)) {
      out.transport_error = "json_rpc.call: bad params json: " + perr;
      return out;
    }
    body += paramsJson;
  }
  body += "}";

  // Reuse the network path: it applies the URL policy and host grant, follows
  // redirects with a re-check, and clamps the timeout.
  const ScriptValue resp = HttpRequestBlocking(
      ctx, "POST", url, body, timeoutMs, policy, 4u * 1024 * 1024);
  const ScriptTable* t = resp.AsTable();
  if (!t) {
    out.transport_error = "json_rpc.call: no response";
    return out;
  }
  auto num = [&](const char* k) -> f64 {
    const ScriptValue* f = resp.Field(k);
    return (f && f->IsNumber()) ? f->AsNumber() : 0;
  };
  auto str = [&](const char* k) -> std::string {
    const ScriptValue* f = resp.Field(k);
    return (f && f->IsString()) ? f->AsString() : std::string();
  };
  out.http_status = (int)num("status");
  const std::string transportErr = str("error");
  if (!transportErr.empty()) {
    out.transport_error = transportErr;
    return out;
  }
  if (out.http_status < 200 || out.http_status >= 300) {
    out.transport_error = "json_rpc.call: http " + std::to_string(out.http_status);
    return out;
  }

  const std::string responseBody = str("body");
  Value root;
  std::string perr;
  if (!detail::json::Parse(responseBody, &root, &perr)) {
    out.transport_error = "json_rpc.call: bad response json: " + perr;
    return out;
  }
  if (!root.IsObject()) {
    out.transport_error = "json_rpc.call: response is not an object";
    return out;
  }
  const Value* ver = root.Get("jsonrpc");
  if (!ver || !ver->IsString() || ver->str != "2.0") {
    out.transport_error = "json_rpc.call: not a json-rpc 2.0 response";
    return out;
  }
  const Value* rid = root.Get("id");
  if (!rid) {
    out.transport_error = "json_rpc.call: response has no id";
    return out;
  }
  // The id must match, or the reply is for someone else (or a spoof).
  bool idMatches = false;
  if (rid->IsString()) idMatches = rid->str == idJson;
  else if (rid->IsNumber()) idMatches = rid->number == (f64)id;
  if (!idMatches) {
    out.transport_error = "json_rpc.call: response id mismatch";
    return out;
  }

  if (const Value* err = root.Get("error")) {
    if (err->IsObject()) {
      if (const Value* code = err->Get("code")) {
        if (code->IsNumber()) out.error_code = (int)code->number;
      }
      if (const Value* msg = err->Get("message")) {
        if (msg->IsString()) out.error_message = msg->str;
      }
    }
    if (out.error_message.empty()) out.error_message = "json-rpc error";
    return out;
  }
  const Value* result = root.Get("result");
  if (!result) {
    out.transport_error = "json_rpc.call: response has no result";
    return out;
  }
  out.result_json = detail::jsonw::Dump(*result);
  out.ok = true;
  return out;
}

}  // namespace Berry

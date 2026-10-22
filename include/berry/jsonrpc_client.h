#pragma once
// Script-facing JSON-RPC 2.0 client.
//
// `Berry::Network` already carries a JSON-RPC 2.0 helper (`JsonRpc`), but it
// only builds and parses frames. This is the client a script uses: it issues a
// request over the same egress-checked HTTP path as `network.post` and returns
// the result as a structured table, so a script can call a remote method
// without parsing JSON itself.
//
// Two deliberate properties:
//
//   * The destination rules are the HTTP ones. A call goes through the same
//     URL policy and host grant as `network.get`, so JSON-RPC cannot become a
//     way around the SSRF gate.
//   * The response is validated as JSON-RPC 2.0 before it is trusted: a
//     mismatched id, a malformed envelope, or a non-JSON body is reported as an
//     error rather than a result, so a script never acts on a half-parsed
//     reply.
//
// This is a client-side, local-only helper: it makes an outbound request the
// script already had the grant to make, and it changes no game state.

#include "berry/export.h"
#include "berry/script.h"
#include "berry/types.h"
#include "berry/url.h"

#include <string>

namespace Berry {

/// A JSON-RPC outcome, mirroring the HttpResult shape so a script branches on
/// it the same way. `result_json` is the raw JSON of the `result` member (or
/// empty when the call failed); `error_code`/`error_message` carry a JSON-RPC
/// error, and `transport_error` a lower-level failure (bad URL, refused
/// destination, HTTP status, malformed envelope).
struct BERRY_API JsonRpcOutcome {
  bool ok = false;
  int error_code = 0;
  std::string error_message;
  std::string result_json;
  int http_status = 0;
  std::string transport_error;
};

/// Converts a parsed JSON document (a `detail::json::Value`) into a ScriptValue.
/// Objects become tables (string keys), arrays become tables keyed "0","1",…,
/// and null/bool/number/string map directly. The parameter is type-erased so
/// this header does not have to expose the internal JSON type; passing anything
/// that is not a JSON value pointer is a caller bug.
BERRY_API ScriptValue JsonDocumentToScriptValue(const void* jsonValue);

/// Parses `text` as one JSON document and converts it. Returns a Nil value and
/// sets `error` when the text is not valid JSON, so a caller can fail closed.
BERRY_API ScriptValue JsonTextToScriptValue(const std::string& text,
                                            std::string* error);

/// Runs one JSON-RPC 2.0 call over the blocking HTTP path:
///
///   POST url  {"jsonrpc":"2.0","id":<id>,"method":<method>,"params":<paramsJson>}
///
/// `paramsJson` may be empty (the `params` member is then omitted). The
/// response must be a JSON-RPC 2.0 object and its `id` must match. Returns the
/// outcome rather than raising, so the caller decides how to surface it.
///
/// `policy` is the same egress policy `network.post` uses (the harness relaxes
/// it for loopback); the host grant is checked inside the HTTP path.
BERRY_API JsonRpcOutcome JsonRpcCallBlocking(ScriptContext& ctx,
                                             const std::string& url,
                                             const std::string& method,
                                             const std::string& paramsJson,
                                             long timeoutMs,
                                             const UrlPolicy& policy);

}  // namespace Berry

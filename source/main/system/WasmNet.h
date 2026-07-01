/*
    This source file is part of Rigs of Rods
    For more information, see http://www.rigsofrods.org/

    Rigs of Rods is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3, as
    published by the Free Software Foundation.
*/

/// @file
/// @brief Web (emscripten) HTTP transport. Native builds use libcurl; on WebGL2
///        libcurl has no TLS backend and POSIX sockets have no transport, so the
///        browser's fetch/XHR (emscripten_fetch) is used instead.

#pragma once

#ifdef __EMSCRIPTEN__

#include <string>
#include <vector>

namespace RoR {

/// Synchronous HTTP GET via emscripten_fetch (browser XHR). MUST be called on a
/// worker thread (a sync fetch on the main/browser thread would deadlock); RoR's
/// repository code already runs on background std::threads. Returns the HTTP status
/// code (0 = transport failure, e.g. blocked by CORS). On success fills out_data.
long WasmHttpGet(const std::string& url, std::vector<char>& out_data);

/// Optional CORS-proxy prefix (cvar 'remote_cors_proxy'): browsers block cross-origin
/// fetches and RoR's API/forum don't send CORS headers, so requests to them must go
/// through a proxy that adds those headers. Returns proxy+url when a proxy is set,
/// else url unchanged. Same-origin URLs (the local build) need no proxy.
std::string WasmProxiedUrl(const std::string& url);

} // namespace RoR

#endif // __EMSCRIPTEN__

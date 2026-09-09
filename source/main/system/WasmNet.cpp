/*
    This source file is part of Rigs of Rods
    For more information, see http://www.rigsofrods.org/

    Rigs of Rods is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3, as
    published by the Free Software Foundation.
*/

#ifdef __EMSCRIPTEN__

#include "WasmNet.h"

#include "Application.h"
#include "GameContext.h"

#include <emscripten.h>
#include <emscripten/fetch.h>
#include <cstring>
#include <cctype>
#include <cstdlib>

namespace RoR {

long WasmHttpGet(const std::string& url, std::vector<char>& out_data)
{
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strcpy(attr.requestMethod, "GET");
    // LOAD_TO_MEMORY: response body kept in fetch->data. SYNCHRONOUS: block this
    // (worker) thread until done - a synchronous XHR, only legal off the main thread.
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;

    emscripten_fetch_t* fetch = emscripten_fetch(&attr, url.c_str());
    long status = 0;
    if (fetch)
    {
        status = fetch->status;
        if (fetch->numBytes > 0 && fetch->data)
        {
            out_data.assign(fetch->data, fetch->data + fetch->numBytes);
        }
        emscripten_fetch_close(fetch);
    }
    return status;
}

// Percent-encode a URL so it survives being carried inside another URL's query
// string. Unreserved set per RFC 3986; everything else (including '/', ':', '?'
// and '&') is escaped, which is what a "?url=" style proxy needs.
static std::string WasmUrlEncode(const std::string& s)
{
    static const char* HEX = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += static_cast<char>(c);
        }
        else
        {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 0x0F];
        }
    }
    return out;
}

std::string WasmProxiedUrl(const std::string& url)
{
    const std::string proxy = App::remote_cors_proxy->getStr();
    if (proxy.empty())
    {
        return url;
    }

    // Proxies come in two shapes and the target has to be escaped for one of them.
    // A repository download URL carries its own query ("...?file=31555"), so plain
    // concatenation onto a "?url=" proxy would hand that parameter to the proxy
    // instead of the forum.
    //   "https://host/{url}"     -> placeholder, encoded
    //   "https://host/?url="     -> trailing '=', encoded and appended
    //   "https://host/"          -> prefix style, appended verbatim
    const std::string placeholder = "{url}";
    const size_t at = proxy.find(placeholder);
    if (at != std::string::npos)
    {
        return proxy.substr(0, at) + WasmUrlEncode(url) + proxy.substr(at + placeholder.size());
    }
    if (!proxy.empty() && proxy.back() == '=')
    {
        return proxy + WasmUrlEncode(url);
    }
    return proxy + url;
}

} // namespace RoR

// --- Settings carried in the page URL ------------------------------------------------

// Read one query parameter from the page URL. Returns a malloc'd C string (empty
// when absent) that the caller frees.
EM_JS(char*, ror_wasm_query_param, (const char* key), {
    var name = UTF8ToString(key);
    var val = "";
    try { val = new URLSearchParams(location.search).get(name) || ""; } catch (e) {}
    var len = lengthBytesUTF8(val) + 1;
    var buf = _malloc(len);
    stringToUTF8(val, buf, len);
    return buf;
});

// Whether the parameter appears at all. "?cors_proxy=" (present but empty) means
// "no proxy", which is not the same as omitting it and taking the default.
EM_JS(bool, ror_wasm_query_param_present, (const char* key), {
    try { return new URLSearchParams(location.search).has(UTF8ToString(key)); }
    catch (e) { return false; }
});

namespace RoR {

// Default CORS proxy for the web build. forum.rigsofrods.org serves repository
// downloads without an Access-Control-Allow-Origin header, so the browser drops
// them; this proxy re-fetches server-side and adds the CORS + CORP headers. It is
// allowlisted to the two Rigs of Rods hosts, so it is not a general-purpose relay.
// The trailing '=' is significant: WasmProxiedUrl percent-encodes the target for
// proxies whose template ends in '=', which a download URL needs because it
// carries its own "?file=NNNN" query.
static const char* WASM_DEFAULT_CORS_PROXY = "https://cors.htdguide.com/?url=";

void WasmApplyUrlSettings()
{
    // Precedence: page URL > RoR.cfg > built-in default. There is no settings UI
    // for this cvar and web RoR.cfg lives in MEMFS (wiped on reload), so the page
    // URL is the only channel a visitor has:
    //   https://<host>/?cors_proxy=https://my-proxy.example/?url=
    // Pass an empty value (?cors_proxy=) to disable the proxy entirely.
    char* proxy = ror_wasm_query_param("cors_proxy");
    const bool have_param = (proxy != nullptr) && ror_wasm_query_param_present("cors_proxy");
    if (have_param)
    {
        App::remote_cors_proxy->setStr(proxy);
        LogFormat("[RoR|wasm] remote_cors_proxy from page URL: '%s'", proxy);
    }
    else if (App::remote_cors_proxy->getStr().empty())
    {
        App::remote_cors_proxy->setStr(WASM_DEFAULT_CORS_PROXY);
        LogFormat("[RoR|wasm] remote_cors_proxy defaulted to '%s'", WASM_DEFAULT_CORS_PROXY);
    }
    std::free(proxy);
}

} // namespace RoR

// --- Install mod from a browser-chosen file ----------------------------------------

// Called back from JS once the picked .zip has been written into /content. Rescans the
// mod cache so the new content appears (must run in the main menu; see main.cpp handler).
extern "C" EMSCRIPTEN_KEEPALIVE void ror_wasm_mod_installed(const char* name)
{
    RoR::LogFormat("[RoR|wasm] installed mod file '%s' - rescanning mod cache", name ? name : "");
    RoR::App::GetGameContext()->PushMessage(RoR::Message(RoR::MSG_APP_MODCACHE_UPDATE_REQUESTED));
}

// JS: pop a file picker, read the chosen .zip into MEMFS at /content/<name>, then call
// back into C to rescan. FS and ccall are exported (see CMakeLists EXPORTED_RUNTIME_METHODS).
EM_JS(void, ror_wasm_pick_mod, (), {
    var input = document.createElement('input');
    input.type = 'file';
    input.accept = '.zip';
    input.style.display = 'none';
    document.body.appendChild(input);
    input.onchange = function(e) {
        var file = (e.target.files && e.target.files[0]) || null;
        if (!file) { document.body.removeChild(input); return; }
        var reader = new FileReader();
        reader.onload = function() {
            try {
                try { FS.mkdir('/content'); } catch (ignore) {}
                FS.writeFile('/content/' + file.name, new Uint8Array(reader.result));
                Module.ccall('ror_wasm_mod_installed', null, ['string'], [file.name]);
            } catch (err) {
                console.error('[RoR] mod install failed:', err);
            }
            document.body.removeChild(input);
        };
        reader.readAsArrayBuffer(file);
    };
    input.click();
});

namespace RoR {
void WasmInstallModFromFile()
{
    ror_wasm_pick_mod();
}
} // namespace RoR

#endif // __EMSCRIPTEN__

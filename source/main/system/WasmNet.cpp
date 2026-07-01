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

std::string WasmProxiedUrl(const std::string& url)
{
    const std::string proxy = App::remote_cors_proxy->getStr();
    if (proxy.empty())
    {
        return url;
    }
    return proxy + url;
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

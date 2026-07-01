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

#endif // __EMSCRIPTEN__

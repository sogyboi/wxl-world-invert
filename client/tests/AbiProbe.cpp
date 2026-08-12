#include <windows.h>

#include <cstdio>

#include "wxl/PluginApi.h"

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: wxl-world-invert-abi-probe <wxl-world-invert.dll>\n");
        return 2;
    }

    HMODULE module = LoadLibraryA(argv[1]);
    if (module == nullptr) {
        std::fprintf(stderr, "LoadLibraryA failed: %lu\n", GetLastError());
        return 3;
    }
    const auto query = reinterpret_cast<WXL_QueryFn>(GetProcAddress(module, "WXL_Query"));
    if (query == nullptr) {
        std::fputs("WXL_Query export missing\n", stderr);
        FreeLibrary(module);
        return 4;
    }

    const WXL_PluginInfo* info = query();
    const bool valid = info != nullptr && info->structSize == sizeof(WXL_PluginInfo) &&
        info->apiVersion == WXL_API_VERSION && info->clientBuild == WXL_CLIENT_BUILD &&
        info->name != nullptr;
    std::printf("name=%s api=%u build=%u version=%u valid=%s\n",
                info != nullptr && info->name != nullptr ? info->name : "<null>",
                info != nullptr ? info->apiVersion : 0u,
                info != nullptr ? info->clientBuild : 0u,
                info != nullptr ? info->pluginVersion : 0u,
                valid ? "true" : "false");
    FreeLibrary(module);
    return valid ? 0 : 5;
}

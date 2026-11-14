// Manual check: prints the network name the profiles see, then watches for
// changes for a few seconds (unplug the cable / switch Wi-Fi to see it move).
//   archon_network_probe [seconds]

#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "network_watch.hpp"
#include "win_util.hpp"

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::string now = agent::to_utf8(agent::current_network_name());
    std::printf("network now: \"%s\"\n", now.c_str());
    const int secs = argc > 1 ? _wtoi(argv[1]) : 0;
    if (secs > 0) {
        agent::NetworkWatch w;
        w.start([](const std::string& n) { std::printf("changed: \"%s\"\n", n.c_str()); });
        Sleep(DWORD(secs) * 1000);
        w.stop();
    }
    CoUninitialize();
    return now.empty() ? 1 : 0;
}

#pragma once
// 1.6: battery of wireless audio devices (Bluetooth headphones, speakers,
// headsets). Windows keeps the level on the device's Plug and Play nodes —
// the same value Settings > Bluetooth & devices shows — and every node of one
// physical device shares a container id, as does its audio endpoint.

#include <windows.h>
#include <mmdeviceapi.h>

#include <map>
#include <string>

namespace agent {

// Container id of an audio endpoint ("{...}"; "" if unknown).
std::string endpoint_container(IMMDevice* dev);

// Battery levels (0..100) of every present device that reports one, by
// container id. Reads Plug and Play properties only (no radio traffic);
// takes tens of milliseconds, so callers refresh it about once a minute.
std::map<std::string, int> container_batteries();

}  // namespace agent

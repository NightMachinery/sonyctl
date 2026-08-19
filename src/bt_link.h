// Bluetooth baseband link control for this Mac only.
//
// This is a different layer from the MDR control channel the rest of sonyctl
// speaks: it connects/disconnects *this host's* Bluetooth link to the device,
// leaving the headphones' links to any other host (multipoint) untouched.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// All functions take a "AA:BB:CC:DD:EE:FF" address.
// Return: 1 = connected, 0 = not connected, -1 = device not found.
int btLinkIsConnected(const char* macAddress);

// Return 0 on success, non-zero IOReturn on failure, -1 if device not found.
int btLinkOpen(const char* macAddress);
int btLinkClose(const char* macAddress);

#ifdef __cplusplus
}
#endif

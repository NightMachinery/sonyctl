// sonyctl — observe and control Sony MDR-protocol headphones (WF-1000XM6 and
// other v2-era models) from the macOS command line.
//
// Built on libmdr / libmdr-bt from https://github.com/mos9527/SonyHeadphonesClient

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include <mdr-bt/ConnectionMacOS.h>
#include <mdr-c/Base.h>
#include <mdr-c/Connection.h>
#include <mdr-c/Headphones.h>
#include <mdr/Command.hpp>

#include "bt_link.h"

#ifndef SONYCTL_VERSION
#define SONYCTL_VERSION "unknown"
#endif

namespace {

struct Options {
    std::string mac;        // --mac override
    std::string nameFilter; // --device substring match
    std::string command;
    std::vector<std::string> args;
};

// Output mode, set once from the command line.
bool gJson = false;      // --json: machine-readable stdout
bool gColorErr = false;  // colorize stderr (gray), per --color and isatty

// Diagnostics always go to stderr so stdout stays parseable in both modes.
void errf(const char* fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    // Callers keep a trailing newline for readability; strip it so the message
    // can be embedded in JSON, and re-add it when printing.
    size_t len = std::strlen(msg);
    while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
        msg[--len] = '\0';

    const char* on = gColorErr ? "\033[90m" : "";
    const char* off = gColorErr ? "\033[0m" : "";
    if (gJson) {
        std::string escaped;
        for (const char* p = msg; *p; ++p) {
            if (*p == '"' || *p == '\\')
                escaped += '\\', escaped += *p;
            else if (static_cast<unsigned char>(*p) < 0x20)
                escaped += ' ';
            else
                escaped += *p;
        }
        std::fprintf(stderr, "%s{\"error\":\"%s\"}%s\n", on, escaped.c_str(), off);
    } else {
        std::fprintf(stderr, "%s%s%s\n", on, msg, off);
    }
}

// Minimal JSON object builder — enough for this tool's flat-ish output.
class JsonObj {
    std::string body;
    void sep() { if (!body.empty()) body += ','; }

public:
    static std::string escape(const std::string& in)
    {
        std::string out;
        for (char c : in) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += ' ';
                else
                    out += c;
            }
        }
        return out;
    }
    JsonObj& str(const char* key, const std::string& value)
    {
        sep();
        body += '"' + std::string(key) + "\":\"" + escape(value) + '"';
        return *this;
    }
    JsonObj& num(const char* key, long long value)
    {
        sep();
        body += '"' + std::string(key) + "\":" + std::to_string(value);
        return *this;
    }
    JsonObj& boolean(const char* key, bool value)
    {
        sep();
        body += '"' + std::string(key) + "\":" + (value ? "true" : "false");
        return *this;
    }
    // Insert an already-serialized JSON value (object, array, literal).
    JsonObj& raw(const char* key, const std::string& value)
    {
        sep();
        body += '"' + std::string(key) + "\":" + value;
        return *this;
    }
    std::string dump() const { return "{" + body + "}"; }
};

void printJson(const JsonObj& obj)
{
    std::printf("%s\n", obj.dump().c_str());
}

int usage(FILE* out)
{
    std::fprintf(out,
        "usage: sonyctl [options] <command> [args]\n"
        "\n"
        "options:\n"
        "  --mac AA:BB:CC:DD:EE:FF   target a specific device\n"
        "  --device SUBSTR           target the first device whose name matches\n"
        "  --json                    machine-readable JSON on stdout\n"
        "  --color=auto|always|never colorize diagnostics (default auto)\n"
        "\n"
        "Diagnostics always go to stderr, so stdout stays parseable.\n"
        "\n"
        "commands:\n"
        "  devices     list paired Bluetooth devices\n"
        "  connect     connect this Mac's Bluetooth link to the headphones\n"
        "  disconnect  drop this Mac's link only (other hosts stay connected)\n"
        "  link        report whether this Mac is linked to the headphones\n"
        "  status      show model, firmware, noise mode, battery\n"
        "  mode        print current noise mode (off | nc | ambient)\n"
        "  mode MODE   set the noise mode\n"
        "  nc          noise cancelling      (aliases: anc, noise-cancelling)\n"
        "  ambient     ambient sound         (aliases: transparency, passthrough)\n"
        "              flags: --level 1-20, --voice-focus on|off\n"
        "  off         no noise control      (alias: normal)\n"
        "  battery     show battery levels\n"
        "  multipoint  show multipoint state; 'multipoint on|off|reset' to change\n"
        "              (reset = off then on; recovers buggy audio states in place)\n"
        "  power-off   shut the headphones down (wake requires case/wear;\n"
        "              the protocol has no remote power-on)\n"
        "  raw HEX [--listen SECS]\n"
        "              send a raw MDR payload (hex) and hex-dump all frames\n"
        "              (e.g. 'raw 6617 --listen 3' = NCASM get param)\n"
        "  version     print the sonyctl version\n"
        "  help        show this help\n");
    return out == stderr ? 2 : 0;
}

// Default device patterns tried in order when neither --mac nor --device is given.
const char* const kDefaultDevicePatterns[] = {"WF-1000XM6", "WF-1000XM", "WH-1000XM",
                                              "LinkBuds", "ULT WEAR", "WF-", "WH-"};

struct ConnectionHolder {
    MDRConnectionMacOS* platform = nullptr;
    MDRConnection* conn = nullptr;

    ConnectionHolder()
    {
        platform = mdrConnectionMacOSCreate();
        if (platform)
            conn = mdrConnectionMacOSGet(platform);
    }
    ~ConnectionHolder()
    {
        if (conn)
            mdrConnectionDisconnect(conn);
        if (platform)
            mdrConnectionMacOSDestroy(platform);
    }
    ConnectionHolder(const ConnectionHolder&) = delete;
    ConnectionHolder& operator=(const ConnectionHolder&) = delete;
};

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Pick the target device: --mac wins, then --device substring, then the
// default Sony name patterns.
bool pickDevice(MDRConnection* conn, const Options& opt, std::string& outMac, std::string& outName)
{
    if (!opt.mac.empty()) {
        outMac = opt.mac;
        outName = opt.mac;
        return true;
    }
    MDRDeviceInfo* list = nullptr;
    int count = 0;
    if (mdrConnectionGetDevicesList(conn, &list, &count) != MDR_RESULT_OK || count == 0) {
        errf("sonyctl: no paired Bluetooth devices found\n");
        return false;
    }
    int found = -1;
    if (!opt.nameFilter.empty()) {
        for (int i = 0; i < count && found < 0; ++i)
            if (std::strstr(list[i].szDeviceName, opt.nameFilter.c_str()))
                found = i;
        if (found < 0)
            errf("sonyctl: no paired device matching '%s'\n", opt.nameFilter.c_str());
    } else {
        for (const char* pattern : kDefaultDevicePatterns) {
            for (int i = 0; i < count && found < 0; ++i)
                if (std::strstr(list[i].szDeviceName, pattern))
                    found = i;
            if (found >= 0)
                break;
        }
        if (found < 0)
            errf("sonyctl: no Sony headphone found among paired devices "
                 "(try --device SUBSTR or --mac)");
    }
    if (found >= 0) {
        outMac = list[found].szDeviceMacAddress;
        outName = list[found].szDeviceName;
    }
    mdrConnectionFreeDevicesList(conn, &list);
    return found >= 0;
}

// Full connect + protocol handshake + initial state sync. On success the
// returned MDRHeadphones* is initialized and synced.
class Session {
public:
    MDRConnection* conn = nullptr;
    MDRHeadphones* dev = nullptr;

    ~Session()
    {
        if (dev)
            mdrHeadphonesDestroy(dev);
    }

    // The handshake fails transiently for a few seconds after the link is
    // established, after another MDR session closes, or when the device is busy
    // (upstream then logs "ACK Timeout" and gives up). Retry the whole session
    // rather than surfacing that to the user.
    bool open(MDRConnection* connection, const Options& opt, int attempts = 3)
    {
        conn = connection;
        std::string mac, name;
        if (!pickDevice(conn, opt, mac, name))
            return false; // already reported

        std::string err;
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            if (openOnce(mac, name, 8000, err))
                return true;
            if (dev) {
                mdrHeadphonesDestroy(dev);
                dev = nullptr;
            }
            mdrConnectionDisconnect(conn);
            if (attempt < attempts)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        errf("sonyctl: %s after %d attempts (is another app using the headphones, "
             "e.g. Sound Connect on a phone?)",
             err.c_str(), attempts);
        return false;
    }

private:
    bool openOnce(const std::string& mac, const std::string& name, int timeoutMs,
                  std::string& err)
    {
        // v2 UUID first, legacy second (mirrors upstream's AUTO mode). Retried:
        // the SDP record is briefly unavailable right after a previous session
        // disconnects.
        const char* const services[] = {MDR_SERVICE_UUID_XM5, MDR_SERVICE_UUID_LEGACY};
        MDRResult r = MDR_RESULT_ERROR_NO_CONNECTION;
        const int64_t connectDeadline = nowMs() + timeoutMs;
        for (int attempt = 0; r != MDR_RESULT_OK && nowMs() < connectDeadline; ++attempt) {
            if (attempt > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            for (const char* uuid : services) {
                r = mdrConnectionConnect(conn, mac.c_str(), uuid);
                if (r == MDR_RESULT_OK || r == MDR_RESULT_INPROGRESS) {
                    while (r != MDR_RESULT_OK && nowMs() < connectDeadline) {
                        r = mdrConnectionPoll(conn, 50);
                        if (r != MDR_RESULT_OK && r != MDR_RESULT_INPROGRESS &&
                            r != MDR_RESULT_ERROR_TIMEOUT)
                            break;
                        if (r == MDR_RESULT_ERROR_TIMEOUT)
                            r = MDR_RESULT_INPROGRESS;
                    }
                }
                if (r == MDR_RESULT_OK)
                    break;
                mdrConnectionDisconnect(conn);
            }
        }
        if (r != MDR_RESULT_OK) {
            err = "cannot connect to " + name + ": " + mdrConnectionGetLastError(conn);
            return false;
        }

        if (mdrHeadphonesCreate(MDR_ABI_VERSION, conn, &dev) != MDR_RESULT_OK) {
            err = "failed to create headphones instance";
            return false;
        }
        if (mdrHeadphonesRequestInit(dev) != MDR_RESULT_OK ||
            !waitEvent(MDR_EVENT_INITIALIZE_COMPLETE, timeoutMs)) {
            err = "protocol init failed or timed out";
            return false;
        }
        if (mdrHeadphonesRequestFetch(dev) != MDR_RESULT_OK ||
            !waitEvent(MDR_EVENT_SYNC_COMPLETE, timeoutMs)) {
            err = "state sync failed or timed out";
            return false;
        }
        return true;
    }

public:

    // Pump the event loop until `target` arrives.
    bool waitEvent(MDREvent target, int timeoutMs = 10000)
    {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            MDREvent event = MDR_EVENT_NONE;
            const MDRResult pr = mdrHeadphonesPoll(dev, &event);
            if (pr != MDR_RESULT_OK)
                return false; // caller decides whether to retry or report
            if (event == target)
                return true;
            if (event == MDR_EVENT_NONE)
                mdrConnectionPoll(conn, 20); // idle-wait; pumps the run loop
        }
        return false;
    }

    // Commit staged changes and wait until applied.
    bool commit(int timeoutMs = 10000)
    {
        if (!mdrHeadphonesIsDirty(dev))
            return true;
        const int64_t deadline = nowMs() + timeoutMs;
        while (!mdrHeadphonesIsReady(dev) && nowMs() < deadline) {
            MDREvent event = MDR_EVENT_NONE;
            if (mdrHeadphonesPoll(dev, &event) != MDR_RESULT_OK)
                return false;
            if (event == MDR_EVENT_NONE)
                mdrConnectionPoll(conn, 20);
        }
        if (mdrHeadphonesRequestCommit(dev) != MDR_RESULT_OK) {
            errf("sonyctl: commit failed\n");
            return false;
        }
        return waitEvent(MDR_EVENT_APPLY_COMPLETE, timeoutMs);
    }

    std::string text(MDRText what, uint32_t index = 0)
    {
        char buf[256];
        uint32_t size = sizeof buf;
        if (mdrHeadphonesGetText(dev, what, index, buf, &size) != MDR_RESULT_OK)
            return {};
        return std::string(buf);
    }
};

const char* noiseModeName(MDRNoiseMode mode)
{
    switch (mode) {
    case MDR_NOISE_MODE_CANCELLING: return "nc";
    case MDR_NOISE_MODE_AMBIENT: return "ambient";
    default: return "off";
    }
}

JsonObj noiseControlJson(const MDRNoiseControl& nc)
{
    JsonObj obj;
    obj.str("mode", noiseModeName(nc.mode))
        .num("ambient_level", nc.ambient_level)
        .boolean("focus_on_voice", nc.focus_on_voice != MDR_FALSE)
        .boolean("adaptive_ambient", nc.adaptive_ambient != MDR_FALSE)
        .num("adaptive_sensitivity", nc.adaptive_sensitivity);
    return obj;
}

void printNoiseControl(const MDRNoiseControl& nc)
{
    if (gJson) {
        printJson(noiseControlJson(nc));
        return;
    }
    std::printf("mode: %s\n", noiseModeName(nc.mode));
    if (nc.mode == MDR_NOISE_MODE_AMBIENT) {
        std::printf("ambient level: %u\n", nc.ambient_level);
        std::printf("focus on voice: %s\n", nc.focus_on_voice ? "yes" : "no");
    }
    if (nc.adaptive_ambient)
        std::printf("adaptive ambient: on (sensitivity %u)\n", nc.adaptive_sensitivity);
}

const char* batteryPartName(MDRBatteryPart part)
{
    switch (part) {
    case MDR_BATTERY_LEFT: return "left";
    case MDR_BATTERY_RIGHT: return "right";
    case MDR_BATTERY_CASE: return "case";
    default: return "battery";
    }
}

const char* chargingName(MDRChargingState state)
{
    switch (state) {
    case MDR_CHARGING_NO: return "no";
    case MDR_CHARGING_YES: return "yes";
    case MDR_CHARGING_COMPLETE: return "complete";
    default: return "unknown";
    }
}

// Serialized JSON array of the present batteries.
std::string batteriesJson(Session& s)
{
    MDRBattery batteries[4];
    uint32_t count = 4;
    if (mdrHeadphonesGetBatteries(s.dev, batteries, &count) != MDR_RESULT_OK)
        return "[]";
    std::string out = "[";
    bool first = true;
    for (uint32_t i = 0; i < count; ++i) {
        const MDRBattery& b = batteries[i];
        if (!b.present)
            continue;
        JsonObj obj;
        obj.str("part", batteryPartName(b.part))
            .num("level_percent", b.level_percent)
            .str("charging", chargingName(b.charging));
        if (!first)
            out += ',';
        out += obj.dump();
        first = false;
    }
    return out + "]";
}

void printBatteries(Session& s)
{
    if (gJson) {
        JsonObj obj;
        obj.raw("batteries", batteriesJson(s));
        printJson(obj);
        return;
    }
    MDRBattery batteries[4];
    uint32_t count = 4;
    if (mdrHeadphonesGetBatteries(s.dev, batteries, &count) != MDR_RESULT_OK)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        const MDRBattery& b = batteries[i];
        if (!b.present)
            continue;
        std::printf("%s: %u%%%s\n", batteryPartName(b.part), b.level_percent,
                    b.charging == MDR_CHARGING_YES        ? " (charging)"
                    : b.charging == MDR_CHARGING_COMPLETE ? " (charged)"
                                                          : "");
    }
}

int cmdStatus(Session& s)
{
    const std::string model = s.text(MDR_TEXT_MODEL_NAME);
    const std::string fw = s.text(MDR_TEXT_FIRMWARE_VERSION);
    if (gJson) {
        JsonObj obj;
        obj.str("model", model).str("firmware", fw);
        MDRNoiseControl nc{};
        if (mdrHeadphonesGetNoiseControl(s.dev, &nc) == MDR_RESULT_OK)
            obj.raw("noise", noiseControlJson(nc).dump());
        obj.raw("batteries", batteriesJson(s));
        printJson(obj);
        return 0;
    }
    if (!model.empty())
        std::printf("model: %s\n", model.c_str());
    if (!fw.empty())
        std::printf("firmware: %s\n", fw.c_str());
    MDRNoiseControl nc{};
    if (mdrHeadphonesGetNoiseControl(s.dev, &nc) == MDR_RESULT_OK)
        printNoiseControl(nc);
    printBatteries(s);
    return 0;
}

int cmdMode(Session& s)
{
    MDRNoiseControl nc{};
    if (mdrHeadphonesGetNoiseControl(s.dev, &nc) != MDR_RESULT_OK) {
        errf("sonyctl: noise control not available\n");
        return 1;
    }
    if (gJson) {
        JsonObj obj;
        obj.str("mode", noiseModeName(nc.mode));
        printJson(obj);
        return 0;
    }
    std::printf("%s\n", noiseModeName(nc.mode));
    return 0;
}

// Canonicalize a noise-mode word (with aliases); empty result = not a mode.
std::string canonicalMode(const std::string& word)
{
    if (word == "nc" || word == "anc" || word == "noise-cancelling" || word == "noise-canceling")
        return "nc";
    if (word == "ambient" || word == "transparency" || word == "passthrough" || word == "asm")
        return "ambient";
    if (word == "off" || word == "normal" || word == "none")
        return "off";
    return {};
}

struct ModeFlags {
    int level = -1;          // --level 1-20 (ambient only)
    int voiceFocus = -1;     // --voice-focus on|off
};

int cmdSetMode(Session& s, const std::string& mode, const ModeFlags& flags)
{
    MDRNoiseControl nc{};
    if (mdrHeadphonesGetNoiseControl(s.dev, &nc) != MDR_RESULT_OK) {
        errf("sonyctl: noise control not available\n");
        return 1;
    }
    if (mode == "nc")
        nc.mode = MDR_NOISE_MODE_CANCELLING;
    else if (mode == "ambient")
        nc.mode = MDR_NOISE_MODE_AMBIENT;
    else
        nc.mode = MDR_NOISE_MODE_OFF;
    if (flags.level >= 0)
        nc.ambient_level = static_cast<uint8_t>(flags.level);
    if (flags.voiceFocus >= 0)
        nc.focus_on_voice = flags.voiceFocus ? MDR_TRUE : MDR_FALSE;

    if (mdrHeadphonesSetNoiseControl(s.dev, &nc) != MDR_RESULT_OK) {
        errf("sonyctl: setting noise control failed\n");
        return 1;
    }
    if (!s.commit()) {
        errf("sonyctl: change was not applied\n");
        return 1;
    }
    MDRNoiseControl applied{};
    if (mdrHeadphonesGetNoiseControl(s.dev, &applied) == MDR_RESULT_OK)
        printNoiseControl(applied);
    return 0;
}

// Locate the multipoint General Setting ("Connect to 2 devices simultaneously",
// subject string MULTIPOINT_SETTING). Returns the setting index, or -1.
int findMultipointSetting(Session& s)
{
    MDRGeneralSettingInfo infos[16];
    uint32_t count = 16;
    if (mdrHeadphonesGetGeneralSettingInfo(s.dev, infos, &count) != MDR_RESULT_OK)
        return -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (infos[i].type != MDR_GENERAL_SETTING_BOOLEAN)
            continue;
        const std::string subject = s.text(MDR_TEXT_GENERAL_SETTING_SUBJECT, infos[i].index);
        if (subject.find("MULTIPOINT") != std::string::npos)
            return static_cast<int>(infos[i].index);
    }
    return -1;
}

bool setMultipoint(Session& s, uint32_t index, bool enabled)
{
    MDRGeneralSetting setting{};
    setting.index = index;
    setting.boolean_value = enabled ? MDR_TRUE : MDR_FALSE;
    if (mdrHeadphonesSetGeneralSetting(s.dev, &setting) != MDR_RESULT_OK) {
        errf("sonyctl: setting multipoint failed\n");
        return false;
    }
    if (!s.commit()) {
        errf("sonyctl: multipoint change was not applied\n");
        return false;
    }
    return true;
}

int cmdMultipoint(Session& s, const std::string& action)
{
    const int index = findMultipointSetting(s);
    if (index < 0) {
        errf("sonyctl: no multipoint setting found on this device\n");
        return 1;
    }
    MDRGeneralSetting current{};
    if (mdrHeadphonesGetGeneralSetting(s.dev, static_cast<uint32_t>(index), &current) !=
        MDR_RESULT_OK) {
        errf("sonyctl: cannot read multipoint state\n");
        return 1;
    }
    const bool was = current.boolean_value != MDR_FALSE;
    if (action.empty()) {
        if (gJson) {
            JsonObj obj;
            obj.boolean("multipoint", was);
            printJson(obj);
        } else {
            std::printf("multipoint: %s\n", was ? "on" : "off");
        }
        return 0;
    }
    if (action == "on" || action == "off") {
        const bool want = action == "on";
        if (!setMultipoint(s, static_cast<uint32_t>(index), want))
            return 1;
        if (gJson) {
            JsonObj obj;
            obj.boolean("multipoint", want).boolean("previous", was).str("action", action);
            printJson(obj);
        } else {
            std::printf("multipoint: %s\n", action.c_str());
        }
        return 0;
    }
    if (action == "reset") {
        if (!gJson) {
            std::printf("multipoint: %s -> off", was ? "on" : "off");
            std::fflush(stdout);
        }
        if (!setMultipoint(s, static_cast<uint32_t>(index), false))
            return 1;
        if (!gJson) {
            std::printf(" -> on");
            std::fflush(stdout);
        }
        if (!setMultipoint(s, static_cast<uint32_t>(index), true))
            return 1;
        if (gJson) {
            JsonObj obj;
            obj.boolean("multipoint", true).boolean("previous", was).str("action", "reset");
            printJson(obj);
        } else {
            std::printf(" (reset done)\n");
        }
        return 0;
    }
    errf("sonyctl: multipoint takes on, off, or reset\n");
    return 2;
}

int cmdPowerOff(Session& s)
{
    MDRPower power{};
    if (mdrHeadphonesGetPower(s.dev, &power) != MDR_RESULT_OK) {
        errf("sonyctl: power control not available\n");
        return 1;
    }
    power.shutdown_requested = MDR_TRUE;
    if (mdrHeadphonesSetPower(s.dev, &power) != MDR_RESULT_OK) {
        errf("sonyctl: power-off request failed\n");
        return 1;
    }
    // The device drops the link while applying; a failed commit-wait here
    // just means the shutdown won the race.
    s.commit(5000);
    if (gJson) {
        JsonObj obj;
        obj.boolean("power_off_sent", true);
        printJson(obj);
    } else {
        std::printf("power-off sent\n");
    }
    return 0;
}

bool parseHex(const std::string& in, std::vector<uint8_t>& out)
{
    std::string s;
    for (char c : in)
        if (c != ' ' && c != ':' && c != '_')
            s.push_back(c);
    if (s.empty() || s.size() % 2 != 0)
        return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = nibble(s[i]), lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return true;
}

// In JSON mode frames are collected and emitted as one object at the end;
// in human mode they stream out as they arrive.
void dumpFrame(void* user, MDRPacketDirection dir, const unsigned char* frame, int size)
{
    std::string hex;
    char byte[3];
    for (int i = 0; i < size; ++i) {
        std::snprintf(byte, sizeof byte, "%02x", frame[i]);
        hex += byte;
    }
    const char* dirName = dir == MDR_PACKET_DIRECTION_TX ? "tx" : "rx";
    if (gJson) {
        auto* frames = static_cast<std::vector<std::string>*>(user);
        JsonObj obj;
        obj.str("dir", dirName).str("hex", hex);
        frames->push_back(obj.dump());
        return;
    }
    std::printf("%s %s\n", dir == MDR_PACKET_DIRECTION_TX ? "TX" : "RX", hex.c_str());
    std::fflush(stdout);
}

int cmdRaw(Session& s, const std::string& hex, int listenSecs)
{
    std::vector<uint8_t> payload;
    if (!parseHex(hex, payload)) {
        errf("sonyctl: raw payload must be an even-length hex string\n");
        return 2;
    }
    std::vector<std::string> frames;
    mdrHeadphonesSetPacketCallback(s.dev, dumpFrame, &frames);

    // Frame as an MDR data command and inject on the wire. Sequence numbering is
    // owned by the library's own sends; this probe path shares the channel, so a
    // seq clash is possible — fine for observation, which is the point of `raw`.
    // The injected frame bypasses the library's send path, so it is not echoed by
    // the packet callback; the device's reply and all other traffic still are.
    if (!payload.empty()) {
        mdr::MDRBuffer packed = mdr::MDRPackCommand(
            mdr::MDRDataType::DATA_MDR, 0,
            mdr::Span<const mdr::UInt8>(payload.data(), payload.size()));
        int sent = 0;
        const MDRResult r = mdrConnectionSend(
            s.conn, reinterpret_cast<const char*>(packed.data()),
            static_cast<int>(packed.size()), &sent);
        if (r != MDR_RESULT_OK && r != MDR_RESULT_INPROGRESS)
            errf("sonyctl: raw send failed: %s\n", mdrResultString(r));
    }

    const int64_t deadline = nowMs() + static_cast<int64_t>(listenSecs) * 1000;
    while (nowMs() < deadline) {
        MDREvent event = MDR_EVENT_NONE;
        if (mdrHeadphonesPoll(s.dev, &event) != MDR_RESULT_OK)
            break;
        if (event == MDR_EVENT_NONE)
            mdrConnectionPoll(s.conn, 50);
    }
    mdrHeadphonesSetPacketCallback(s.dev, nullptr, nullptr);
    if (gJson) {
        std::string arr = "[";
        for (size_t i = 0; i < frames.size(); ++i)
            arr += (i ? "," : "") + frames[i];
        arr += "]";
        JsonObj obj;
        obj.raw("frames", arr);
        printJson(obj);
    }
    return 0;
}

// connect / disconnect / link-status operate on this Mac's Bluetooth link only;
// they do not need (and must not hold) an MDR control session.
int cmdLink(MDRConnection* conn, const Options& opt, const std::string& action)
{
    std::string mac, name;
    if (!pickDevice(conn, opt, mac, name))
        return 1;

    const int before = btLinkIsConnected(mac.c_str());
    if (before < 0) {
        errf("sonyctl: device %s not found", mac.c_str());
        return 1;
    }
    if (action == "link") {
        if (gJson) {
            JsonObj obj;
            obj.str("device", name).str("mac", mac).boolean("connected", before == 1);
            printJson(obj);
        } else {
            std::printf("%s: %s\n", name.c_str(), before == 1 ? "connected" : "disconnected");
        }
        return 0;
    }

    const bool want = action == "connect";
    const int rc = want ? btLinkOpen(mac.c_str()) : btLinkClose(mac.c_str());
    if (rc != 0) {
        errf("sonyctl: %s failed for %s (IOReturn 0x%x)", action.c_str(), name.c_str(), rc);
        return 1;
    }
    // The link state settles asynchronously; poll briefly so we report reality.
    int now = before;
    const int64_t deadline = nowMs() + 5000;
    while (nowMs() < deadline) {
        now = btLinkIsConnected(mac.c_str());
        if ((now == 1) == want)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if ((now == 1) != want) {
        errf("sonyctl: %s did not take effect for %s", action.c_str(), name.c_str());
        return 1;
    }

    if (gJson) {
        JsonObj obj;
        obj.str("device", name)
            .str("mac", mac)
            .boolean("connected", now == 1)
            .boolean("previously_connected", before == 1)
            .str("action", action);
        printJson(obj);
    } else {
        std::printf("%s: %s%s\n", name.c_str(), now == 1 ? "connected" : "disconnected",
                    (before == 1) == want ? " (already)" : "");
    }
    return 0;
}

int cmdDevices(MDRConnection* conn)
{
    MDRDeviceInfo* list = nullptr;
    int count = 0;
    const MDRResult r = mdrConnectionGetDevicesList(conn, &list, &count);
    if (r != MDR_RESULT_OK) {
        errf("sonyctl: cannot list devices: %s\n", mdrResultString(r));
        return 1;
    }
    if (gJson) {
        std::string arr = "[";
        for (int i = 0; i < count; ++i) {
            JsonObj obj;
            obj.str("mac", list[i].szDeviceMacAddress).str("name", list[i].szDeviceName);
            arr += (i ? "," : "") + obj.dump();
        }
        arr += "]";
        JsonObj obj;
        obj.raw("devices", arr);
        printJson(obj);
    } else {
        for (int i = 0; i < count; ++i)
            std::printf("%s  %s\n", list[i].szDeviceMacAddress, list[i].szDeviceName);
    }
    mdrConnectionFreeDevicesList(conn, &list);
    if (count == 0)
        errf("sonyctl: no paired Bluetooth devices found\n");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    std::string colorMode = "auto"; // auto | always | never
    bool wantHelp = false, wantVersion = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                errf("sonyctl: %s requires a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--mac")
            opt.mac = needValue("--mac");
        else if (arg == "--device")
            opt.nameFilter = needValue("--device");
        else if (arg == "--json")
            gJson = true;
        else if (arg.rfind("--color=", 0) == 0)
            colorMode = arg.substr(8);
        else if (arg == "--color")
            colorMode = needValue("--color");
        else if (arg == "-h" || arg == "--help" || arg == "help")
            wantHelp = true;
        else if (arg == "-V" || arg == "--version" || arg == "version")
            wantVersion = true;
        else if (opt.command.empty())
            opt.command = arg;
        else
            opt.args.push_back(arg);
    }

    // Resolve output modes before anything can print.
    if (colorMode == "auto")
        gColorErr = isatty(fileno(stderr)) != 0;
    else if (colorMode == "always")
        gColorErr = true;
    else if (colorMode == "never")
        gColorErr = false;
    else {
        errf("sonyctl: --color takes auto, always, or never");
        return 2;
    }

    if (wantHelp)
        return usage(stdout);
    if (wantVersion) {
        if (gJson) {
            JsonObj obj;
            obj.str("version", SONYCTL_VERSION);
            printJson(obj);
        } else {
            std::printf("sonyctl %s\n", SONYCTL_VERSION);
        }
        return 0;
    }
    if (opt.command.empty())
        return usage(stderr);

    ConnectionHolder holder;
    if (!holder.conn) {
        errf("sonyctl: failed to create Bluetooth connection backend\n");
        return 1;
    }

    if (opt.command == "devices")
        return cmdDevices(holder.conn);
    if (opt.command == "connect" || opt.command == "disconnect" || opt.command == "link")
        return cmdLink(holder.conn, opt, opt.command);

    // Resolve `mode <MODE>` and top-level mode shortcuts (nc/ambient/off + aliases).
    std::string setMode;
    ModeFlags modeFlags;
    std::vector<std::string> rest = opt.args;
    if (opt.command == "mode" && !rest.empty()) {
        setMode = canonicalMode(rest.front());
        if (setMode.empty()) {
            errf("sonyctl: unknown mode '%s'\n", rest.front().c_str());
            return 2;
        }
        rest.erase(rest.begin());
    } else if (opt.command != "mode") {
        setMode = canonicalMode(opt.command);
    }
    for (size_t i = 0; !setMode.empty() && i < rest.size(); ++i) {
        const std::string& arg = rest[i];
        auto flagValue = [&](const char* flag) -> std::string {
            if (i + 1 >= rest.size()) {
                errf("sonyctl: %s requires a value\n", flag);
                std::exit(2);
            }
            return rest[++i];
        };
        if (arg == "--level") {
            modeFlags.level = std::atoi(flagValue("--level").c_str());
            if (modeFlags.level < 1 || modeFlags.level > 20) {
                errf("sonyctl: --level must be 1-20\n");
                return 2;
            }
        } else if (arg == "--voice-focus") {
            const std::string v = flagValue("--voice-focus");
            modeFlags.voiceFocus = (v == "on" || v == "yes" || v == "1") ? 1 : 0;
        } else {
            errf("sonyctl: unknown argument '%s'\n", arg.c_str());
            return 2;
        }
    }

    // raw <hex> [--listen SECS]
    std::string rawHex;
    int rawListen = 3;
    if (opt.command == "raw") {
        if (opt.args.empty()) {
            errf("sonyctl: raw requires a hex payload\n");
            return 2;
        }
        rawHex = opt.args.front();
        for (size_t i = 1; i < opt.args.size(); ++i) {
            if (opt.args[i] == "--listen" && i + 1 < opt.args.size())
                rawListen = std::atoi(opt.args[++i].c_str());
            else {
                errf("sonyctl: unknown raw argument '%s'\n", opt.args[i].c_str());
                return 2;
            }
        }
    }

    const bool knownCommand = opt.command == "status" || opt.command == "mode" ||
                              opt.command == "battery" || opt.command == "multipoint" ||
                              opt.command == "power-off" || opt.command == "raw" ||
                              !setMode.empty();
    if (!knownCommand) {
        errf("sonyctl: unknown command '%s'\n", opt.command.c_str());
        return usage(stderr);
    }

    Session session;
    if (!session.open(holder.conn, opt))
        return 1;

    if (!setMode.empty())
        return cmdSetMode(session, setMode, modeFlags);
    if (opt.command == "status")
        return cmdStatus(session);
    if (opt.command == "mode")
        return cmdMode(session);
    if (opt.command == "battery") {
        printBatteries(session);
        return 0;
    }
    if (opt.command == "multipoint")
        return cmdMultipoint(session, opt.args.empty() ? std::string() : opt.args.front());
    if (opt.command == "power-off")
        return cmdPowerOff(session);
    if (opt.command == "raw")
        return cmdRaw(session, rawHex, rawListen);
    return usage(stderr);
}

// sonyctl — observe and control Sony MDR-protocol headphones (WF-1000XM6 and
// other v2-era models) from the macOS command line.
//
// Built on libmdr / libmdr-bt from https://github.com/mos9527/SonyHeadphonesClient

#include <chrono>
#include <cstdio>
#include <thread>
#include <cstring>
#include <string>
#include <vector>

#include <mdr-bt/ConnectionMacOS.h>
#include <mdr-c/Base.h>
#include <mdr-c/Connection.h>
#include <mdr-c/Headphones.h>
#include <mdr/Command.hpp>

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

int usage(FILE* out)
{
    std::fprintf(out,
        "usage: sonyctl [--mac AA:BB:CC:DD:EE:FF] [--device SUBSTR] <command> [args]\n"
        "\n"
        "commands:\n"
        "  devices     list paired Bluetooth devices\n"
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
        std::fprintf(stderr, "sonyctl: no paired Bluetooth devices found\n");
        return false;
    }
    int found = -1;
    if (!opt.nameFilter.empty()) {
        for (int i = 0; i < count && found < 0; ++i)
            if (std::strstr(list[i].szDeviceName, opt.nameFilter.c_str()))
                found = i;
        if (found < 0)
            std::fprintf(stderr, "sonyctl: no paired device matching '%s'\n", opt.nameFilter.c_str());
    } else {
        for (const char* pattern : kDefaultDevicePatterns) {
            for (int i = 0; i < count && found < 0; ++i)
                if (std::strstr(list[i].szDeviceName, pattern))
                    found = i;
            if (found >= 0)
                break;
        }
        if (found < 0)
            std::fprintf(stderr,
                         "sonyctl: no Sony headphone found among paired devices "
                         "(try --device SUBSTR or --mac)\n");
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

    bool open(MDRConnection* connection, const Options& opt, int timeoutMs = 15000)
    {
        conn = connection;
        std::string mac, name;
        if (!pickDevice(conn, opt, mac, name))
            return false;

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
            std::fprintf(stderr, "sonyctl: cannot connect to %s: %s\n", name.c_str(),
                         mdrConnectionGetLastError(conn));
            return false;
        }

        if (mdrHeadphonesCreate(MDR_ABI_VERSION, conn, &dev) != MDR_RESULT_OK) {
            std::fprintf(stderr, "sonyctl: failed to create headphones instance\n");
            return false;
        }
        if (mdrHeadphonesRequestInit(dev) != MDR_RESULT_OK ||
            !waitEvent(MDR_EVENT_INITIALIZE_COMPLETE, timeoutMs)) {
            std::fprintf(stderr, "sonyctl: protocol init failed or timed out\n");
            return false;
        }
        if (mdrHeadphonesRequestFetch(dev) != MDR_RESULT_OK ||
            !waitEvent(MDR_EVENT_SYNC_COMPLETE, timeoutMs)) {
            std::fprintf(stderr, "sonyctl: state sync failed or timed out\n");
            return false;
        }
        return true;
    }

    // Pump the event loop until `target` arrives.
    bool waitEvent(MDREvent target, int timeoutMs = 10000)
    {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            MDREvent event = MDR_EVENT_NONE;
            const MDRResult pr = mdrHeadphonesPoll(dev, &event);
            if (pr != MDR_RESULT_OK) {
                std::fprintf(stderr, "sonyctl: connection lost while waiting: %s (%s / %s)\n",
                             mdrResultString(pr), text(MDR_TEXT_LAST_ERROR).c_str(),
                             mdrConnectionGetLastError(conn));
                return false;
            }
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
            std::fprintf(stderr, "sonyctl: commit failed\n");
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

void printNoiseControl(const MDRNoiseControl& nc)
{
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

void printBatteries(Session& s)
{
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
        std::fprintf(stderr, "sonyctl: noise control not available\n");
        return 1;
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
        std::fprintf(stderr, "sonyctl: noise control not available\n");
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
        std::fprintf(stderr, "sonyctl: setting noise control failed\n");
        return 1;
    }
    if (!s.commit()) {
        std::fprintf(stderr, "sonyctl: change was not applied\n");
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
        std::fprintf(stderr, "sonyctl: setting multipoint failed\n");
        return false;
    }
    if (!s.commit()) {
        std::fprintf(stderr, "sonyctl: multipoint change was not applied\n");
        return false;
    }
    return true;
}

int cmdMultipoint(Session& s, const std::string& action)
{
    const int index = findMultipointSetting(s);
    if (index < 0) {
        std::fprintf(stderr, "sonyctl: no multipoint setting found on this device\n");
        return 1;
    }
    MDRGeneralSetting current{};
    if (mdrHeadphonesGetGeneralSetting(s.dev, static_cast<uint32_t>(index), &current) !=
        MDR_RESULT_OK) {
        std::fprintf(stderr, "sonyctl: cannot read multipoint state\n");
        return 1;
    }
    if (action.empty()) {
        std::printf("multipoint: %s\n", current.boolean_value ? "on" : "off");
        return 0;
    }
    if (action == "on" || action == "off") {
        if (!setMultipoint(s, static_cast<uint32_t>(index), action == "on"))
            return 1;
        std::printf("multipoint: %s\n", action.c_str());
        return 0;
    }
    if (action == "reset") {
        std::printf("multipoint: %s -> off", current.boolean_value ? "on" : "off");
        std::fflush(stdout);
        if (!setMultipoint(s, static_cast<uint32_t>(index), false))
            return 1;
        std::printf(" -> on");
        std::fflush(stdout);
        if (!setMultipoint(s, static_cast<uint32_t>(index), true))
            return 1;
        std::printf(" (reset done)\n");
        return 0;
    }
    std::fprintf(stderr, "sonyctl: multipoint takes on, off, or reset\n");
    return 2;
}

int cmdPowerOff(Session& s)
{
    MDRPower power{};
    if (mdrHeadphonesGetPower(s.dev, &power) != MDR_RESULT_OK) {
        std::fprintf(stderr, "sonyctl: power control not available\n");
        return 1;
    }
    power.shutdown_requested = MDR_TRUE;
    if (mdrHeadphonesSetPower(s.dev, &power) != MDR_RESULT_OK) {
        std::fprintf(stderr, "sonyctl: power-off request failed\n");
        return 1;
    }
    // The device drops the link while applying; a failed commit-wait here
    // just means the shutdown won the race.
    s.commit(5000);
    std::printf("power-off sent\n");
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

void dumpFrame(void*, MDRPacketDirection dir, const unsigned char* frame, int size)
{
    std::printf("%s", dir == MDR_PACKET_DIRECTION_TX ? "TX " : "RX ");
    for (int i = 0; i < size; ++i)
        std::printf("%02x", frame[i]);
    std::printf("\n");
    std::fflush(stdout);
}

int cmdRaw(Session& s, const std::string& hex, int listenSecs)
{
    std::vector<uint8_t> payload;
    if (!parseHex(hex, payload)) {
        std::fprintf(stderr, "sonyctl: raw payload must be an even-length hex string\n");
        return 2;
    }
    mdrHeadphonesSetPacketCallback(s.dev, dumpFrame, nullptr);

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
            std::fprintf(stderr, "sonyctl: raw send failed: %s\n", mdrResultString(r));
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
    return 0;
}

int cmdDevices(MDRConnection* conn)
{
    MDRDeviceInfo* list = nullptr;
    int count = 0;
    const MDRResult r = mdrConnectionGetDevicesList(conn, &list, &count);
    if (r != MDR_RESULT_OK) {
        std::fprintf(stderr, "sonyctl: cannot list devices: %s\n", mdrResultString(r));
        return 1;
    }
    for (int i = 0; i < count; ++i)
        std::printf("%s  %s\n", list[i].szDeviceMacAddress, list[i].szDeviceName);
    mdrConnectionFreeDevicesList(conn, &list);
    if (count == 0)
        std::fprintf(stderr, "sonyctl: no paired Bluetooth devices found\n");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "sonyctl: %s requires a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--mac")
            opt.mac = needValue("--mac");
        else if (arg == "--device")
            opt.nameFilter = needValue("--device");
        else if (arg == "-h" || arg == "--help" || arg == "help")
            return usage(stdout);
        else if (arg == "-V" || arg == "--version" || arg == "version") {
            std::printf("sonyctl %s\n", SONYCTL_VERSION);
            return 0;
        }
        else if (opt.command.empty())
            opt.command = arg;
        else
            opt.args.push_back(arg);
    }
    if (opt.command.empty())
        return usage(stderr);

    ConnectionHolder holder;
    if (!holder.conn) {
        std::fprintf(stderr, "sonyctl: failed to create Bluetooth connection backend\n");
        return 1;
    }

    if (opt.command == "devices")
        return cmdDevices(holder.conn);

    // Resolve `mode <MODE>` and top-level mode shortcuts (nc/ambient/off + aliases).
    std::string setMode;
    ModeFlags modeFlags;
    std::vector<std::string> rest = opt.args;
    if (opt.command == "mode" && !rest.empty()) {
        setMode = canonicalMode(rest.front());
        if (setMode.empty()) {
            std::fprintf(stderr, "sonyctl: unknown mode '%s'\n", rest.front().c_str());
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
                std::fprintf(stderr, "sonyctl: %s requires a value\n", flag);
                std::exit(2);
            }
            return rest[++i];
        };
        if (arg == "--level") {
            modeFlags.level = std::atoi(flagValue("--level").c_str());
            if (modeFlags.level < 1 || modeFlags.level > 20) {
                std::fprintf(stderr, "sonyctl: --level must be 1-20\n");
                return 2;
            }
        } else if (arg == "--voice-focus") {
            const std::string v = flagValue("--voice-focus");
            modeFlags.voiceFocus = (v == "on" || v == "yes" || v == "1") ? 1 : 0;
        } else {
            std::fprintf(stderr, "sonyctl: unknown argument '%s'\n", arg.c_str());
            return 2;
        }
    }

    // raw <hex> [--listen SECS]
    std::string rawHex;
    int rawListen = 3;
    if (opt.command == "raw") {
        if (opt.args.empty()) {
            std::fprintf(stderr, "sonyctl: raw requires a hex payload\n");
            return 2;
        }
        rawHex = opt.args.front();
        for (size_t i = 1; i < opt.args.size(); ++i) {
            if (opt.args[i] == "--listen" && i + 1 < opt.args.size())
                rawListen = std::atoi(opt.args[++i].c_str());
            else {
                std::fprintf(stderr, "sonyctl: unknown raw argument '%s'\n", opt.args[i].c_str());
                return 2;
            }
        }
    }

    const bool knownCommand = opt.command == "status" || opt.command == "mode" ||
                              opt.command == "battery" || opt.command == "multipoint" ||
                              opt.command == "power-off" || opt.command == "raw" ||
                              !setMode.empty();
    if (!knownCommand) {
        std::fprintf(stderr, "sonyctl: unknown command '%s'\n", opt.command.c_str());
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

// sonyctl — observe and control Sony MDR-protocol headphones (WF-1000XM6 and
// other v2-era models) from the macOS command line.
//
// Built on libmdr / libmdr-bt from https://github.com/mos9527/SonyHeadphonesClient

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <mdr-bt/ConnectionMacOS.h>
#include <mdr-c/Base.h>
#include <mdr-c/Connection.h>
#include <mdr-c/Headphones.h>

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
        "  battery     show battery levels\n"
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

        // v2 UUID first, legacy second (mirrors upstream's AUTO mode).
        const char* const services[] = {MDR_SERVICE_UUID_XM5, MDR_SERVICE_UUID_LEGACY};
        MDRResult r = MDR_RESULT_ERROR_NO_CONNECTION;
        for (const char* uuid : services) {
            r = mdrConnectionConnect(conn, mac.c_str(), uuid);
            if (r == MDR_RESULT_OK || r == MDR_RESULT_INPROGRESS) {
                const int64_t deadline = nowMs() + timeoutMs;
                while (r != MDR_RESULT_OK && nowMs() < deadline) {
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

    const bool knownCommand =
        opt.command == "status" || opt.command == "mode" || opt.command == "battery";
    if (!knownCommand) {
        std::fprintf(stderr, "sonyctl: unknown command '%s'\n", opt.command.c_str());
        return usage(stderr);
    }

    Session session;
    if (!session.open(holder.conn, opt))
        return 1;

    if (opt.command == "status")
        return cmdStatus(session);
    if (opt.command == "mode" && opt.args.empty())
        return cmdMode(session);
    if (opt.command == "battery") {
        printBatteries(session);
        return 0;
    }
    return usage(stderr);
}

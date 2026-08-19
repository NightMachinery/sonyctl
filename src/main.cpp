// sonyctl — observe and control Sony MDR-protocol headphones (WF-1000XM6 and
// other v2-era models) from the macOS command line.
//
// Built on libmdr / libmdr-bt from https://github.com/mos9527/SonyHeadphonesClient

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

    std::fprintf(stderr, "sonyctl: unknown command '%s'\n", opt.command.c_str());
    return usage(stderr);
}

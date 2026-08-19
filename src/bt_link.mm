#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

#include "bt_link.h"

namespace {

IOBluetoothDevice* deviceFor(const char* macAddress)
{
    if (!macAddress || !*macAddress)
        return nil;
    NSString* address = [NSString stringWithUTF8String:macAddress];
    return [IOBluetoothDevice deviceWithAddressString:address];
}

} // namespace

int btLinkIsConnected(const char* macAddress)
{
    @autoreleasepool {
        IOBluetoothDevice* device = deviceFor(macAddress);
        if (!device)
            return -1;
        return [device isConnected] ? 1 : 0;
    }
}

int btLinkOpen(const char* macAddress)
{
    @autoreleasepool {
        IOBluetoothDevice* device = deviceFor(macAddress);
        if (!device)
            return -1;
        if ([device isConnected])
            return 0;
        // Opening the baseband link is enough: macOS reconnects the audio
        // profiles (A2DP/HFP) for a paired audio device on its own.
        return (int)[device openConnection];
    }
}

int btLinkClose(const char* macAddress)
{
    @autoreleasepool {
        IOBluetoothDevice* device = deviceFor(macAddress);
        if (!device)
            return -1;
        if (![device isConnected])
            return 0;
        return (int)[device closeConnection];
    }
}

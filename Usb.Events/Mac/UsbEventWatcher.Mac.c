#include <CoreFoundation/CoreFoundation.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#ifdef __MAC_OS_X_VERSION_MAX_ALLOWED
#if __MAC_OS_X_VERSION_MAX_ALLOWED <= 120100
#define kIOMainPortDefault kIOMasterPortDefault
#endif
#endif

#include <DiskArbitration/DiskArbitration.h>
#include <DiskArbitration/DASession.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct UsbDeviceData
{
    char DeviceName[512];
    char DeviceSystemPath[1024];
    char Product[512];
    char ProductDescription[512];
    char ProductID[512];
    char SerialNumber[512];
    char Vendor[512];
    char VendorDescription[512];
    char VendorID[512];
} UsbDeviceData;

static const struct UsbDeviceData empty;

typedef void (*UsbDeviceCallback)(UsbDeviceData usbDevice);
UsbDeviceCallback InsertedCallback;
UsbDeviceCallback RemovedCallback;

typedef void (*MountPointCallback)(const char* mountPoint);

static IONotificationPortRef notificationPort;

void debug_print(const char* format, ...)
{
#ifdef DEBUG
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
#endif
}

void print_cfstringref(const char* prefix, CFStringRef cfVal)
{
#ifdef DEBUG
    if (!cfVal) return;

    // Correctly calculate buffer size for UTF-8
    CFIndex len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(cfVal), kCFStringEncodingUTF8) + 1;
    char* cVal = malloc(len);

    if (!cVal) return;

    if (CFStringGetCString(cfVal, cVal, len, kCFStringEncodingUTF8))
    {
        printf("%s %s\n", prefix, cVal);
    }

    free(cVal);
#endif
}

void print_cfnumberref(const char* prefix, CFNumberRef cfVal)
{
#ifdef DEBUG
    int result;

    if (CFNumberGetValue(cfVal, kCFNumberSInt32Type, &result))
    {
        printf("%s %i\n", prefix, result);
    }
#endif
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// Helper function to extract a mount path from a disk session
// Returns 1 if found, 0 otherwise. Writes to outBuffer.
int GetMountPathFromDisk(DASessionRef session, const char* bsdName, char* outBuffer, size_t outBufferSize)
{
    if (!session || !bsdName || !outBuffer) return 0;

    int found = 0;
    DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsdName);
    if (disk)
    {
        CFDictionaryRef diskInfo = DADiskCopyDescription(disk);
        if (diskInfo)
        {
            CFURLRef fspath = (CFURLRef)CFDictionaryGetValue(diskInfo, kDADiskDescriptionVolumePathKey);
            if (fspath)
            {
                if (CFURLGetFileSystemRepresentation(fspath, false, (UInt8*)outBuffer, outBufferSize))
                {
                    found = 1;
                }
            }
            CFRelease(diskInfo);
        }
        CFRelease(disk);
    }
    return found;
}

// Changed to accept a buffer argument instead of returning a pointer to a global static
int getMountPathByBSDName(char* bsdName, char* outBuffer, size_t outBufferSize)
{
    if (!bsdName || !outBuffer)
    {
        return 0;
    }

    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session)
    {
        return 0;
    }

    int found = 0;

    // 1. Try to find a child partition that has a mount point
    CFDictionaryRef matchingDictionary = IOBSDNameMatching(kIOMainPortDefault, 0, bsdName);
    io_iterator_t it;

    // IOServiceGetMatchingServices consumes matchingDictionary
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, matchingDictionary, &it);
    if (kr != KERN_SUCCESS)
    {
        CFRelease(session);
        return 0;
    }

    io_object_t service;
    while ((service = IOIteratorNext(it)))
    {
        io_iterator_t children = 0;
        io_registry_entry_t child;

        if (IORegistryEntryGetChildIterator(service, kIOServicePlane, &children) == KERN_SUCCESS)
        {
            while ((child = IOIteratorNext(children)))
            {
                // FIX: Object returned by IORegistryEntrySearchCFProperty MUST be released
                CFStringRef bsdNameChild = (CFStringRef)IORegistryEntrySearchCFProperty(
                    child,
                    kIOServicePlane,
                    CFSTR("BSD Name"),
                    kCFAllocatorDefault,
                    kIORegistryIterateRecursively);

                if (bsdNameChild)
                {
                    // Dynamic buffer allocation based on encoding size
                    CFIndex len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(bsdNameChild), kCFStringEncodingUTF8)
                        + 1;
                    char* cVal = malloc(len);
                    if (cVal)
                    {
                        if (CFStringGetCString(bsdNameChild, cVal, len, kCFStringEncodingUTF8))
                        {
                            if (GetMountPathFromDisk(session, cVal, outBuffer, outBufferSize))
                            {
                                found = 1;
                            }
                        }
                        free(cVal);
                    }
                    CFRelease(bsdNameChild); // Fix Memory Leak
                }
                IOObjectRelease(child);

                if (found) break;
            }
            IOObjectRelease(children);
        }
        IOObjectRelease(service);

        if (found) break;
    }
    IOObjectRelease(it);

    // 2. If no child is found, try the device itself
    if (!found)
    {
        if (GetMountPathFromDisk(session, bsdName, outBuffer, outBufferSize))
        {
            found = 1;
        }
    }

    CFRelease(session);
    return found;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void get_usb_device_info(io_service_t device, int newdev)
{
    io_name_t devicename;
    io_string_t devicepath;
    io_name_t classname;

    char* c_val;
    CFIndex len;
    int result;

    if (IORegistryEntryGetName(device, devicename) != KERN_SUCCESS)
    {
        fprintf(stderr, "%s unknown device\n", newdev ? "added" : " removed");
        return;
    }

    // Use local struct instead of global to be re-entrant/thread-safe
    UsbDeviceData usbDevice = empty;

    debug_print("%s USB device: %s\n", newdev ? "FOUND" : "REMOVED", devicename);

    // Safe copy for DeviceName and other fields
    snprintf(usbDevice.DeviceName, sizeof(usbDevice.DeviceName), "%s", devicename);

    if (IORegistryEntryGetPath(device, kIOServicePlane, devicepath) == KERN_SUCCESS)
    {
        debug_print("\tDevice path: %s\n", devicepath);

        snprintf(usbDevice.DeviceSystemPath, sizeof(usbDevice.DeviceSystemPath), "%s", devicepath);
    }

    if (IOObjectGetClass(device, classname) == KERN_SUCCESS)
    {
        debug_print("\tDevice class name: %s\n", classname);
    }

    // Special case for Vendor/Product where we copy to two fields
    CFStringRef vendorname = (CFStringRef)IORegistryEntrySearchCFProperty(
        device, kIOServicePlane, CFSTR("USB Vendor Name"), NULL,
        kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (vendorname)
    {
        print_cfstringref("\tDevice vendor name:", vendorname);

        len = CFStringGetLength(vendorname) + 1;
        c_val = malloc(len * sizeof(char));
        if (c_val)
        {
            // CHANGED: ASCII -> UTF8
            if (CFStringGetCString(vendorname, c_val, len, kCFStringEncodingUTF8))
            {
                snprintf(usbDevice.Vendor, sizeof(usbDevice.Vendor), "%s", c_val);
                snprintf(usbDevice.VendorDescription, sizeof(usbDevice.VendorDescription), "%s", c_val);
            }

            free(c_val);
        }
        CFRelease(vendorname); // ADDED: Fix Memory Leak
    }

    CFNumberRef vendorId = (CFNumberRef)IORegistryEntrySearchCFProperty(
        device,
        kIOServicePlane,
        CFSTR("idVendor"),
        NULL,
        kIORegistryIterateRecursively | kIORegistryIterateParents);

    if (vendorId)
    {
        print_cfnumberref("\tVendor id:", vendorId);
        if (CFNumberGetValue(vendorId, kCFNumberSInt32Type, &result))
        {
            snprintf(usbDevice.VendorID, sizeof(usbDevice.VendorID), "%d", result);
        }
        CFRelease(vendorId); // Fix Memory Leak
    }

    CFStringRef productname = (CFStringRef)IORegistryEntrySearchCFProperty(
        device,
        kIOServicePlane,
        CFSTR("USB Product Name"),
        NULL,
        kIORegistryIterateRecursively | kIORegistryIterateParents);

    if (productname)
    {
        print_cfstringref("\tDevice product name:", productname);

        len = CFStringGetLength(productname) + 1;
        c_val = malloc(len * sizeof(char));
        if (c_val)
        {
            // CHANGED: ASCII -> UTF8
            if (CFStringGetCString(productname, c_val, len, kCFStringEncodingUTF8))
            {
                snprintf(usbDevice.Product, sizeof(usbDevice.Product), "%s", c_val);
                snprintf(usbDevice.ProductDescription, sizeof(usbDevice.ProductDescription), "%s", c_val);
            }

            free(c_val);
        }
        CFRelease(productname); // ADDED: Fix Memory Leak
    }

    CFNumberRef productId = (CFNumberRef)IORegistryEntrySearchCFProperty(
        device,
        kIOServicePlane,
        CFSTR("idProduct"),
        NULL,
        kIORegistryIterateRecursively | kIORegistryIterateParents);

    if (productId)
    {
        print_cfnumberref("\tProduct id:", productId);
        if (CFNumberGetValue(productId, kCFNumberSInt32Type, &result))
        {
            snprintf(usbDevice.ProductID, sizeof(usbDevice.ProductID), "%d", result);
        }
        CFRelease(productId); // Fix Memory Leak
    }

    CFStringRef serialnumber = (CFStringRef)IORegistryEntrySearchCFProperty(
        device,
        kIOServicePlane,
        CFSTR("USB Serial Number"),
        NULL,
        kIORegistryIterateRecursively | kIORegistryIterateParents);

    if (serialnumber)
    {
        print_cfstringref("\tDevice serial number:", serialnumber);

        len = CFStringGetLength(serialnumber) + 1;
        c_val = malloc(len * sizeof(char));
        if (c_val)
        {
            // CHANGED: ASCII -> UTF8
            if (CFStringGetCString(serialnumber, c_val, len, kCFStringEncodingUTF8))
            {
                snprintf(usbDevice.SerialNumber, sizeof(usbDevice.SerialNumber), "%s", c_val);
            }

            free(c_val);
        }
        CFRelease(serialnumber); // ADDED: Fix Memory Leak
    }

    debug_print("\n");

    if (newdev)
    {
        InsertedCallback(usbDevice);
    }
    else
    {
        RemovedCallback(usbDevice);
    }
}

void iterate_usb_devices(io_iterator_t iterator, int newdev)
{
    io_service_t usbDevice;

    while ((usbDevice = IOIteratorNext(iterator)))
    {
        get_usb_device_info(usbDevice, newdev);
        IOObjectRelease(usbDevice);
    }
}

void usb_device_added(void* refcon, io_iterator_t iterator)
{
    iterate_usb_devices(iterator, 1);
}

void usb_device_removed(void* refcon, io_iterator_t iterator)
{
    iterate_usb_devices(iterator, 0);
}

// Global variable to hold the run loop source
CFRunLoopSourceRef stopRunLoopSource = NULL;

CFRunLoopRef runLoop;

// Callback function for the run loop source
void stopRunLoopSourceCallback(void* info)
{
    // Stop the run loop when the source is triggered
    CFRunLoopStop(runLoop);
}

// Function to add the stop run loop source
void addStopRunLoopSource(void)
{
    // Create a custom context for the run loop source
    CFRunLoopSourceContext sourceContext = {
        .version = 0,
        .info = NULL,
        .retain = NULL,
        .release = NULL,
        .copyDescription = NULL,
        .equal = NULL,
        .hash = NULL,
        .schedule = NULL,
        .cancel = NULL,
        .perform = stopRunLoopSourceCallback
    };

    // Create the run loop source
    stopRunLoopSource = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &sourceContext);

    // Add the run loop source to the current run loop
    CFRunLoopAddSource(runLoop, stopRunLoopSource, kCFRunLoopDefaultMode);
}

// Function to remove the stop run loop source
void removeStopRunLoopSource(void)
{
    if (stopRunLoopSource != NULL)
    {
        // Remove the run loop source from the current run loop
        CFRunLoopRemoveSource(runLoop, stopRunLoopSource, kCFRunLoopDefaultMode);

        // Release the run loop source
        CFRelease(stopRunLoopSource);
        stopRunLoopSource = NULL;
    }
}

void init_notifier(void)
{
    notificationPort = IONotificationPortCreate(kIOMainPortDefault);
    CFRunLoopAddSource(runLoop, IONotificationPortGetRunLoopSource(notificationPort), kCFRunLoopDefaultMode);

    debug_print("init_notifier ok\n");
}

// https://sudonull.com/post/141779-Working-with-USB-devices-in-a-C-program-on-MacOS-X

// If a function has Create or Copy in its name, you own the returned object and must CFRelease it when done.
// Otherwise, you do not own it and must not release it. If you do, you will get CF_IS_OBJC exception.

void configure_and_start_notifier(void)
{
    debug_print("Starting notifier\n");

    CFMutableDictionaryRef matchDictAdded = (CFMutableDictionaryRef)IOServiceMatching(kIOUSBDeviceClassName);

    if (!matchDictAdded)
    {
        fprintf(stderr,
                "Failed to create matching dictionary for kIOUSBDeviceClassName (for kIOMatchedNotification)\n");
        return;
    }

    kern_return_t addResult;

    io_iterator_t deviceAddedIter;
    addResult = IOServiceAddMatchingNotification(notificationPort, kIOMatchedNotification, matchDictAdded,
                                                 usb_device_added, NULL, &deviceAddedIter);

    if (addResult != KERN_SUCCESS)
    {
        fprintf(stderr, "IOServiceAddMatchingNotification failed for kIOMatchedNotification\n");
        //CFRelease(matchDict); - CF_IS_OBJC exception
        return;
    }

    usb_device_added(NULL, deviceAddedIter);

    // CHANGED: Create a NEW dictionary for Removed events (DO NOT REUSE matchDict)
    CFMutableDictionaryRef matchDictRemoved = (CFMutableDictionaryRef)IOServiceMatching(kIOUSBDeviceClassName);

    if (!matchDictRemoved)
    {
        fprintf(stderr,
                "Failed to create matching dictionary for kIOUSBDeviceClassName (for kIOTerminatedNotification)\n");
        return;
    }

    io_iterator_t deviceRemovedIter;
    addResult = IOServiceAddMatchingNotification(notificationPort, kIOTerminatedNotification, matchDictRemoved,
                                                 usb_device_removed, NULL, &deviceRemovedIter);

    if (addResult != KERN_SUCCESS)
    {
        fprintf(stderr, "IOServiceAddMatchingNotification failed for kIOTerminatedNotification\n");
        //CFRelease(matchDict); - CF_IS_OBJC exception
        return;
    }

    usb_device_removed(NULL, deviceRemovedIter);

    // Add the stop run loop source
    addStopRunLoopSource();

    // Start the run loop
    CFRunLoopRun();

    // Remove the stop run loop source
    removeStopRunLoopSource();

    //CFRelease(matchDict); - CF_IS_OBJC exception
}

void deinit_notifier(void)
{
    CFRunLoopRemoveSource(runLoop, IONotificationPortGetRunLoopSource(notificationPort), kCFRunLoopDefaultMode);
    IONotificationPortDestroy(notificationPort);

    debug_print("deinit_notifier ok\n");
}

void signal_handler(int signum)
{
    debug_print("\ngot signal, signnum=%i  stopping current RunLoop\n", signum);

    CFRunLoopStop(runLoop);
}

void init_signal_handler(void)
{
    signal(SIGINT, signal_handler);
    signal(SIGQUIT, signal_handler);
    signal(SIGTERM, signal_handler);
}

#ifdef __cplusplus

extern "C" {



#endif

void StartMacWatcher(UsbDeviceCallback insertedCallback, UsbDeviceCallback removedCallback)
{
    InsertedCallback = insertedCallback;
    RemovedCallback = removedCallback;

    runLoop = CFRunLoopGetCurrent();

    //init_signal_handler();
    init_notifier();
    configure_and_start_notifier();
    deinit_notifier();
}

void StopMacWatcher(void)
{
    if (stopRunLoopSource != NULL)
    {
        // Signal the run loop source to stop the run loop
        CFRunLoopSourceSignal(stopRunLoopSource);

        // Wake up the run loop to process the signal immediately
        CFRunLoopWakeUp(runLoop);
    }
}

void GetMacMountPoint(const char* syspath, MountPointCallback mountPointCallback)
{
    if (!syspath)
    {
        mountPointCallback("");
        return;
    }

    CFMutableDictionaryRef matchingDictionary = IOServiceMatching(kIOUSBInterfaceClassName);

    // now specify class and subclass to iterate only through USB mass storage devices:
    CFNumberRef cfValue;
    SInt32 deviceClassNum = kUSBMassStorageInterfaceClass;
    cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &deviceClassNum);
    CFDictionaryAddValue(matchingDictionary, CFSTR(kUSBInterfaceClass), cfValue);
    CFRelease(cfValue);

    // NOTE: if you will specify only a device class and will not specify a subclass, it will return an empty iterator, and I don't know how to say that we need any subclass. 
    // BUT: all the devices I've checked had kUSBMassStorageSCSISubClass
    SInt32 deviceSubClassNum = kUSBMassStorageSCSISubClass;
    cfValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &deviceSubClassNum);
    CFDictionaryAddValue(matchingDictionary, CFSTR(kUSBInterfaceSubClass), cfValue);
    CFRelease(cfValue);

    io_iterator_t foundIterator = 0;
    io_service_t usbInterface;
    IOServiceGetMatchingServices(kIOMainPortDefault, matchingDictionary, &foundIterator);

    int found = 0;
    int match = 0;

    // iterate through USB mass storage devices
    while ((usbInterface = IOIteratorNext(foundIterator)))
    {
        io_string_t devicepath;
        if (IORegistryEntryGetPath(usbInterface, kIOServicePlane, devicepath) == KERN_SUCCESS)
        {
            if (strncmp(devicepath, syspath, strlen(syspath)) == 0)
            {
                CFStringRef bsdName = (CFStringRef)IORegistryEntrySearchCFProperty(
                    usbInterface,
                    kIOServicePlane,
                    CFSTR("BSD Name"),
                    kCFAllocatorDefault,
                    kIORegistryIterateRecursively);

                if (bsdName)
                {
                    long len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(bsdName), kCFStringEncodingUTF8) +
                        1;
                    char* c_val = malloc(len);
                    if (c_val)
                    {
                        if (CFStringGetCString(bsdName, c_val, len, kCFStringEncodingUTF8))
                        {
                            char mountPathBuffer[1024];
                            if (getMountPathByBSDName(c_val, mountPathBuffer, sizeof(mountPathBuffer)))
                            {
                                found = 1;
                                mountPointCallback(mountPathBuffer);
                            }
                        }
                        free(c_val);
                    }
                    CFRelease(bsdName); // Fix Memory Leak
                }

                match = 1;
            }
        }
        IOObjectRelease(usbInterface);

        if (match)
            break;
    }
    IOObjectRelease(foundIterator);

    if (!found)
        mountPointCallback("");
}

#ifdef __cplusplus
}
#endif

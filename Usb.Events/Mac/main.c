#include "UsbEventWatcher.Mac.h"
#include <stdio.h>
#include <pthread.h>

void OnInserted(UsbDeviceData usbDevice)
{
    printf("++ Inserted: %s \n", usbDevice.DeviceName);
}

void OnRemoved(UsbDeviceData usbDevice)
{
    printf("-- Removed: %s \n", usbDevice.DeviceName);
}

void* ctx;

void* StartWatcher(void* arg)
{
    if (ctx) RunMacWatcher(ctx);

    pthread_exit(NULL);
}

int main(void)
{
    pthread_t thread;

    printf("USB events: \n");

    ctx = CreateMacWatcherContext(OnInserted, OnRemoved);

    int result = pthread_create(&thread, NULL, StartWatcher, NULL);

    if (result != 0)
    {
        printf("Error creating the thread. Exiting program.\n");
        return -1;
    }

    getchar();

    if (ctx)
    {
        StopMacWatcher(ctx);
    }

    pthread_join(thread, NULL);
    
    ReleaseMacWatcherContext(ctx);

    return 0;
}

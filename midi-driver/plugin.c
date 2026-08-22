/*
 * E-MU CA0189 -- CoreMIDI driver plug-in.
 *
 * The 0404 USB carries an ordinary USB-MIDI 1.0 interface, but under the
 * vendor-specific class byte, so macOS's built-in class driver never binds it
 * -- the same reason the audio side needs the HAL plug-in. This bundle lives
 * in /Library/Audio/MIDI Drivers and runs inside MIDIServer, which is a
 * different process from the audio driver's coreaudiod: the two coexist
 * because each claims only its own USB interface, and neither holds the
 * device open.
 *
 * Threading, per MIDIDriver.h: everything except Send() and MIDIReceived()
 * happens on the server's main thread, including the IOKit hot-plug
 * notifications, which are scheduled on that run loop from Start(). Send()
 * arrives on MIDIServer's scheduling thread and only touches the bulk OUT
 * pipe. The reader thread only touches the bulk IN pipe and MIDIReceived().
 * The lock exists to keep both away from detach, not from each other.
 *
 * The packet framing lives in the Rust core (midi.rs), tested without
 * hardware; this file is only the IOKit transport and the CoreMIDI surface.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreMIDI/MIDIDriver.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../rust/emu-ca0189/include/emu_ca0189.h"
#include "../shared/device.h"
#include "../shared/usb_util.h"

static os_log_t emu_log(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create("net.quantum-bit.EMUMIDIDriver", "driver"); });
    return log;
}

#define EMU_LOG(fmt, ...) os_log(emu_log(), "EMUMIDI: " fmt, ##__VA_ARGS__)

/* --- state ---------------------------------------------------------------
 *
 * One device, like the HAL plug-in. All of it lives in globals because
 * MIDIServer creates exactly one instance of the driver.
 */

static MIDIDeviceRef   gMidiDevice;   /* the object in the user's MIDI setup */
static MIDIEndpointRef gSource;       /* device -> apps */
static MIDIEndpointRef gDest;         /* apps -> device */

static const EmuDeviceIdentity* gIdentity;

/* USB transport. Guarded by gUsbLock against detach. */
static pthread_mutex_t gUsbLock = PTHREAD_MUTEX_INITIALIZER;
static IOUSBDeviceInterface500**    gDev;
static IOUSBInterfaceInterface500** gIntf;
static uint8_t  gInPipe, gOutPipe;

static pthread_t     gReader;
static bool          gReaderRunning;
static atomic_bool   gReaderStop;

/* Storage for the Rust packet encoder used by Send(). */
static uint8_t gEncoderStorage[256] __attribute__((aligned(16)));
static EmuMidiEncoder* gEncoder;

/* Hot-plug notifications, one first-match and one termination per known
 * product ID (IOKit will not match on a vendor alone). */
static IONotificationPortRef gNotifyPort;
static io_iterator_t gMatchIter[EMU_DEVICE_COUNT];
static io_iterator_t gTermIter[EMU_DEVICE_COUNT];

/* --- input: reader thread ----------------------------------------------- */

static void* reader_main(void* arg)
{
    (void)arg;
    pthread_setname_np("emu-midi-reader");

    while (!atomic_load_explicit(&gReaderStop, memory_order_relaxed)) {
        uint8_t buffer[512];
        UInt32 size = sizeof buffer;

        /* A finite timeout keeps the stop flag honoured; MIDI is idle almost
         * always, so timeouts are the common case, not a failure. */
        IOReturn kr = (*gIntf)->ReadPipeTO(gIntf, gInPipe, buffer, &size, 250, 250);
        if (kr == kIOUSBTransactionTimeout || kr == kIOReturnAborted) continue;
        if (kr != kIOReturnSuccess) {
            /* Device unplugged or the pipe died; the termination notification
             * on the main thread owns the cleanup. */
            EMU_LOG("read pipe error 0x%08x, reader exiting", kr);
            break;
        }

        /* One bulk transfer becomes one MIDIPacket. The decoded bytes are in
         * stream order, and a MIDIPacket may carry several messages -- or a
         * SysEx fragment that continues in the next one. */
        uint8_t midi[384];
        uint32_t midi_len = 0;
        for (UInt32 off = 0; off + 4 <= size; off += 4) {
            uint8_t decoded[3];
            uint32_t n = emu_midi_decode(buffer + off, decoded);
            for (uint32_t i = 0; i < n && midi_len < sizeof midi; i++) {
                midi[midi_len++] = decoded[i];
            }
        }
        if (midi_len == 0) continue;

        struct { MIDIPacketList list; uint8_t space[512]; } storage;
        MIDIPacket* packet = MIDIPacketListInit(&storage.list);
        packet = MIDIPacketListAdd(&storage.list, sizeof storage, packet,
                                   mach_absolute_time(), midi_len, midi);
        if (packet && gSource) MIDIReceived(gSource, &storage.list);
    }
    return NULL;
}

/* --- USB attach and detach ----------------------------------------------
 *
 * Main thread only (Start, Stop, and the IOKit notification callbacks).
 */

static void usb_detach(void)
{
    if (gReaderRunning) {
        atomic_store_explicit(&gReaderStop, true, memory_order_relaxed);
        pthread_join(gReader, NULL);
        gReaderRunning = false;
    }

    pthread_mutex_lock(&gUsbLock);
    if (gIntf) {
        (*gIntf)->USBInterfaceClose(gIntf);
        (*gIntf)->Release(gIntf);
        gIntf = NULL;
    }
    if (gDev) {
        (*gDev)->Release(gDev);
        gDev = NULL;
    }
    gEncoder = NULL;
    pthread_mutex_unlock(&gUsbLock);
}

static bool usb_attach(void)
{
    if (gIntf) return true;

    io_service_t service = IO_OBJECT_NULL;
    gIdentity = emu_find_device(EMU_DEFAULT_PRODUCT_ID, &service);
    if (!gIdentity) return false;

    IOCFPlugInInterface** plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS || !plugin) return false;

    IOUSBDeviceInterface500** dev = NULL;
    HRESULT hr = (*plugin)->QueryInterface(plugin,
                    CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID*)&dev);
    (*plugin)->Release(plugin);
    if (hr || !dev) return false;

    /* Descriptors are readable without opening the device, which matters:
     * opening it would collide with the audio driver's clock requests. */
    IOUSBConfigurationDescriptorPtr cfg = NULL;
    EmuDeviceModel model;
    if ((*dev)->GetConfigurationDescriptorPtr(dev, 0, &cfg) != kIOReturnSuccess ||
        emu_parse_config_descriptor((const uint8_t*)cfg,
                                    OSSwapLittleToHostInt16(cfg->wTotalLength),
                                    &model) != 0 ||
        model.midi_interface == 0xff) {
        /* A Tracker Pre lands here: MIDI ports on the box, nothing in the
         * descriptors to drive them with. */
        EMU_LOG("%{public}s has no MIDI-streaming interface", gIdentity->name);
        (*dev)->Release(dev);
        return false;
    }

    IOUSBInterfaceInterface500** intf = NULL;
    if (!emu_find_interface(dev, model.midi_interface, &intf)) {
        (*dev)->Release(dev);
        return false;
    }
    if ((*intf)->USBInterfaceOpen(intf) != kIOReturnSuccess) {
        (*intf)->Release(intf);
        (*dev)->Release(dev);
        return false;
    }

    uint8_t in_pipe, out_pipe;
    uint16_t max_packet;
    if (!emu_find_bulk_pipe(intf, kUSBIn, &in_pipe, &max_packet) ||
        !emu_find_bulk_pipe(intf, kUSBOut, &out_pipe, &max_packet)) {
        (*intf)->USBInterfaceClose(intf);
        (*intf)->Release(intf);
        (*dev)->Release(dev);
        return false;
    }

    pthread_mutex_lock(&gUsbLock);
    gDev = dev;
    gIntf = intf;
    gInPipe = in_pipe;
    gOutPipe = out_pipe;
    gEncoder = emu_midi_encoder_init(gEncoderStorage, 0);
    pthread_mutex_unlock(&gUsbLock);

    atomic_store_explicit(&gReaderStop, false, memory_order_relaxed);
    gReaderRunning = pthread_create(&gReader, NULL, reader_main, NULL) == 0;

    EMU_LOG("attached %{public}s, MIDI interface %u", gIdentity->name,
            model.midi_interface);
    return true;
}

/* --- the CoreMIDI device object ------------------------------------------ */

static void set_offline(bool offline)
{
    if (gMidiDevice) {
        MIDIObjectSetIntegerProperty(gMidiDevice, kMIDIPropertyOffline, offline);
    }
}

/* Creates the device, its single entity and the two endpoints, and hands it
 * to `list` (FindDevices) or installs it into the running setup (Start). */
static MIDIDeviceRef create_midi_device(MIDIDriverRef self, const char* name)
{
    MIDIDeviceRef device = 0;
    CFStringRef cfname = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    if (!cfname) return 0;

    if (MIDIDeviceCreate(self, cfname, CFSTR("E-MU Systems"), cfname, &device) == noErr) {
        MIDIEntityRef entity = 0;
        if (MIDIDeviceNewEntity(device, CFSTR("MIDI"), kMIDIProtocol_1_0,
                                true, 1, 1, &entity) != noErr) {
            MIDIDeviceDispose(device);
            device = 0;
        }
    }
    CFRelease(cfname);
    return device;
}

static void bind_endpoints(void)
{
    gSource = 0;
    gDest = 0;
    if (!gMidiDevice || MIDIDeviceGetNumberOfEntities(gMidiDevice) == 0) return;
    MIDIEntityRef entity = MIDIDeviceGetEntity(gMidiDevice, 0);
    if (MIDIEntityGetNumberOfSources(entity) > 0) {
        gSource = MIDIEntityGetSource(entity, 0);
    }
    if (MIDIEntityGetNumberOfDestinations(entity) > 0) {
        gDest = MIDIEntityGetDestination(entity, 0);
    }
}

/* Attaches the hardware if it is present, creating the CoreMIDI device on
 * first sight, and keeps kMIDIPropertyOffline truthful either way. */
static void attach_and_publish(MIDIDriverRef self)
{
    if (!usb_attach()) {
        set_offline(true);
        return;
    }

    if (!gMidiDevice) {
        gMidiDevice = create_midi_device(self, gIdentity->name);
        if (gMidiDevice) {
            MIDISetupAddDevice(gMidiDevice);
            bind_endpoints();
        }
    }
    set_offline(false);
}

/* --- hot-plug ------------------------------------------------------------ */

static MIDIDriverRef gSelf;   /* for the IOKit callbacks */

static void drain(io_iterator_t iter)
{
    io_service_t service;
    while ((service = IOIteratorNext(iter))) IOObjectRelease(service);
}

static void device_appeared(void* refcon, io_iterator_t iter)
{
    (void)refcon;
    drain(iter);
    attach_and_publish(gSelf);
}

static void device_terminated(void* refcon, io_iterator_t iter)
{
    (void)refcon;
    drain(iter);
    /* Tear down and re-probe rather than working out which device died; with
     * a second one still plugged in, this reattaches to it. */
    usb_detach();
    attach_and_publish(gSelf);
}

static void install_notifications(void)
{
    gNotifyPort = IONotificationPortCreate(kIOMainPortDefault);
    if (!gNotifyPort) return;
    CFRunLoopAddSource(CFRunLoopGetCurrent(),
                       IONotificationPortGetRunLoopSource(gNotifyPort),
                       kCFRunLoopDefaultMode);

    for (unsigned i = 0; i < EMU_DEVICE_COUNT; i++) {
        for (int kind = 0; kind < 2; kind++) {
            CFMutableDictionaryRef matching = IOServiceMatching(kIOUSBDeviceClassName);
            if (!matching) continue;
            SInt32 vid = EMU_VENDOR_ID, pid = kEmuDevices[i].product_id;
            CFNumberRef vref = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);
            CFNumberRef pref = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
            CFDictionarySetValue(matching, CFSTR(kUSBVendorID), vref);
            CFDictionarySetValue(matching, CFSTR(kUSBProductID), pref);
            CFRelease(vref);
            CFRelease(pref);

            io_iterator_t* iter = kind == 0 ? &gMatchIter[i] : &gTermIter[i];
            if (IOServiceAddMatchingNotification(
                    gNotifyPort,
                    kind == 0 ? kIOFirstMatchNotification : kIOTerminatedNotification,
                    matching,
                    kind == 0 ? device_appeared : device_terminated,
                    NULL, iter) == KERN_SUCCESS) {
                /* Arming requires draining what already matches. */
                drain(*iter);
            }
        }
    }
}

static void remove_notifications(void)
{
    for (unsigned i = 0; i < EMU_DEVICE_COUNT; i++) {
        if (gMatchIter[i]) { IOObjectRelease(gMatchIter[i]); gMatchIter[i] = 0; }
        if (gTermIter[i])  { IOObjectRelease(gTermIter[i]);  gTermIter[i] = 0; }
    }
    if (gNotifyPort) {
        IONotificationPortDestroy(gNotifyPort);
        gNotifyPort = NULL;
    }
}

/* --- driver entry points ------------------------------------------------- */

/* Version 1 servers only. Modern MIDIServer uses the setup persisted from
 * Start(), but the contract costs little to honour. */
static OSStatus FindDevices(MIDIDriverRef self, MIDIDeviceListRef devList)
{
    io_service_t service = IO_OBJECT_NULL;
    const EmuDeviceIdentity* id = emu_find_device(EMU_DEFAULT_PRODUCT_ID, &service);
    if (!id) return noErr;
    IOObjectRelease(service);

    MIDIDeviceRef device = create_midi_device(self, id->name);
    if (device) MIDIDeviceListAddDevice(devList, device);
    return noErr;
}

static OSStatus Start(MIDIDriverRef self, MIDIDeviceListRef devList)
{
    gSelf = self;

    /* Devices from a previous run come back through the setup. Adopt the
     * first; remove_duplicates is not worth solving for a driver that
     * publishes exactly one. */
    if (MIDIDeviceListGetNumberOfDevices(devList) > 0) {
        gMidiDevice = MIDIDeviceListGetDevice(devList, 0);
        bind_endpoints();
    }

    install_notifications();
    attach_and_publish(self);
    return noErr;
}

static OSStatus Stop(MIDIDriverRef self)
{
    (void)self;
    remove_notifications();
    usb_detach();
    set_offline(true);
    return noErr;
}

static OSStatus Send(MIDIDriverRef self, const MIDIPacketList* pktlist,
                     void* destRefCon1, void* destRefCon2)
{
    (void)self; (void)destRefCon1; (void)destRefCon2;

    /* Encode the whole packet list, then write once per packet-list rather
     * than per message. 512 bytes of packets is 128 messages, far beyond any
     * real packet list; overflow just splits the write. */
    pthread_mutex_lock(&gUsbLock);
    if (!gIntf || !gEncoder) {
        pthread_mutex_unlock(&gUsbLock);
        return kMIDINotPermitted;
    }

    uint8_t out[512];
    uint32_t out_len = 0;
    OSStatus status = noErr;

    const MIDIPacket* packet = &pktlist->packet[0];
    for (UInt32 p = 0; p < pktlist->numPackets; p++) {
        for (UInt16 i = 0; i < packet->length; i++) {
            if (emu_midi_encode(gEncoder, packet->data[i], out + out_len)) {
                out_len += 4;
                if (out_len == sizeof out) {
                    if ((*gIntf)->WritePipeTO(gIntf, gOutPipe, out, out_len,
                                              1000, 1000) != kIOReturnSuccess) {
                        status = kMIDIMessageSendErr;
                        goto done;
                    }
                    out_len = 0;
                }
            }
        }
        packet = MIDIPacketNext(packet);
    }
    if (out_len > 0) {
        if ((*gIntf)->WritePipeTO(gIntf, gOutPipe, out, out_len, 1000, 1000)
            != kIOReturnSuccess) {
            status = kMIDIMessageSendErr;
        }
    }
done:
    pthread_mutex_unlock(&gUsbLock);
    return status;
}

static OSStatus Configure(MIDIDriverRef self, MIDIDeviceRef device)
{
    (void)self; (void)device;
    return noErr;
}

static OSStatus EnableSource(MIDIDriverRef self, MIDIEndpointRef src, Boolean enabled)
{
    (void)self; (void)src; (void)enabled;
    /* The reader always runs; four idle bytes a second are not worth the
     * bookkeeping of stopping it. */
    return noErr;
}

static OSStatus Flush(MIDIDriverRef self, MIDIEndpointRef dest,
                      void* destRefCon1, void* destRefCon2)
{
    (void)self; (void)dest; (void)destRefCon1; (void)destRefCon2;
    /* Nothing is scheduled ahead; Send writes synchronously. */
    return noErr;
}

static OSStatus Monitor(MIDIDriverRef self, MIDIEndpointRef dest,
                        const MIDIPacketList* pktlist)
{
    (void)self; (void)dest; (void)pktlist;
    return noErr;
}

/* --- CFPlugIn COM boilerplate -------------------------------------------- */

typedef struct {
    MIDIDriverInterface* vtable;
    CFUUIDRef factoryID;
    UInt32 refCount;
} Driver;

static HRESULT DriverQueryInterface(void* thisPointer, REFIID iid, LPVOID* ppv);
static ULONG DriverAddRef(void* thisPointer);
static ULONG DriverRelease(void* thisPointer);

static MIDIDriverInterface gInterface = {
    NULL,   /* _reserved */
    DriverQueryInterface,
    DriverAddRef,
    DriverRelease,
    FindDevices,
    Start,
    Stop,
    Configure,
    Send,
    EnableSource,
    Flush,
    Monitor,
    NULL,   /* SendPackets, version 3 */
    NULL,   /* MonitorEvents, version 3 */
};

static HRESULT DriverQueryInterface(void* thisPointer, REFIID iid, LPVOID* ppv)
{
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, iid);
    bool ok = CFEqual(requested, kMIDIDriverInterface2ID)
           || CFEqual(requested, kMIDIDriverInterfaceID)
           || CFEqual(requested, IUnknownUUID);
    CFRelease(requested);

    if (!ok) {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    DriverAddRef(thisPointer);
    *ppv = thisPointer;
    return S_OK;
}

static ULONG DriverAddRef(void* thisPointer)
{
    Driver* driver = (Driver*)thisPointer;
    return ++driver->refCount;
}

static ULONG DriverRelease(void* thisPointer)
{
    Driver* driver = (Driver*)thisPointer;
    if (--driver->refCount > 0) return driver->refCount;

    CFPlugInRemoveInstanceForFactory(driver->factoryID);
    CFRelease(driver->factoryID);
    free(driver);
    return 0;
}

void* EMUMIDIDriverFactory(CFAllocatorRef allocator, CFUUIDRef typeID);

void* EMUMIDIDriverFactory(CFAllocatorRef allocator, CFUUIDRef typeID)
{
    (void)allocator;
    if (!CFEqual(typeID, kMIDIDriverTypeID)) return NULL;

    Driver* driver = calloc(1, sizeof(Driver));
    if (!driver) return NULL;

    driver->vtable = &gInterface;
    driver->factoryID = CFUUIDCreateFromString(NULL,
        CFSTR("33299861-73A0-4DE4-97BD-C4B74730F9F0"));
    driver->refCount = 1;
    CFPlugInAddInstanceForFactory(driver->factoryID);
    return driver;
}

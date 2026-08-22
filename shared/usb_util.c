#include "usb_util.h"

#include <stdio.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>

bool emu_find_interface(IOUSBDeviceInterface500** dev,
                        uint8_t interface_number,
                        IOUSBInterfaceInterface500*** out)
{
    IOUSBFindInterfaceRequest request;
    request.bInterfaceClass    = kIOUSBFindInterfaceDontCare;
    request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    request.bAlternateSetting  = kIOUSBFindInterfaceDontCare;

    io_iterator_t iter = IO_OBJECT_NULL;
    if ((*dev)->CreateInterfaceIterator(dev, &request, &iter) != kIOReturnSuccess) {
        fprintf(stderr, "error: CreateInterfaceIterator failed\n");
        return false;
    }

    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        IOCFPlugInInterface** plugin = NULL;
        SInt32 score = 0;
        kern_return_t kr = IOCreatePlugInInterfaceForService(
            service, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID,
            &plugin, &score);
        IOObjectRelease(service);
        if (kr != kIOReturnSuccess || !plugin) continue;

        IOUSBInterfaceInterface500** intf = NULL;
        HRESULT hr = (*plugin)->QueryInterface(plugin,
                        CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500), (LPVOID*)&intf);
        (*plugin)->Release(plugin);
        if (hr || !intf) continue;

        UInt8 number = 0xff;
        (*intf)->GetInterfaceNumber(intf, &number);
        if (number == interface_number) {
            IOObjectRelease(iter);
            *out = intf;
            return true;
        }
        (*intf)->Release(intf);
    }

    IOObjectRelease(iter);
    fprintf(stderr, "error: interface %u not found\n", interface_number);
    return false;
}

bool emu_find_isoc_pipe(IOUSBInterfaceInterface500** intf,
                        uint8_t direction,
                        uint8_t* out_pipe,
                        uint16_t* out_max_packet)
{
    UInt8 num_endpoints = 0;
    if ((*intf)->GetNumEndpoints(intf, &num_endpoints) != kIOReturnSuccess) {
        return false;
    }

    /* Pipe 0 is the default control pipe, so endpoints start at 1. */
    for (UInt8 i = 1; i <= num_endpoints; i++) {
        UInt8 pipe_direction, number, transfer_type, interval;
        UInt16 max_packet = 0;
        if ((*intf)->GetPipeProperties(intf, i, &pipe_direction, &number,
                                       &transfer_type, &max_packet, &interval)
            != kIOReturnSuccess) {
            continue;
        }
        if (transfer_type == kUSBIsoc && pipe_direction == direction) {
            *out_pipe = i;
            *out_max_packet = max_packet;
            return true;
        }
    }
    return false;
}

const char* emu_isoc_status_name(int32_t status)
{
    switch ((uint32_t)status) {
        case 0:          return "success";
        case 0xe0004001: return "kIOUSBNotSent1Err";
        case 0xe0004002: return "kIOUSBNotSent2Err";
        case 0xe0004003: return "kIOUSBBufferUnderrunErr";
        case 0xe0004004: return "kIOUSBBufferOverrunErr";
        case 0xe000400f: return "kIOUSBWrongPIDErr";
        case 0xe0004010: return "kIOUSBPIDCheckErr";
        case 0xe0004011: return "kIOUSBDataToggleErr";
        case 0xe0004050: return "kIOUSBTransactionReturned";
        case 0xe0004051: return "kIOUSBTransactionTimeout";
        case 0xe00002e7: return "kIOReturnUnderrun";
        case 0xe00002e8: return "kIOReturnOverrun";
        case 0xe00002eb: return "kIOReturnAborted";
        case 0xe00002ec: return "kIOReturnNoBandwidth";
        case 0xe00002ed: return "kIOReturnNotResponding";
        case 0xe00002ee: return "kIOReturnIsoTooOld";
        case 0xe00002ef: return "kIOReturnIsoTooNew";
        default:         return "?";
    }
}

bool emu_frame_ok(int32_t status)
{
    return status == 0 || (uint32_t)status == 0xe00002e7;
}

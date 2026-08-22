/*
 * midi-check -- is the MIDI driver publishing endpoints, and do they move?
 *
 * The CoreMIDI analogue of hal-check: it asks the system, not the hardware,
 * so it exercises the exact path an application would.
 *
 *   midi-check              list every MIDI endpoint, flagging ours
 *   midi-check send         send a note on/off pair to our destination
 *   midi-check dump [secs]  print what arrives from our source
 *
 * With a DIN cable from the device's MIDI OUT to its MIDI IN, `send` in one
 * terminal and `dump` in another close the loop through CoreMIDI, the driver,
 * USB and the physical ports.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#define DRIVER_ID "net.quantum-bit.EMUMIDIDriver"

static void print_cfstring(CFStringRef s)
{
    char buffer[256] = "?";
    if (s) CFStringGetCString(s, buffer, sizeof buffer, kCFStringEncodingUTF8);
    printf("%s", buffer);
    if (s) CFRelease(s);
}

static CFStringRef endpoint_string(MIDIEndpointRef ep, CFStringRef property)
{
    CFStringRef s = NULL;
    MIDIObjectGetStringProperty(ep, property, &s);
    return s;
}

static bool is_ours(MIDIEndpointRef ep)
{
    CFStringRef owner = endpoint_string(ep, kMIDIPropertyDriverOwner);
    bool ours = owner && CFStringCompare(owner, CFSTR(DRIVER_ID), 0) == kCFCompareEqualTo;
    if (owner) CFRelease(owner);
    return ours;
}

static void list_side(const char* label, ItemCount count,
                      MIDIEndpointRef (*get)(ItemCount))
{
    printf("%s (%lu)\n", label, (unsigned long)count);
    for (ItemCount i = 0; i < count; i++) {
        MIDIEndpointRef ep = get(i);
        SInt32 offline = 0;
        MIDIObjectGetIntegerProperty(ep, kMIDIPropertyOffline, &offline);
        printf("  ");
        print_cfstring(endpoint_string(ep, kMIDIPropertyDisplayName));
        if (offline) printf("  (offline)");
        if (is_ours(ep)) printf("  <-- this driver");
        printf("\n");
    }
}

static MIDIEndpointRef find_ours(bool source)
{
    ItemCount count = source ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < count; i++) {
        MIDIEndpointRef ep = source ? MIDIGetSource(i) : MIDIGetDestination(i);
        if (is_ours(ep)) return ep;
    }
    return 0;
}

static int cmd_list(void)
{
    list_side("Sources", MIDIGetNumberOfSources(), MIDIGetSource);
    printf("\n");
    list_side("Destinations", MIDIGetNumberOfDestinations(), MIDIGetDestination);

    if (!find_ours(true) && !find_ours(false)) {
        printf("\nno endpoints from %s -- is the driver installed and the\n"
               "device plugged in?\n", DRIVER_ID);
        return 1;
    }
    return 0;
}

static int cmd_send(MIDIClientRef client)
{
    MIDIEndpointRef dest = find_ours(false);
    if (!dest) {
        fprintf(stderr, "error: no destination from %s\n", DRIVER_ID);
        return 1;
    }

    MIDIPortRef port = 0;
    if (MIDIOutputPortCreate(client, CFSTR("out"), &port) != noErr) {
        fprintf(stderr, "error: MIDIOutputPortCreate failed\n");
        return 1;
    }

    struct { MIDIPacketList list; uint8_t space[64]; } storage;
    const uint8_t on[]  = { 0x90, 0x3c, 0x40 };
    const uint8_t off[] = { 0x80, 0x3c, 0x00 };

    MIDIPacket* packet = MIDIPacketListInit(&storage.list);
    packet = MIDIPacketListAdd(&storage.list, sizeof storage, packet, 0, sizeof on, on);
    OSStatus rc = MIDISend(port, dest, &storage.list);

    usleep(200 * 1000);

    packet = MIDIPacketListInit(&storage.list);
    packet = MIDIPacketListAdd(&storage.list, sizeof storage, packet, 0, sizeof off, off);
    if (rc == noErr) rc = MIDISend(port, dest, &storage.list);

    if (rc != noErr) {
        fprintf(stderr, "error: MIDISend: %d\n", (int)rc);
        return 1;
    }
    printf("sent note on + off (middle C) to ");
    print_cfstring(endpoint_string(dest, kMIDIPropertyDisplayName));
    printf("\n");
    return 0;
}

static void read_proc(const MIDIPacketList* pktlist, void* refcon, void* connRefCon)
{
    (void)refcon; (void)connRefCon;
    const MIDIPacket* packet = &pktlist->packet[0];
    for (UInt32 p = 0; p < pktlist->numPackets; p++) {
        printf("  ");
        for (UInt16 i = 0; i < packet->length; i++) {
            printf("%02x ", packet->data[i]);
        }
        printf("\n");
        fflush(stdout);
        packet = MIDIPacketNext(packet);
    }
}

static int cmd_dump(MIDIClientRef client, int seconds)
{
    MIDIEndpointRef source = find_ours(true);
    if (!source) {
        fprintf(stderr, "error: no source from %s\n", DRIVER_ID);
        return 1;
    }

    MIDIPortRef port = 0;
    if (MIDIInputPortCreate(client, CFSTR("in"), read_proc, NULL, &port) != noErr ||
        MIDIPortConnectSource(port, source, NULL) != noErr) {
        fprintf(stderr, "error: input port setup failed\n");
        return 1;
    }

    printf("listening on ");
    print_cfstring(endpoint_string(source, kMIDIPropertyDisplayName));
    printf(" for %d s...\n", seconds);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    return 0;
}

int main(int argc, char** argv)
{
    MIDIClientRef client = 0;
    if (MIDIClientCreate(CFSTR("midi-check"), NULL, NULL, &client) != noErr) {
        fprintf(stderr, "error: MIDIClientCreate failed\n");
        return 1;
    }

    if (argc < 2) return cmd_list();
    if (strcmp(argv[1], "send") == 0) return cmd_send(client);
    if (strcmp(argv[1], "dump") == 0) {
        return cmd_dump(client, argc > 2 ? atoi(argv[2]) : 10);
    }

    fprintf(stderr, "usage: midi-check [send | dump [secs]]\n");
    return 2;
}

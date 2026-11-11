// SPDX-License-Identifier: BSD-2-Clause-Patent
// Empty IOAcceleratorES for the firmware-backed CPU display.
//
// backboardd matches IOAcceleratorES before CoreAnimation composes a frame;
// with no such service the UI never draws, even though the GOP framebuffer
// works. The CPU fallback derives from the original IOAcceleratorES: it
// answers every user-client call with success and zeroed outputs and
// advertises no MetalPluginName, which leaves CoreAnimation on its software
// path. The same approach brought mondrian to the iOS main UI.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/proc.h>
#include "OriginalAcceleratorABI.h"
#include "PicassoSpmi.h"

class PicassoAcceleratorUserClient : public IOUserClient {
    OSDeclareDefaultStructors(PicassoAcceleratorUserClient);
public:
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) override;
};

OSDefineMetaClassAndStructors(PicassoAcceleratorUserClient, IOUserClient);

IOReturn PicassoAcceleratorUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}

IOReturn PicassoAcceleratorUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *, OSObject *, void *) {
    static unsigned calls;
    for (uint32_t i = 0; i < args->scalarOutputCount && i < 16; i++) args->scalarOutput[i] = 0;
    if (args->structureOutput && args->structureOutputSize) bzero(args->structureOutput, args->structureOutputSize);
    // The first selectors are enough to see what the software path still asks for.
    if (__atomic_fetch_add(&calls, 1, __ATOMIC_RELAXED) < 24) {
        char name[32] = "?";
        proc_selfname(name, sizeof(name));
        IOLog("PICASSO_ACCEL %s selector=%u in=%u out=%u answered by the empty accelerator\n",
            name, selector, args->scalarInputCount, args->scalarOutputCount);
    }
    return kIOReturnSuccess;
}

class PicassoCpuAccelerator : public IOAcceleratorES {
    OSDeclareDefaultStructors(PicassoCpuAccelerator);
public:
    bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PicassoCpuAccelerator, IOAcceleratorES);

bool PicassoCpuAccelerator::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    setProperty(kIOUserClientClassKey, "PicassoAcceleratorUserClient");
    setProperty("IOAcceleratorStub", kOSBooleanTrue);
    registerService();
    return true;
}

extern "C" IOService *picasso_accelerator_allocate(void) {
    return OSTypeAlloc(PicassoCpuAccelerator);
}

// A fixed IOPMPowerSource: a full battery on a USB-C charger. powerd finds
// every IOPMPowerSource and builds the power sources list from its
// properties; with none, iOS takes the phone for one on battery, and
// SBDontDimOrLockOnAC (set for a board without input) never applies, so the
// lock screen goes dark. The battery itself is not read: Picasso has no fuel
// gauge driver yet. Shape after HackiOS's HakiosBattery.
class PicassoPowerSource : public IOPMPowerSource {
    OSDeclareDefaultStructors(PicassoPowerSource);
public:
    void publishFixedState();
private:
    void number(const char *key, unsigned long long value);
};

OSDefineMetaClassAndStructors(PicassoPowerSource, IOPMPowerSource);

void PicassoPowerSource::number(const char *key, unsigned long long value) {
    const OSSymbol *symbol = OSSymbol::withCString(key);
    OSNumber *n = OSNumber::withNumber(value, 32);
    if (symbol && n) setPSProperty(symbol, n);
    OSSafeReleaseNULL(n);
    OSSafeReleaseNULL(symbol);
}

// ---- battery -------------------------------------------------------------------
// PM7250B's VADC through the SPMI arbiter. Two rules carried over from the standalone driver,
// because both were learned the hard way:
//   1. The two windows are NOT interchangeable. PicassoBacklight.cpp maps the observer window
//      (0xe600000 + slot*0x80) for reads and the writer window (0xc600000 + slot*0x10000) for
//      writes. Writing through the observer window reaches nothing, so the conversion would
//      never complete.
//   2. Nothing is written before the slot has been shown to answer and to be the page we asked
//      for. The slot comes from the arbiter's own mapping table (never computed from the PPID),
//      and a duplicate match is refused rather than resolved.
// If any step disagrees with expectations the fixed properties stay and one PICASSO_BATT line
// says which step refused.
namespace {

enum : uint8_t {
    regPerphType = 0x04, regPerphSubtype = 0x05,
    regStatus1 = 0x08, regDigParam = 0x42, regChannel = 0x44, regEnCtl1 = 0x46,
    regConvReq = 0x47, regData0 = 0x50, regData1 = 0x51,
    statusEoc = 1u << 0, statusConvFault = 1u << 7, enAdc = 1u << 7, convRequest = 1u << 7,
    channelGnd = 0x00, channelRef = 0x01, channelVbat = 0x84,
};

enum : unsigned {
    ppidVadc = 0x231,           // PM7250B (USID 2), page 0x31: a page number, not a slot
    ppidSamePageUsid0 = 0x031,  // PM8150's page of the same number: logged, never used
    apidSlots = 512, mapOffset = 0x900, ownerOffset = 0x700, apidStride = 0x80,
};

enum : int { apidAbsent = -1, apidAmbiguous = -2 };

bool mappingNames(uint32_t mapping, unsigned ppid) {
    return mapping != 0 && ((mapping >> 8) & 0xfff) == ppid;
}
bool ownerIsApps(uint32_t owner) { return (owner & 7) == 0; }

struct VadcMap {
    IOMemoryMap *map = nullptr;
    volatile uint32_t *va = nullptr;
    bool open(uint64_t base, uint64_t size) {
        IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
        map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        va = map ? reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()) : nullptr;
        return va != nullptr;
    }
    ~VadcMap() { OSSafeReleaseNULL(map); }
};

struct VadcWindows {
    VadcMap observer, writer;
    bool open(unsigned slot) {
        return observer.open(0xe600000ull + uint64_t(slot) * apidStride, apidStride) &&
               writer.open(0xc600000ull + uint64_t(slot) * 0x10000, 0x1000);
    }
};

int findSlot(volatile uint32_t *core, volatile uint32_t *owners, unsigned ppid) {
    int found = apidAbsent;
    for (unsigned slot = 0; slot < apidSlots; ++slot) {
        if (!mappingNames(core[(mapOffset + slot * 4) / 4], ppid)) continue;
        if (!ownerIsApps(owners[(ownerOffset + slot * 4) / 4])) continue;
        if (found != apidAbsent) return apidAmbiguous;
        found = int(slot);
    }
    return found;
}

// Read-only. Says which slot each page resolved to and whether it answers; two converters share
// this page number (PM8150 at USID 0, PM7250B at USID 2), so the log has to show both.
void describePage(volatile uint32_t *core, volatile uint32_t *owners, unsigned ppid, const char *label) {
    const int slot = findSlot(core, owners, ppid);
    if (slot < 0) {
        IOLog("PICASSO_BATT page %s ppid=%#x slot=%s\n", label, ppid,
            slot == apidAmbiguous ? "ambiguous" : "absent");
        return;
    }
    VadcMap observer;
    if (!observer.open(0xe600000ull + uint64_t(slot) * apidStride, apidStride)) {
        IOLog("PICASSO_BATT page %s ppid=%#x slot=%d window=failed\n", label, ppid, slot);
        return;
    }
    uint8_t type = 0, subtype = 0;
    const int typeError = picasso_spmi_read8(observer.va, regPerphType, type);
    const int subtypeError = picasso_spmi_read8(observer.va, regPerphSubtype, subtype);
    IOLog("PICASSO_BATT page %s ppid=%#x slot=%d type=%02x/%d subtype=%02x/%d\n",
        label, ppid, slot, type, typeError, subtype, subtypeError);
}

// One conversion. The first read happens before any write: a slot that answers 0xff is not
// there, and writing to it would be a guess about someone else's peripheral.
bool convertChannel(VadcWindows &bus, uint8_t channel, unsigned &code) {
    uint8_t status = 0;
    if (picasso_spmi_read8(bus.observer.va, regStatus1, status)) return false;
    if (status == 0xff) return false;
    if (picasso_spmi_write8(bus.writer.va, regDigParam, 0)) return false;
    if (picasso_spmi_write8(bus.writer.va, regChannel, channel)) return false;
    if (picasso_spmi_write8(bus.writer.va, regEnCtl1, enAdc)) return false;
    if (picasso_spmi_write8(bus.writer.va, regConvReq, convRequest)) return false;
    for (unsigned poll = 0; poll < 20; ++poll) {
        // The second read, after the enable: catches a dead slot and a converter that faults.
        if (picasso_spmi_read8(bus.observer.va, regStatus1, status)) return false;
        if (status == 0xff) return false;
        if (status & statusConvFault) return false;
        if (status & statusEoc) {
            uint8_t lsb = 0, msb = 0;
            if (picasso_spmi_read8(bus.observer.va, regData0, lsb)) return false;
            if (picasso_spmi_read8(bus.observer.va, regData1, msb)) return false;
            code = (unsigned(msb) << 8) | lsb;
            return true;
        }
        IODelay(100);
    }
    return false;
}

// Two-point calibration from the converter's own references, then undo the 1/3 divider.
bool millivolts(unsigned code, unsigned gnd, unsigned reference, unsigned &out) {
    if (reference <= gnd || reference - gnd < 2000 || code <= gnd) return false;
    const uint64_t microvolts = uint64_t(code - gnd) * 1250000ull * 3ull / (reference - gnd);
    const unsigned value = unsigned(microvolts / 1000);
    if (value < 3000 || value > 4600) return false;
    out = value;
    return true;
}

// Coarse Li-ion curve: the points this log can be checked against.
int percentFor(unsigned mv) {
    static const struct { unsigned millivolts; int percent; } curve[] = {
        {4300, 100}, {4200, 95}, {4100, 88}, {4000, 78}, {3900, 65}, {3800, 52},
        {3750, 45}, {3700, 38}, {3650, 30}, {3600, 22}, {3550, 15}, {3500, 10},
        {3450, 6}, {3400, 3}, {3300, 1}, {3200, 0},
    };
    const unsigned last = unsigned(sizeof(curve) / sizeof(curve[0])) - 1;
    if (mv >= curve[0].millivolts) return 100;
    if (mv <= curve[last].millivolts) return 0;
    for (unsigned i = 1; i <= last; ++i) {
        if (mv < curve[i].millivolts) continue;
        const unsigned high = curve[i - 1].millivolts, low = curve[i].millivolts;
        const int highPercent = curve[i - 1].percent, lowPercent = curve[i].percent;
        return lowPercent + int((mv - low) * unsigned(highPercent - lowPercent) / (high - low));
    }
    return 0;
}

bool readPackMillivolts(unsigned &millivoltsOut) {
    const char *why = "?";
    unsigned gnd = 0, reference = 0, raw = 0;
    int slot = apidAbsent;
    do {
        if (!picasso_spmi_init()) { why = "spmi"; break; }
        VadcMap core, owners;
        if (!core.open(0xc440000, 0x2000) || !owners.open(0xc40a000, 0x1000)) { why = "arbiter"; break; }
        describePage(core.va, owners.va, ppidVadc, "usid2");
        describePage(core.va, owners.va, ppidSamePageUsid0, "usid0");
        slot = findSlot(core.va, owners.va, ppidVadc);
        if (slot < 0) { why = slot == apidAmbiguous ? "ambiguous" : "absent"; break; }
        VadcWindows bus;
        if (!bus.open(unsigned(slot))) { why = "windows"; break; }
        if (!convertChannel(bus, channelGnd, gnd)) { why = "gnd"; break; }
        if (!convertChannel(bus, channelRef, reference)) { why = "ref"; break; }
        if (!convertChannel(bus, channelVbat, raw)) { why = "vbat"; break; }
        if (!millivolts(raw, gnd, reference, millivoltsOut)) { why = "calibration"; break; }
        IOLog("PICASSO_BATT slot=%d gnd=%u ref=%u vbat=%u mv=%u pct=%d\n",
            slot, gnd, reference, raw, millivoltsOut, percentFor(millivoltsOut));
        return true;
    } while (false);
    IOLog("PICASSO_BATT refused reason=%s slot=%d gnd=%u ref=%u vbat=%u\n",
        why, slot, gnd, reference, raw);
    return false;
}

}  // namespace

void PicassoPowerSource::publishFixedState() {
    unsigned packMillivolts = 0;
    const bool measured = readPackMillivolts(packMillivolts);
    setBatteryInstalled(true);
    setExternalConnected(true);
    setExternalChargeCapable(true);
    setIsCharging(false);
    // "Fully charged" is only honest when a reading says so. A failed read must not set it:
    // the previous behaviour (always true) is exactly the bug this replaced.
    if (measured && percentFor(packMillivolts) >= 100) {
        const OSSymbol *full = OSSymbol::withCString("FullyCharged");
        if (full) setPSProperty(full, kOSBooleanTrue);
        OSSafeReleaseNULL(full);
    }
    setCurrentCapacity(measured ? percentFor(packMillivolts) : 100);
    setMaxCapacity(100);
    setVoltage(measured ? packMillivolts : 4400);
    setAmperage(0);
    number("AppleRawCurrentCapacity", measured ? percentFor(packMillivolts) : 4500);
    number("AppleRawMaxCapacity", 4500);
    number("DesignCapacity", 4500);
    OSDictionary *adapter = OSDictionary::withCapacity(4);
    OSNumber *watts = OSNumber::withNumber(18ULL, 32), *family = OSNumber::withNumber(0xe0004009ULL, 32);  // USB-C
    if (adapter && watts && family) {
        adapter->setObject(kIOPMPSAdapterDetailsWattsKey, watts);
        adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, family);
        adapter->setObject("IsWireless", kOSBooleanFalse);
        const OSSymbol *key = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
        if (key) setPSProperty(key, adapter);
        OSSafeReleaseNULL(key);
    }
    OSSafeReleaseNULL(watts); OSSafeReleaseNULL(family); OSSafeReleaseNULL(adapter);
    updateStatus();
}

extern "C" bool picasso_power_source_publish(void) {
    IOService *platform = IOService::getPlatform();
    PicassoPowerSource *source = OSTypeAlloc(PicassoPowerSource);
    if (!platform || !source || !source->init()) { OSSafeReleaseNULL(source); return false; }
    source->publishFixedState();  // the properties are there before anybody finds the service
    bool attached = source->attach(platform);
    if (attached) source->setName("PicassoPowerSource");
    bool started = attached && source->start(platform);
    if (started) source->registerService();
    else if (attached) source->detach(platform);
    OSSafeReleaseNULL(source);  // the registry keeps a published service
    return started;
}

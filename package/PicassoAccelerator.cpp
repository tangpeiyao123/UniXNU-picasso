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
// PM7250B's VADC through the SPMI arbiter. Read-mostly and bounded: any unexpected state
// returns false, one PICASSO_BATT line says why, and the caller keeps the fixed values it
// already published. The slot comes from the arbiter's own mapping table (never computed
// from the PPID), and the two on-chip references (0 V, 1.25 V) are the calibration, so no
// data-sheet constant is involved. Self-contained on purpose: this file is compiled from
// kits/picasso, where a newly added header would not be present.
namespace {

enum : uint8_t {
    regStatus1 = 0x08, regDigParam = 0x42, regChannel = 0x44, regEnCtl1 = 0x46,
    regConvReq = 0x47, regData0 = 0x50, regData1 = 0x51,
    statusEoc = 1u << 0, statusConvFault = 1u << 7, enAdc = 1u << 7, convRequest = 1u << 7,
    channelGnd = 0x00, channelRef = 0x01, channelVbat = 0x84,
};

enum : unsigned {
    ppidVadc = 0x231,          // PM7250B (USID 2), page 0x31: a page number, not a slot
    apidSlots = 512, mapOffset = 0x900, ownerOffset = 0x700, apidStride = 0x80,
};

enum : int { apidAbsent = -1, apidAmbiguous = -2 };

bool mappingNames(uint32_t mapping) { return mapping != 0 && ((mapping >> 8) & 0xfff) == ppidVadc; }
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

// One conversion, in the ADC5 order: parameters, enable, channel, request.
bool convertChannel(volatile uint32_t *window, uint8_t channel, unsigned &code) {
    if (picasso_spmi_write8(window, regDigParam, 0)) return false;
    if (picasso_spmi_write8(window, regEnCtl1, enAdc)) return false;
    if (picasso_spmi_write8(window, regChannel, channel)) return false;
    if (picasso_spmi_write8(window, regConvReq, convRequest)) return false;
    for (unsigned poll = 0; poll < 20; ++poll) {
        uint8_t status = 0;
        if (picasso_spmi_read8(window, regStatus1, status)) return false;
        if (status & statusConvFault) return false;
        if (status & statusEoc) {
            uint8_t lsb = 0, msb = 0;
            if (picasso_spmi_read8(window, regData0, lsb)) return false;
            if (picasso_spmi_read8(window, regData1, msb)) return false;
            code = (unsigned(msb) << 8) | lsb;
            return true;
        }
        IODelay(100);
    }
    return false;
}

// Two slots claiming the same page is a reason to stop: this path writes to the slot.
int findVadcSlot(volatile uint32_t *core, volatile uint32_t *owners) {
    int found = apidAbsent;
    for (unsigned slot = 0; slot < apidSlots; ++slot) {
        if (!mappingNames(core[(mapOffset + slot * 4) / 4])) continue;
        if (!ownerIsApps(owners[(ownerOffset + slot * 4) / 4])) continue;
        if (found != apidAbsent) return apidAmbiguous;
        found = int(slot);
    }
    return found;
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

// Coarse Li-ion curve. Fine-grained accuracy would only pretend; these are the points the
// log can be checked against.
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
        VadcMap core, owners, block;
        if (!core.open(0xc440000, 0x2000) || !owners.open(0xc40a000, 0x1000)) { why = "arbiter"; break; }
        slot = findVadcSlot(core.va, owners.va);
        if (slot < 0) { why = slot == apidAmbiguous ? "ambiguous" : "absent"; break; }
        if (!block.open(0xe600000ull + uint64_t(slot) * apidStride, apidStride)) { why = "window"; break; }
        if (!convertChannel(block.va, channelGnd, gnd)) { why = "gnd"; break; }
        if (!convertChannel(block.va, channelRef, reference)) { why = "ref"; break; }
        if (!convertChannel(block.va, channelVbat, raw)) { why = "vbat"; break; }
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
    // "Fully charged" is only honest when the measurement says so; with no reading we
    // keep the previous fixed state rather than invent one.
    const OSSymbol *full = OSSymbol::withCString("FullyCharged");
    if (full && (!measured || percentFor(packMillivolts) >= 100))
        setPSProperty(full, kOSBooleanTrue);
    OSSafeReleaseNULL(full);
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

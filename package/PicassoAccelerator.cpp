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
#include "PicassoBatteryCore.h"
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
// PM7250B's VADC, reached through the SPMI arbiter. Every step here is bounded and
// read-mostly: any unexpected state returns false, the reason is logged once, and the
// caller keeps the fixed properties it published before. Nothing is guessed: the slot
// is taken from the arbiter's own mapping table (never computed from the PPID), and the
// two on-chip references (0 V, 1.25 V) provide the calibration, so no data-sheet
// constant is involved.
namespace {

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
    using namespace picasso_battery::core;
    if (picasso_spmi_write8(window, regDigParam, 0)) return false;
    if (picasso_spmi_write8(window, regEnCtl1, enAdc)) return false;
    if (picasso_spmi_write8(window, regChannel, channel)) return false;
    if (picasso_spmi_write8(window, regConvReq, convRequest)) return false;
    for (unsigned poll = 0; poll < 20; ++poll) {
        uint8_t status = 0;
        if (picasso_spmi_read8(window, regStatus1, status)) return false;
        if (status & picasso_battery::core::statusConvFault) return false;
        if (status & picasso_battery::core::statusEoc) {
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
    using namespace picasso_battery::core;
    int found = apidAbsent;
    for (unsigned slot = 0; slot < apidSlots; ++slot) {
        const uint32_t mapping = core[(mapOffset + slot * 4) / 4];
        if (!mappingNames(mapping, ppidVadc)) continue;
        if (!ownerIsApps(owners[(ownerOffset + slot * 4) / 4])) continue;
        if (found != apidAbsent) return apidAmbiguous;
        found = int(slot);
    }
    return found;
}

bool readPackMillivolts(unsigned &millivoltsOut) {
    using namespace picasso_battery::core;
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
        Scale scale;
        if (!scaleFrom(gnd, reference, scale)) { why = "calibration"; break; }
        if (!millivolts(raw, scale, millivoltsOut)) { why = "range"; break; }
        IOLog("PICASSO_BATT slot=%d gnd=%u ref=%u vbat=%u mv=%u pct=%d\n",
            slot, gnd, reference, raw, millivoltsOut, percentFor(millivoltsOut));
        return true;
    } while (false);
    IOLog("PICASSO_BATT refused reason=%s slot=%d gnd=%u ref=%u vbat=%u\n", why, slot, gnd, reference, raw);
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
    if (full && (!measured || picasso_battery::core::percentFor(packMillivolts) >= 100))
        setPSProperty(full, kOSBooleanTrue);
    OSSafeReleaseNULL(full);
    setCurrentCapacity(measured ? picasso_battery::core::percentFor(packMillivolts) : 100);
    setMaxCapacity(100);
    setVoltage(measured ? packMillivolts : 4400);
    setAmperage(0);
    number("AppleRawCurrentCapacity", measured ? picasso_battery::core::percentFor(packMillivolts) : 4500);
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

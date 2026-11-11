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
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <pexpert/pexpert.h>
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
    IOWorkLoop *workLoop = nullptr;
    IOTimerEventSource *timer = nullptr;
    unsigned lastChargerStatus = 0xffffffffu;   // 0xffffffff = nothing read yet
    unsigned pollsSinceFull = 0;
    unsigned lastGoodMillivolts = 0;            // last successfully converted pack voltage
    int lastGoodPercent = 100;
    bool haveReading = false;
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void publishFixedState();
private:
    void refreshState();
    void refreshTimer(OSObject *, IOTimerEventSource *);
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
    channelUsbInI = 0x07, channelUsbInV = 0x08,   // PM7250B VADC: usb_in_i_uv@7, usb_in_v_div_16@8
};

enum : unsigned {
    ppidVadc = 0x231,           // PM7250B (USID 2), page 0x31: a page number, not a slot
    ppidSamePageUsid0 = 0x031,  // PM8150's page of the same number: logged, never used
    ppidChgr = 0x210,           // PM7250B (USID 2), SMB5 CHGR page 0x10: a page number, not a slot
    apidSlots = 512, mapOffset = 0x900, ownerOffset = 0x700, apidStride = 0x80,
};

enum : int { apidAbsent = -1, apidAmbiguous = -2 };

// Slots resolved from the arbiter's mapping table never move while the system is up, so they are
// cached after the first success: the 512-slot scan and the two describePage() probes used to run
// on every single refresh, which put a pointless burst of arbiter reads on the work loop.
int cachedVadcSlot = apidAbsent;
int cachedChgrSlot = apidAbsent;
bool pagesDescribed = false;

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

// Two-point calibration from the converter's own references, then undo the channel's
// pre-scale divider: vbat_sns = 1/3, usb_in_v_div_16 = 1/16, usb_in_i_uv = 1/1.
bool scaledMicrovolts(unsigned code, unsigned gnd, unsigned reference, unsigned scale, uint64_t &out) {
    if (reference <= gnd || reference - gnd < 2000 || code <= gnd) return false;
    out = uint64_t(code - gnd) * 1250000ull * scale / (reference - gnd);
    return true;
}

bool millivolts(unsigned code, unsigned gnd, unsigned reference, unsigned &out) {
    uint64_t microvolts = 0;
    if (!scaledMicrovolts(code, gnd, reference, 3, microvolts)) return false;
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
        if (!pagesDescribed) {
            pagesDescribed = true;
            describePage(core.va, owners.va, ppidVadc, "usid2");
            describePage(core.va, owners.va, ppidSamePageUsid0, "usid0");
        }
        if (cachedVadcSlot == apidAbsent) cachedVadcSlot = findSlot(core.va, owners.va, ppidVadc);
        slot = cachedVadcSlot;
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

// USB presence. usb_in_v_div_16 reads the VBUS rail divided by 16, so a calibrated value well
// above 3 V means the port is powered. usb_in_i_uv is the current-sense channel (1:1); its raw
// ADC code is returned for the log only — the sign/scale against the sense resistor is not yet
// decoded, so amperage stays 0 and isCharging stays false until SMB5 chgr@1000 is decoded.
bool readUsbState(unsigned &usbMillivolts, unsigned &senseCode) {
    unsigned gnd = 0, reference = 0, usbV = 0, usbI = 0;
    if (!picasso_spmi_init()) return false;
    VadcMap core, owners;
    if (!core.open(0xc440000, 0x2000) || !owners.open(0xc40a000, 0x1000)) return false;
    if (cachedVadcSlot == apidAbsent) cachedVadcSlot = findSlot(core.va, owners.va, ppidVadc);
    const int slot = cachedVadcSlot;
    if (slot < 0) return false;
    VadcWindows bus;
    if (!bus.open(unsigned(slot))) return false;
    if (!convertChannel(bus, channelGnd, gnd)) return false;
    if (!convertChannel(bus, channelRef, reference)) return false;
    if (!convertChannel(bus, channelUsbInV, usbV)) return false;
    if (!convertChannel(bus, channelUsbInI, usbI)) return false;
    uint64_t microvolts = 0;
    if (!scaledMicrovolts(usbV, gnd, reference, 16, microvolts)) return false;
    usbMillivolts = unsigned(microvolts / 1000);
    senseCode = usbI;
    return true;
}

// SMB5 charger state machine. BATTERY_CHARGER_STATUS_1 (CHGR base 0x1000 + 0x06) low 3 bits:
//   0=INHIBIT 1=TRICKLE 2=PRE 3=FULLON 4=TAPER 5=TERMINATE 6=PAUSE 7=DISABLE.
// 1..4 mean the battery is actually charging. This is a hardware state machine: SMB5 charges on
// its own once a valid input is present, no software enable is required, so this read-only
// register reports the true charge state. Register offsets from Linux smb5-reg.h.
bool readChargerStatus(unsigned &statusOut) {
    if (!picasso_spmi_init()) return false;
    VadcMap core, owners;
    if (!core.open(0xc440000, 0x2000) || !owners.open(0xc40a000, 0x1000)) return false;
    if (cachedChgrSlot == apidAbsent) {
        cachedChgrSlot = findSlot(core.va, owners.va, ppidChgr);
        if (cachedChgrSlot >= 0) {
            IOLog("PICASSO_BATT chgr slot=%d (page 0x10, SMB5 BATTERY_CHARGER_STATUS_1 @0x06)\n",
                cachedChgrSlot);
        } else {
            IOLog("PICASSO_BATT chgr slot=%s ppid=%#x\n",
                cachedChgrSlot == apidAmbiguous ? "ambiguous" : "absent", ppidChgr);
        }
    }
    const int slot = cachedChgrSlot;
    if (slot < 0) return false;
    VadcMap observer;
    if (!observer.open(0xe600000ull + uint64_t(slot) * apidStride, apidStride)) return false;
    uint8_t status = 0;
    if (picasso_spmi_read8(observer.va, 0x06, status)) return false;   // BATTERY_CHARGER_STATUS_1
    statusOut = status & 0x7u;
    return true;
}

}  // namespace

void PicassoPowerSource::refreshState() {
    unsigned packMillivolts = 0, usbMillivolts = 0, senseCode = 0;
    const bool measured = readPackMillivolts(packMillivolts);
    const bool usb = readUsbState(usbMillivolts, senseCode);
    const bool powered = usb && usbMillivolts >= 3000;   // VBUS rail present
    const bool full = measured && percentFor(packMillivolts) >= 100;
    // A failed conversion must not invent a state: before this, one bad read published 100 %/4400 mV
    // and yanked the UI to a full battery. Keep the last good reading instead.
    if (measured) {
        lastGoodMillivolts = packMillivolts;
        lastGoodPercent = percentFor(packMillivolts);
        haveReading = true;
    }
    const unsigned shownMillivolts = measured ? packMillivolts : (haveReading ? lastGoodMillivolts : 4400);
    const int shownPercent = measured ? percentFor(packMillivolts) : (haveReading ? lastGoodPercent : 100);
    // True charge state from SMB5's hardware state machine (BATTERY_CHARGER_STATUS_1, low 3 bits).
    // 1..4 (TRICKLE/PRE/FULLON/TAPER) mean the battery is charging; SMB5 decides on its own whether
    // the input (SDP/CDP/DCP) can charge, so no VBUS approximation is needed.
    unsigned chargerStatus = 0;
    const bool hasCharger = readChargerStatus(chargerStatus);
    // Charging needs both halves to agree. CHGR alone said "3" on a cable that had just been pulled
    // (usb=0 powered=0 charging=1 in the log), so a *valid* VBUS reading that says "no rail" vetoes
    // it. A failed VBUS read must not veto anything: that would drop the bolt on a transient error.
    const bool charging = hasCharger && chargerStatus >= 1 && chargerStatus <= 4 && (!usb || powered);

    setBatteryInstalled(true);
    setExternalConnected(powered);
    setExternalChargeCapable(powered);
    setIsCharging(charging);

    // "Fully charged" only when a reading actually says so; clear it otherwise so the
    // UI drops back out of the "full" state once the pack discharges below the top.
    const OSSymbol *fullKey = OSSymbol::withCString("FullyCharged");
    if (fullKey) {
        if (full) setPSProperty(fullKey, kOSBooleanTrue);
        else setPSProperty(fullKey, kOSBooleanFalse);
        OSSafeReleaseNULL(fullKey);
    }

    setCurrentCapacity(shownPercent);
    setMaxCapacity(100);
    setVoltage(shownMillivolts);
    setAmperage(0);   // current sense resistor not calibrated yet
    number("AppleRawCurrentCapacity", 4500ull * unsigned(shownPercent) / 100);
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

    // ok= says whether this round actually re-read the pack; shown mv/pct are what got published
    // (last good reading when ok=0), so the log can never be read as "the UI jumped".
    IOLog("PICASSO_BATT state ok=%d mv=%u pct=%d usb=%umV powered=%d charging=%d chg_status=%u sense_code=%u\n",
        measured ? 1 : 0, shownMillivolts, shownPercent,
        usb ? usbMillivolts : 0, powered ? 1 : 0, charging ? 1 : 0,
        hasCharger ? chargerStatus : 8u, senseCode);
    updateStatus();
}

void PicassoPowerSource::publishFixedState() {
    // Boot-path bootstrap only: fixed fallbacks, no VADC reads here. Seven conversions on the
    // service-publish critical path would stall early boot. The first real read happens on the
    // timer shortly after start().
    setBatteryInstalled(true);
    setExternalConnected(true);   // fallback: this board is normally USB-powered
    setExternalChargeCapable(true);
    setIsCharging(false);
    setCurrentCapacity(100);
    setMaxCapacity(100);
    setVoltage(4400);
    setAmperage(0);
    number("AppleRawCurrentCapacity", 4500);
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

bool PicassoPowerSource::start(IOService *provider) {
    if (!IOPMPowerSource::start(provider)) return false;
    workLoop = IOWorkLoop::workLoop();
    if (!workLoop) return false;
    timer = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this, &PicassoPowerSource::refreshTimer));
    if (!timer || workLoop->addEventSource(timer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(timer);
        OSSafeReleaseNULL(workLoop);
        return false;
    }
    // A/B switch: `sheng-batt=0` keeps the periodic reads off entirely (fixed properties only), so
    // one binary can be compared with and without the SPMI polling. The d2 cluster sitting at
    // busy=100 % appeared together with the charger read; this is the single-variable test that can
    // settle whether the polling is responsible, without burning another build.
    unsigned battEnable = 1;
    PE_parse_boot_argn("sheng-batt", &battEnable, sizeof battEnable);
    if (battEnable == 0) {
        IOLog("PICASSO_BATT timer disabled (sheng-batt=0)\n");
        return true;
    }
    // First real read ~1s after boot (publishFixedState only set fallbacks to keep the early
    // boot path light); refreshTimer then ticks every 2 s and re-reads the pack on any change.
    // SPMI reads are safe here: init is idempotent and read/write take the controller IRQ lock
    // only for the register access.
    timer->setTimeoutMS(1000);
    return true;
}

void PicassoPowerSource::stop(IOService *provider) {
    if (timer) {
        timer->cancelTimeout();
        if (workLoop) workLoop->removeEventSource(timer);
        OSSafeReleaseNULL(timer);
    }
    OSSafeReleaseNULL(workLoop);
    IOPMPowerSource::stop(provider);
}

void PicassoPowerSource::refreshTimer(OSObject *, IOTimerEventSource *sender) {
    // Cheap 2 s tick: one read of the charger status register (no VADC conversion). A plug/unplug or
    // a charge-stage change re-reads the whole pack immediately, so the charge bolt follows the
    // cable within a couple of seconds. A full sweep still happens every ~30 s so the percentage
    // tracks discharge. Doing all 11 conversions every 2 s would put needless work on the work loop.
    unsigned status = 0;
    const bool haveStatus = readChargerStatus(status);
    bool refresh = (lastChargerStatus == 0xffffffffu);
    if (haveStatus) {
        if (status != lastChargerStatus) refresh = true;
        lastChargerStatus = status;
    }
    if (++pollsSinceFull >= 15) refresh = true;
    if (refresh) {
        pollsSinceFull = 0;
        refreshState();
    }
    if (sender) sender->setTimeoutMS(2000);   // re-arm via the parameter, not the member
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

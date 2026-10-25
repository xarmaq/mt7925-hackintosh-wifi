// <wtl> project: mtkwlan | file: src/MtkWlan.cpp | author: watchthelight </wtl>
// SPDX-License-Identifier: ISC
// W1 bring-up, following mt7925_pci_probe() in Linux mt76 v6.18 (mt7925/pci.c) and the
// ownership handshakes in mt792x_core.c (Copyright (C) 2023 MediaTek Inc., ISC):
//   memory + bus master on, BAR0 mapped, fw_own then drv_own, read chip ID and revision.
// Then W2/W3 (MtkFw.cpp): WFSYS reset, WFDMA rings, MSI, patch + RAM firmware download.
// 0.8.0 (W0): the core is a plain IOService with its own work loop; the Ethernet front end (MtkWlanEth)
// is created as a child in netStage() and shares that work loop.
// 0.9.0 (W1): no hard-coded SSID. After the firmware runs, hwInit() readies the station and netStage() starts
// the front end (link down) and the tick; the connection manager scans and joins from there. MtkAutoOpenSsids
// (Info.plist) lists the open networks it may join on its own; everything else comes through the user client.

#include "MtkWlan.hpp"
#include "MtkCrypto.h"
#include <pexpert/pexpert.h>
#include <IOKit/IOMessage.h>

OSDefineMetaClassAndStructors(MtkWlan, IOService)

namespace {
// mt792x_regs.h / mt7925/regs.h
constexpr uint32_t MT_CONN_ON_LPCTL         = 0x7c060010;
constexpr uint32_t PCIE_LPCR_HOST_SET_OWN   = 1u << 0;
constexpr uint32_t PCIE_LPCR_HOST_CLR_OWN   = 1u << 1;
constexpr uint32_t PCIE_LPCR_HOST_OWN_SYNC  = 1u << 2;
constexpr uint32_t MT_CONN_ON_MISC          = 0x7c0600f0;
constexpr uint32_t MT_HW_CHIPID             = 0x70010200;
constexpr uint32_t MT_HW_REV                = 0x70010204;
constexpr uint32_t MT_WFSYS_SW_RST_B        = 0x7c000140;
constexpr int      MT792x_DRV_OWN_RETRY_COUNT = 10;
} // namespace

void MtkWlan::step(const char *state) {
    setProperty("MtkState", state);
    IOLog("MtkWlan: %s\n", state);
}

void MtkWlan::setNum(const char *key, uint64_t val, unsigned bits) {
    setProperty(key, val, bits);
}

// mt792xe_mcu_fw_pmctrl: hand the chip to the firmware (SET_OWN, wait for OWN_SYNC = 1)
int MtkWlan::fwOwn() {
    for (int i = 0; i < MT792x_DRV_OWN_RETRY_COUNT; i++) {
        wr(MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_SET_OWN);
        if (pollMsec(MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_OWN_SYNC, PCIE_LPCR_HOST_OWN_SYNC, 50, 1))
            return i + 1;
    }
    return -1;
}

// __mt792xe_mcu_drv_pmctrl: take the chip back for the driver (CLR_OWN, wait for OWN_SYNC = 0)
int MtkWlan::drvOwn() {
    for (int i = 0; i < MT792x_DRV_OWN_RETRY_COUNT; i++) {
        wr(MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_CLR_OWN);
        if (pollMsec(MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_OWN_SYNC, 0, 50, 1))
            return i + 1;
    }
    return -1;
}

// Which interrupt sources IOPCIFamily offers for the card (MSI is what W2 needs)
void MtkWlan::listInterrupts() {
    char buf[96];
    int len = 0;
    buf[0] = 0;
    for (int i = 0; i < 8; i++) {
        int type = 0;
        if (pci->getInterruptType(i, &type) != kIOReturnSuccess)
            break;
        len += snprintf(buf + len, sizeof(buf) - len, "%s%d:%s", i ? " " : "", i,
                        (type & kIOInterruptTypePCIMessaged) ? "msi" :
                        (type & kIOInterruptTypeLevel) ? "level" : "edge");
        if (len >= (int)sizeof(buf))
            break;
    }
    setProperty("MtkInterrupts", buf[0] ? buf : "none");
}

bool MtkWlan::start(IOService *provider) {
    if (!IOService::start(provider))
        return false;
    // 0.7.0 got its work loop from IONetworkController::start (createWorkLoop); the core makes its own
    workLoop = IOWorkLoop::workLoop();
    if (!workLoop) {
        IOService::stop(provider);
        return false;
    }

    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        step("W1 fail: provider is not a PCI device");
        return true; // stay attached so the state is visible
    }
    pci->retain();
    // IONetworkingFamily takes the interface's IOBuiltin from the controller's provider, which is now this
    // core and no longer the PCI device: carry the device's "built-in" over so en1 stays built-in
    if (OSObject *builtin = pci->getProperty("built-in")) setProperty("built-in", builtin);
    // open networks the connection manager may join by itself (SSID prefixes); none = never
    if (OSArray *list = OSDynamicCast(OSArray, getProperty("MtkAutoOpenSsids"))) {
        for (unsigned i = 0; i < list->getCount() && nAutoOpenSsids < 4; i++) {
            OSString *s = OSDynamicCast(OSString, list->getObject(i));
            if (!s || !s->getLength() || s->getLength() > 32) continue;
            strlcpy(autoOpenSsids[nAutoOpenSsids++], s->getCStringNoCopy(), 33);
        }
    }
    // MtkAutoNetworks: WPA2 networks for a machine without the native menu (installer, other macOS builds); each entry
    // {Ssid, Pmk} with the PMK as 64 hex digits (mtkwifi pmk prints one), never a passphrase
    if (OSArray *list = OSDynamicCast(OSArray, getProperty("MtkAutoNetworks"))) {
        for (unsigned i = 0; i < list->getCount() && nAutoNets < 4; i++) {
            OSDictionary *d = OSDynamicCast(OSDictionary, list->getObject(i));
            OSString *ssid = d ? OSDynamicCast(OSString, d->getObject("Ssid")) : nullptr;
            OSString *pmk = d ? OSDynamicCast(OSString, d->getObject("Pmk")) : nullptr;
            if (!ssid || !pmk || !ssid->getLength() || ssid->getLength() > 32) continue;
            AutoNet &n = autoNets[nAutoNets];
            if (mtk_hex_pmk(pmk->getCStringNoCopy(), n.pmk)) continue;
            n.ssidLen = (uint8_t)ssid->getLength();
            memcpy(n.ssid, ssid->getCStringNoCopy(), n.ssidLen);
            nAutoNets++;
        }
        removeProperty("MtkAutoNetworks");      // the keys do not stay readable in the registry
        setNum("MtkAutoNetworkCount", nAutoNets);
    }
    autoOpen = autoAllowed();
    // link supervisor (I-096): seconds of a dead link before it acts, 0 = off; boot-arg mtkstall= overrides
    uint32_t stallSec = 8;
    if (OSNumber *n = OSDynamicCast(OSNumber, getProperty("MtkStallSec"))) stallSec = n->unsigned32BitValue();
    PE_parse_boot_argn("mtkstall", &stallSec, sizeof(stallSec));
    if (stallSec > 600) stallSec = 600;
    mtk_super_init(&super, stallSec * 1000);
    setNum("MtkStallSec", stallSec);
    // 0.10.0: which front end owns the data path. MtkFrontEnd "native" waits for MtkWlanNative.kext (falls back to the
    // Ethernet front end if it declines or stays away); boot-arg mtkfe=native|ethernet overrides the plist
    if (OSString *feStr = OSDynamicCast(OSString, getProperty("MtkFrontEnd"))) feNativeWanted = feStr->isEqualTo("native");
    char feArg[16] = {};
    if (PE_parse_boot_arg_str("mtkfe", feArg, sizeof(feArg))) feNativeWanted = !strcmp(feArg, "native");
    OSNumber *rate = OSDynamicCast(OSNumber, getProperty("MtkDataRate"));
    if (rate && rate->unsigned32BitValue() >= 18 && rate->unsigned32BitValue() <= 25) dataRate = (uint8_t)rate->unsigned8BitValue();
    OSBoolean *pwr = OSDynamicCast(OSBoolean, getProperty("MtkPower"));
    if (pwr) powerOn = pwr->isTrue();
    OSBoolean *rxba = OSDynamicCast(OSBoolean, getProperty("MtkRxBa"));
    if (rxba) rxBaOn = rxba->isTrue();
    OSBoolean *txba = OSDynamicCast(OSBoolean, getProperty("MtkTxBa"));
    if (txba) txBaOn = txba->isTrue();
    OSBoolean *ht = OSDynamicCast(OSBoolean, getProperty("MtkHt"));
    if (ht) htOn = ht->isTrue();
    OSNumber *mbw = OSDynamicCast(OSNumber, getProperty("MtkMaxBw"));
    if (mbw && mbw->unsigned32BitValue() <= 2) maxBw = (uint8_t)mbw->unsigned32BitValue();
    // sleep / wake (default on): MtkSleep false or boot-arg -mtknosleep go back to acknowledging sleep only
    OSBoolean *slp = OSDynamicCast(OSBoolean, getProperty("MtkSleep"));
    if (slp) sleepOn = slp->isTrue();
    uint32_t noSleep = 0;
    if (PE_parse_boot_argn("-mtknosleep", &noSleep, sizeof(noSleep))) sleepOn = false;
    setProperty("MtkSleep", sleepOn);
    setProperty("MtkMilestone", "W1");

    setNum("MtkPciCommandBefore", pci->configRead16(kIOPCIConfigCommand), 16);
    pci->setMemoryEnable(true);
    pci->setBusMasterEnable(true);
    setNum("MtkPciCommand", pci->configRead16(kIOPCIConfigCommand), 16);

    map = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!map) {
        step("W1 fail: BAR0 map");
        return true;
    }
    mmio = (volatile uint8_t *)map->getVirtualAddress();
    mmioLen = (uint32_t)map->getLength();
    setNum("MtkBar0Phys", map->getPhysicalAddress(), 64);
    setNum("MtkBar0Size", mmioLen);
    step("W1 BAR0 mapped");

    listInterrupts();

    setNum("MtkLpctlBefore", rr(MT_CONN_ON_LPCTL));
    int fw = fwOwn();
    setNum("MtkFwOwnTries", (uint32_t)fw);
    int drv = drvOwn();
    setNum("MtkDrvOwnTries", (uint32_t)drv);
    if (drv < 0 && pciFlr()) {                  // I-081: a chip left wedged by a warm restart
        fw = fwOwn();
        drv = drvOwn();
        setNum("MtkDrvOwnTriesAfterFlr", (uint32_t)drv);
    }
    setNum("MtkLpctl", rr(MT_CONN_ON_LPCTL));
    if (drv < 0) {
        setNum("MtkBadAccess", badAccess);
        step(fw < 0 ? "W1 fail: fw_own and drv_own" : "W1 fail: drv_own");
        return true;
    }

    uint32_t chipId = rr(MT_HW_CHIPID);
    uint32_t hwRev = rr(MT_HW_REV);
    setNum("MtkChipId", chipId);
    setNum("MtkHwRev", hwRev);
    setNum("MtkAsicRevision", (chipId << 16) | (hwRev & 0xff)); // mdev->rev in mt7925_pci_probe
    setNum("MtkConnOnMisc", rr(MT_CONN_ON_MISC));
    setNum("MtkWfsysSwRst", rr(MT_WFSYS_SW_RST_B));
    setNum("MtkBadAccess", badAccess);

    if ((chipId & 0xffff) != 0x7925) {
        step("W1 partial: drv_own ok, unexpected chip id");
        return true;
    }
    step("W1 pass: drv_own ok, chip 7925");

    mapper = IOMapper::copyMapperForDevice(pci);
    setProperty("MtkMapper", mapper ? "device mapper" : "none (physical addresses)");
    // join the power plane before bring-up, so restart and power-off reach systemWillShutdown even when the firmware
    // download failed (I-081)
    static IOPMPowerState powerStates[2] = {
        {kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {kIOPMPowerStateVersion1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
    };
    PMinit();
    provider->joinPMtree(this);
    pmJoined = registerPowerDriver(this, powerStates, 2) == kIOReturnSuccess;
    setProperty("MtkPmJoined", pmJoined);
    bringUp();
    setNum("MtkBadAccess", badAccess);
    registerService();
    return true;
}

// The power plane parent is the PCI device: state 0 comes before it drops to D3 (system sleep, hibernation), state 1
// after IOPCIFamily restored its config space on wake. Both run synchronously on the gate, so no tick, front-end call
// or user-client call touches the chip in between; the wake bring-up (firmware download) holds it for about a second.
// With MtkSleep off, or before the station ever came up, this only acknowledges, as up to 0.12.1.
IOReturn MtkWlan::setPowerState(unsigned long state, IOService *device) {
    if (!sleepOn || !workLoop || !pci || !mmio)
        return kIOPMAckImplied;
    workLoop->runAction([](OSObject *o, void *on, void *, void *, void *) -> IOReturn {
        MtkWlan *self = static_cast<MtkWlan *>(o);
        if (on) self->wakeUp();
        else self->sleepDown();
        return kIOReturnSuccess;
    }, this, (void *)(uintptr_t)(state != 0));
    return kIOPMAckImplied;
}

// On the gate, so no tick or front-end call touches the chip while it is being reset
void MtkWlan::systemWillShutdown(IOOptionBits specifier) {
    if (specifier == kIOMessageSystemWillPowerOff || specifier == kIOMessageSystemWillRestart) {
        const char *why = specifier == kIOMessageSystemWillRestart ? "restart" : "power off";
        if (workLoop)
            workLoop->runAction([](OSObject *o, void *w, void *, void *, void *) -> IOReturn {
                static_cast<MtkWlan *>(o)->quiesce((const char *)w);
                return kIOReturnSuccess;
            }, this, (void *)why);
        else
            quiesce(why);
    }
    IOService::systemWillShutdown(specifier);
}

void MtkWlan::stop(IOService *provider) {
    if (pmJoined) {
        PMstop();
        pmJoined = false;
    }
    if (timer) {
        timer->cancelTimeout();
        if (workLoop) workLoop->removeEventSource(timer);
        OSSafeReleaseNULL(timer);
    }
    secReset();
    mtk_zero(wantPmk, sizeof(wantPmk));
    mtk_zero(autoNets, sizeof(autoNets));
    // the front end (child) is stopped before its provider; its stop already removed the callbacks
    if (irqIndex >= 0) {
        pci->disableInterrupt(irqIndex);
        pci->unregisterInterrupt(irqIndex);
        irqIndex = -1;
    }
    if (mmio)
        rawWrite(0xd4208, rawRead(0xd4208) & ~0x5u); // WFDMA0 GLO_CFG: stop TX/RX DMA before freeing rings
    for (uint8_t t = 0; t < 8; t++) rxBaStop(t);
    ringFree(txData); ringFree(txMcu); ringFree(txFwdl); ringFree(rxMcu); ringFree(rxData);
    OSSafeReleaseNULL(mapper);
    OSSafeReleaseNULL(map);
    mmio = nullptr;
    OSSafeReleaseNULL(pci);
    IOService::stop(provider);
}

void MtkWlan::free() {
    if (feFallbackCall) {
        thread_call_cancel(feFallbackCall);
        thread_call_free(feFallbackCall);
        feFallbackCall = nullptr;
    }
    OSSafeReleaseNULL(frontEnd);
    OSSafeReleaseNULL(workLoop);
    IOService::free();
}

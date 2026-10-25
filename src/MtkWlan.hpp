// <wtl> project: mtkwlan | file: src/MtkWlan.hpp | author: watchthelight </wtl>
// MtkWlan: macOS driver for the MediaTek MT7925 Wi-Fi 7 PCIe card (14C3:7925) in the Legion 5 15IAX10.
// W1: PCI attach, BAR0 map, ownership handshake, chip ID. W2: WFDMA rings + MSI. W3: firmware download.
// W4: NIC capability (MAC), station interface, US channel domain, firmware scan.
// W5: open-system auth + association.
// W6: Ethernet front-end (en1): 802.11 data TX/RX, TX-free tokens, keepalive, rejoin (MtkNet.cpp).
// W8: HT/VHT association (WMM, HT/VHT caps, 20/40/80 MHz), firmware rate control.
// W9: block ack both ways: RX reorder (mt76 agg-rx.c port), TX ADDBA + firmware A-MPDU (MtkBa.cpp).
// 0.8.0 (W0): MtkWlan is now the core (IOService on the PCI device). The Ethernet front end is the child
// MtkWlanEth (MtkWlanEth.cpp), which reaches the core only through the C ops table in MtkCoreApi.h.
// 0.9.0 (W1/W2): scan and join are state machines driven from the tick, with a BSS cache (MtkScan.cpp,
// MtkJoin.cpp); a connection manager picks the network (the one asked for through the ops table v2 or the
// IOUserClient MtkWlanUserClient, else an allow-listed open one). WPA2-PSK (CCMP) runs in the core: MtkSupp.c (4-way
// and group handshake), MtkCrypto.c, and MtkSec.cpp (EAPOL in and out, key install from the tick, port control).
// Recovery keeps only 128 KB of kernel log, so every step publishes its result as a registry
// property (Mtk*), which recon.sh / wifi.sh capture with ioreg.

#ifndef MTKWLAN_HPP
#define MTKWLAN_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/thread_call.h>
#include <sys/kpi_mbuf.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSByteOrder.h>
#include "MtkDma.hpp"
#include "MtkCoreApi.h"
#include "MtkSupp.h"
#include "MtkSuper.h"

// One WFDMA ring (mt76_queue): descriptors plus one fixed buffer per slot
struct MtkRing {
    MtkDmaMem desc, bufs;
    uint32_t regs = 0;      // BAR offset of {desc_base, ring_size, cpu_idx, dma_idx}
    uint32_t bufSize = 0;
    uint16_t n = 0, head = 0, tail = 0;
    bool rx = false;
};

// One RX block-ack session (mt76_rx_tid): reorder buffer of Ethernet frames indexed by sequence number
constexpr uint16_t MTK_BA_WIN = 64;
struct MtkRxTid {
    bool active, started;
    uint16_t head, size, nframes;
    mbuf_t buf[MTK_BA_WIN];
    uint32_t at[MTK_BA_WIN];
    uint16_t seq[MTK_BA_WIN];
    uint64_t pn[MTK_BA_WIN];                   // CCMP PN of the buffered frame, checked when it leaves the buffer
    int8_t pnSlot[MTK_BA_WIN];                 // replay counter it belongs to, -1 = none
};

// One BSS from beacons / probe responses (W1 cache)
// Bigger than MTK_SCAN_MAX: one scan in a dense area hears more than 48 BSSs (thanhnndev/ga403wm-hackintosh 4257353)
#define MTK_BSS_CACHE 128

struct MtkBss {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t ssidLen, ch;
    int8_t rssi;                               // dBm from the P-RXV RCPI, -127 unknown
    uint8_t sec, akm, pairwise, group, flags;  // MtkUserApi.h MTK_SEC_* / MTK_AKM_* / MTK_CIPHER_* / MTK_BSS_*
    uint8_t probe;                             // the last frame heard was a probe response (else a beacon)
    uint16_t cap, bcnInt, rsnCaps;
    uint32_t lastSeen;                         // ticks
    uint32_t seenGen;                          // bssGen of the scan that last heard it
    uint16_t seen, ieLen;
    uint8_t ie[512];                           // information elements after the fixed fields (same cap as tgtIe)
};

class MtkWlan : public IOService {
    OSDeclareDefaultStructors(MtkWlan)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    // I-081: restart and power-off quiesce the chip so the next OS (or Recovery) finds it clean
    // stealth-a16: system sleep tears the station down, wake brings the chip up again (MtkNet.cpp sleepDown / wakeUp)
    IOReturn setPowerState(unsigned long state, IOService *device) override;
    void systemWillShutdown(IOOptionBits specifier) override;

    // The core's work loop: the tick, RX and every front-end callback run on its gate
    IOWorkLoop *getWorkLoop() const override;
    // MTK_CORE_OPS_V1 hands out the ops table (MtkCoreApi.h); everything else goes to IOService
    using IOService::callPlatformFunction;
    IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2,
                                  void *param3, void *param4) override;

    // MtkRegs.cpp: BAR0 access with the mt7925 bus address remapping (port of mt7925/pci.c)
    uint32_t rr(uint32_t reg);
    void wr(uint32_t reg, uint32_t val);
    uint32_t rmw(uint32_t reg, uint32_t mask, uint32_t val);
    bool pollMsec(uint32_t reg, uint32_t mask, uint32_t val, int timeoutMs, int tickMs);

private:
    uint32_t rawRead(uint32_t off);
    void rawWrite(uint32_t off, uint32_t val);
    uint32_t rawRmw(uint32_t off, uint32_t mask, uint32_t val);
    uint32_t regAddr(uint32_t addr);
    uint32_t mapL1(uint32_t addr);
    uint32_t mapL2(uint32_t addr);
    void remapRestore();

    int fwOwn();
    int drvOwn();

    // MtkFw.cpp: W2/W3
    int bringUp();
    int hwStart();
    bool pciFlr();
    void quiesce(const char *why);
    int reload(const char *why);
    // sleep / wake (MtkNet.cpp, hardware halves in MtkFw.cpp): teardown on sleep, full bring-up on wake
    void sleepDown();
    void wakeUp();
    void sleepHw();
    int wakeHw(bool *powerLost);
    int ringAlloc(MtkRing &r, bool rx, uint16_t idx, uint16_t n, uint32_t bufSize);
    void ringReset(MtkRing &r);
    void ringFree(MtkRing &r) { r.desc.free(); r.bufs.free(); }
    volatile uint32_t *desc(MtkRing &r, uint16_t i) { return (volatile uint32_t *)(r.desc.va + i * 16); }
    void rxArm(MtkRing &r);
    int txSend(MtkRing &r, const void *data, uint32_t len);
    const uint8_t *rxPoll(MtkRing &r, uint32_t *len);
    void rxRelease(MtkRing &r);
    void setupInterrupt();
    static void interruptHandler(OSObject *owner, void *ref, IOService *nub, int source);
    uint8_t nextSeq();
    int mcuCmd(uint8_t cid, const void *payload, uint32_t plen);
    int mcuWait(uint8_t cid, uint8_t seq);
    int sendScatter(const uint8_t *data, uint32_t len);
    int initDownload(uint32_t addr, uint32_t len, uint32_t mode);
    int patchSem(bool get);
    int loadPatch();
    int loadRam();

    // MtkScan.cpp: W4, and the W1 scan machine + BSS cache
    int uniCmd(uint8_t cid, const void *payload, uint32_t plen, bool query, uint8_t *ev, uint32_t evMax, uint32_t *evLen);
    void pollRx();
    void mtkParseFrame(const uint8_t *p, uint32_t len);
    int hwInit(bool keepMac = false);
    int devInfo(bool enable);
    int scanStart();
    void scanService();
    void bssAge();
    void bssNote(const uint8_t *frame, uint32_t len, const char *ssid, uint8_t ssidLen, uint8_t ch, int8_t rssi);
    void bssNoteFrame(const uint8_t *f, uint32_t flen, int8_t rssi);
    static void bssParseSec(MtkBss &b);
    static int8_t rxRssi(const uint8_t *p, uint32_t len, uint32_t rxd1);
    static uint32_t rxFrameOffset(uint32_t rxd1, uint32_t rxd2);
    int bssScore(const MtkBss &b) const;
    bool bssJoinable(const MtkBss &b, bool withKey) const;
    void publishScan();
    void noteEvent(const uint8_t *p, uint32_t len);
    void macInitBand(int band);
    void dispatchRx(const uint8_t *p, uint32_t len);

    // MtkJoin.cpp: W5, and the W1 join machine + connection manager
    void noteMgmt(const uint8_t *p, uint32_t len);
    bool txMgmt(const uint8_t *frame, uint32_t len, uint8_t subtype, uint8_t qIdx = 0x10, uint8_t tid = 0);
    void parseAp();
    int bssInfo(bool enable);
    int edca();
    int staRec(uint8_t state, bool newly, uint16_t aid);
    int staBa(uint8_t tid, uint16_t ssn, uint16_t win, bool tx, bool enable);
    int staKey(bool pairwise, uint8_t keyId, const uint8_t *key, uint8_t keyLen, bool add);
    int staRemove();
    void mgmtHeader(uint8_t *f, uint8_t fc0);
    void sendAuth();
    void sendAssoc();
    bool joinStart(int bssIdx);
    void joinService();
    void joinFail(uint32_t result);
    void joinUp();
    void joinTeardown(uint16_t deauthReason);
    void cmService();
    int cmPick();
    int cmPickOpen();
    int cmPickSaved(int *net);
    void cmResult(uint32_t result);
    int coreScan();
    int coreJoin(const mtk_join_params *p);
    int coreDisconnect();
    int coreSetMac(const uint8_t m[6]);
    void coreStatus(mtk_status *out);
    uint32_t coreResults(mtk_scan_result *out, uint32_t max);
    // MtkSec.cpp: W2 WPA2-PSK glue
    void secReset();
    void secStart();
    void secQueueEapol(const uint8_t *p, uint32_t len);
    void secService();
    bool txEapol(const uint8_t *frame, uint32_t len, bool protect);
    bool secInstallService();
    bool pnAccept(int slot, uint64_t pn);
    static int secSendEapol(void *ctx, const uint8_t *f, size_t n, int protect);
    static int secInstallPtk(void *ctx, const uint8_t tk[16]);
    static int secInstallGtk(void *ctx, int keyid, const uint8_t *gtk, size_t gtklen, const uint8_t rsc[8]);
    static void secRandom(void *ctx, uint8_t *buf, size_t n);

    // MtkPower.cpp: CLC + per-rate TX power limits
    int readEeprom(uint32_t offset, uint8_t *val);
    int clcStage();
    int ratePowerBand(bool is5g, int8_t halfDbm);
    int powerStage(bool afterDomain);
    int domainStage();
    uint32_t domainChannels(uint8_t *ch, uint32_t *flags, uint32_t *n2);
    char country[3] = "US", countryWanted[3] = "US";   // 0.12.1: set_country, applied by the tick when no join runs
    bool countryPending = false;
    void countryService();
    int roc(bool acquire);
    void hwPrep();
    uint8_t *assocIes(uint8_t *p, bool ht);

public:
    // the user client (MtkUserClient.cpp) runs these on the gate through runAction
    static IOReturn gatedUserCall(OSObject *owner, void *selector, void *in, void *out, void *outSize);
    bool userClientAllowed() const { return workLoop != nullptr && timer != nullptr; }
private:

    // MtkNet.cpp: W6 data path and the front-end side of the ops table
    static const mtk_core_ops kOps;
    static const mtk_core_ops_v2 kOpsV2;
    static const mtk_core_ops_v3 kOpsV3;
    static const mtk_core_ops_v4 kOpsV4;
    static const mtk_core_ops_v5 kOpsV5;
    static int opSetMac(void *core, const uint8_t mac[6]);
    static int opGetRateInfo(void *core, mtk_rate_info *out);
    static int opSetCountry(void *core, const char alpha2[2]);
    void rateInfo(mtk_rate_info *out) const;
    static int opSetMode(void *core, uint32_t mode, uint32_t flags);
    static int opGetBss(void *core, uint32_t index, mtk_bss_info *out);
    static int opGetChannels(void *core, uint8_t *list, uint32_t max, uint32_t *count);
    static int opTxBytes(void *core, const uint8_t *frame, uint32_t len);
    int coreGetBss(uint32_t index, mtk_bss_info *out);
    uint32_t coreChannels(uint8_t *list, uint32_t max);
    int txFrame(const uint8_t *eh, uint32_t len, mbuf_t m, const uint8_t *bytes);
    int txEthernetBytes(const uint8_t *frame, uint32_t len);
    void frontEndService();
    static void feFallbackThread(void *owner, void *);
    static int opScan(void *core);
    static int opResults(void *core, mtk_scan_result *out, uint32_t max, uint32_t *count);
    static int opJoin(void *core, const mtk_join_params *p);
    static int opDisconnect(void *core);
    static int opStatus(void *core, mtk_status *out);
    static int opSetCallbacks(void *core, const mtk_fe_callbacks *cb);
    static IOReturn gatedSetCallbacks(OSObject *owner, void *cb, void *, void *, void *);
    static int opTx(void *core, mbuf_t m);
    static void opSetEnabled(void *core, int enabled);
    static int opGetMac(void *core, uint8_t mac[6]);
    static int opGetLinkInfo(void *core, mtk_link_info *out);
    int txEthernet(mbuf_t m);
    int startFrontEnd();
    bool txSlotReady(bool force);
    void txRelease(uint16_t slot);
    void txSweep();
    void txQueue(uint32_t flen, uint8_t qIdx, uint8_t ftype, uint8_t stype, uint8_t rate, bool mcast, bool status,
                 uint32_t hdrLen = 24, uint8_t tid = 0, bool protect = false);
    void addbaDecline(const uint8_t *f, uint32_t flen);
    void txStatus(const uint8_t *p, uint32_t len);
    void txFree(const uint8_t *p, uint32_t len);
    void rxFrame(const uint8_t *p, uint32_t len);
    void deliver(const uint8_t *da, const uint8_t *sa, const uint8_t *typeAndPayload, uint32_t n, int tid = -1,
                 uint16_t seq = 0, int pnSlot = -1, uint64_t pn = 0);
    void inputQueue(mbuf_t m);
    void inputChecked(mbuf_t m, int pnSlot, uint64_t pn);
    // MtkBa.cpp: W9
    void baActionHeader(uint8_t *r, uint8_t action);
    void baAction(const uint8_t *f, uint32_t flen);
    void baQueue(const uint8_t *f, uint32_t flen);
    void baService();
    void txBaRequest();
    void rxBaStart(uint8_t tid, uint16_t ssn, uint16_t size);
    void rxBaStop(uint8_t tid);
    void rxBaReleaseSlot(MtkRxTid &t, int idx);
    void rxBaReleaseFrames(MtkRxTid &t, uint16_t head);
    void rxBaReleaseHead(MtkRxTid &t);
    void rxBaReorder(uint8_t tid, uint16_t seqno, mbuf_t m, int pnSlot, uint64_t pn);
    void rxBaTimeouts();
    void rxBar(const uint8_t *f, uint32_t flen);
    void linkLost();
    void reportLink();
    void keepalive(bool status = false);
    void stallService();
    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    void tick();
    void publishW6();
    int netStage();
    void listInterrupts();
    void step(const char *state);
    void setNum(const char *key, uint64_t val, unsigned bits = 32);

    IOPCIDevice *pci = nullptr;
    IOMemoryMap *map = nullptr;
    volatile uint8_t *mmio = nullptr;
    uint32_t mmioLen = 0;
    uint32_t backupL1 = 0, backupL2 = 0;
    uint32_t badAccess = 0;

    IOMapper *mapper = nullptr;
    MtkRing txData, txMcu, txFwdl, rxMcu, rxData;
    int irqIndex = -1;
    volatile uint32_t irqCount = 0, irqLastStatus = 0;
    uint32_t rxEvents = 0, rxUnmatched = 0, fwBytes = 0;
    uint8_t mcuSeq = 0;

    uint8_t mac[6] = {};
    uint32_t macChanges = 0;                   // station address changes applied through set_mac (MtkMacChanges)
    bool scanDone = false;
    uint32_t rxFrames = 0, rxBeacons = 0, rxOther = 0;

    // W1 scan machine and BSS cache
    MtkBss bss[MTK_BSS_CACHE];
    int nBss = 0;
    uint32_t bssGen = 0;                       // bumped per scan, see MtkBss::seenGen
    bool scanRunning = false, scanWanted = false, scanEverDone = false;
    uint32_t scanStartedAt = 0, scanDoneAt = 0, scanFinishedAt = 0, scans = 0, scanFails = 0;
    uint8_t scanSeq = 0;
    bool userScanPending = false;              // a userspace scan: the auto-open fallback waits for its outcome
    uint32_t userScanDoneAt = 0, firstScanDoneAt = 0;
    // firmware command health: uniCmd never runs inside the RX path, and a dead firmware stops the machines
    bool inDispatch = false, fwDead = false;
    uint32_t uniCmdInDispatch = 0, uniCmdTimeouts = 0, uniCmdFails = 0;
    volatile uint32_t irqStatusOr = 0;
    uint32_t evCount[256] = {};
    uint32_t evShort = 0, evDumps = 0;

    // W5 target BSS and join state (copied from the BSS cache by joinStart)
    char tgtSsid[33] = {};
    uint8_t tgtSsidLen = 0;
    bool tgtFound = false;
    uint8_t tgtBssid[6] = {}, tgtCh = 0;
    bool tgtRsn = false;                       // WPA2-PSK target (else open)
    uint8_t tgtGroup = 4;                      // its group cipher suite type
    uint8_t tgtRsnIe[258];                     // its RSN element (beacon), for the message 3 check
    uint16_t tgtRsnIeLen = 0;
    int8_t tgtRssi = -127;
    uint16_t tgtBcnInt = 0, tgtCap = 0;
    uint8_t tgtIe[512];
    uint32_t tgtIeLen = 0;
    uint8_t apSupp = 0, apBasic = 0;
    uint8_t wantFc = 0;
    uint8_t mgmt[512];
    uint32_t mgmtLen = 0;
    volatile bool rocGrant = false;
    uint8_t rocStatus = 0xff, rocTok = 0;
    bool linkUp = false;                       // associated (802.11)
    bool portOpen = false;                     // data allowed: open network, or WPA2 keys installed
    bool joinedOnce = false;
    uint16_t aid = 0;

    // W1 join machine: 0 idle, 1 ROC, 2 auth, 3 assoc, 4 4-way handshake, 5 up
    uint8_t joinState = 0;
    uint8_t joinTries = 0;
    bool joinHtPass = false, rocHeld = false;
    uint32_t joinDeadline = 0, joinPhaseEnd = 0;
    uint32_t joins = 0, joinFailures = 0, lastResult = MTK_JOIN_OK, authTx = 0, assocTx = 0;
    // connection manager: the wanted network (ops v2 / user client) or an allow-listed open one
    bool wantActive = false, wantHasPmk = false, autoOpen = false;
    uint8_t wantSsid[32] = {}, wantSsidLen = 0, wantBssid[6] = {}, wantPmk[32] = {};
    char autoOpenSsids[4][33] = {};            // MtkAutoOpenSsids: exact SSIDs the fallback may join
    uint8_t nAutoOpenSsids = 0;
    struct AutoNet { uint8_t ssid[32]; uint8_t ssidLen; uint8_t pmk[32]; };
    AutoNet autoNets[4] = {};                  // MtkAutoNetworks: WPA2 networks the fallback may join, PMK only
    uint8_t nAutoNets = 0;
    bool autoAllowed() const { return nAutoOpenSsids + nAutoNets > 0; }
    uint32_t cmNextAt = 0, cmFails = 0, cmLastScanAt = 0, wantMissingSince = 0, cmRuns = 0, cmLastPickAt = 0, linkUpAt = 0;
    uint8_t cmLastBssid[6] = {};
    bool cmKick = false, switchPending = false, disconnectPending = false, rejoinPending = false;
    bool onFallback = false;                   // the current join is the open fallback, not the wanted network
    // the RX path only flags a deauth / disassoc; the tick tears the link down
    bool deauthPending = false;
    uint16_t deauthPendingReason = 0;
    // v2 front-end callbacks (the v1 table lives in fe)
    void (*feScanDone)(void *ctx, int status, uint32_t count) = nullptr;
    void (*feJoinResult)(void *ctx, int result, const uint8_t bssid[6]) = nullptr;

    // W2 WPA2-PSK
    mtk_supp supp = {};
    bool suppActive = false, ptkInstalled = false;
    uint8_t secCipher = 0;                     // BSS_INFO_SEC cipher: 0 open, 4 CCMP
    uint8_t gtkId = 0;
    // keys the supplicant handed over; installed from the tick once the last EAPOL frame has left (secInstallService)
    bool pendingTkSet = false, pendingGtkSet = false;
    uint8_t pendingTk[16] = {}, pendingGtk[32] = {}, pendingGtkLen = 0, pendingGtkId = 0, pendingGtkRsc[8] = {};
    int32_t eapolSlot = -1;                    // TX slot of the last EAPOL frame we sent, -1 = freed
    uint32_t eapolSentAt = 0, secInstalls = 0, secInstallFails = 0;
    // CCMP replay counters (mt76_check_ccmp_pn): 0-7 pairwise by TID, 8-15 group by TID; reset on key install
    uint64_t rxPn[16] = {};
    uint32_t rxPnReplays = 0;
    uint32_t rxNoPn = 0, rxIcvErr = 0, rxCipherMismatch = 0, rxUnprotDrop = 0, rxTransDrop = 0, rxPortDrop = 0,
             rxEapol = 0, txEapolCount = 0, txPortDrop = 0, eapolQDropped = 0;
    struct EapolEvt { uint16_t len; uint8_t f[MTK_EAPOL_MAX]; };
    EapolEvt eapolQ[4];
    uint8_t eapolQHead = 0, eapolQTail = 0;

    // W6 data path; the network interface belongs to the front end (fe, set through the ops table)
    IOWorkLoop *workLoop = nullptr;
    IOTimerEventSource *timer = nullptr;
    bool pmJoined = false, quiesced = false;   // power plane membership (systemWillShutdown), quiesce done once
    // sleep / wake: MtkSleep (Info.plist) or boot-arg -mtknosleep turns it off (then sleep is only acknowledged, as before)
    bool sleepOn = true, asleep = false, wakeFailed = false;
    uint32_t sleeps = 0, wakes = 0, wakeFails = 0, wakePowerLost = 0, lastWakeMs = 0;
    // attach recovery and the link supervisor (MtkSuper.c)
    uint32_t attachRecover = 0, attachFlr = 0, flrUnsupported = 0;
    mtk_super super = {};
    uint32_t stallReloadFails = 0, stallLastReloadMs = 0;
    bool stallFreezeRx = false;                // debug (kMtkUcStallTest): ignore the Rx ring so the supervisor trips
    IOService *frontEnd = nullptr;             // MtkWlanEth child, created by startFrontEnd()
    mtk_fe_callbacks fe = {};                  // all null while no front end is registered
    // 0.10.0: front-end selection (MtkFrontEnd / boot-arg mtkfe). With "native" the core publishes itself and waits for
    // MtkWlanNative to register; if it declines or stays away, the Ethernet front end starts (frontEndService)
    bool feNativeWanted = false, feDeclined = false, feFallbackDone = false, feClaimed = false;   // feClaimed: MTK_CORE_FE_CLAIM seen
    uint32_t feWantedAt = 0, feMode = MTK_FE_ETHERNET, feModeFlags = 0;
    thread_call_t feFallbackCall = nullptr;
    bool ifEnabled = false, linkReported = false, rxQueued = false, txStalled = false;
    uint8_t dataRate = 0;                      // MtkDataRate: 0 = firmware rate control (HT/VHT), 18-25 = fixed legacy rate
    uint32_t rxCtrl = 0;                       // DMA ctrl word of the last rxPoll
    uint32_t ticks = 0, lastTxTick = 0, rejoinAt = 0;
    bool txBusy[1024] = {};                    // 0.11.2: Tx data ring 1024 (was 256)
    uint32_t txBusyAt[1024] = {};
    uint32_t txPackets = 0, txStall = 0, txDrop = 0, txRingFull = 0, txFreeEvents = 0, txFreed = 0,
             txFreeBadVer = 0, txLostFree = 0, txFailed = 0;
    uint32_t rxPackets = 0, rxBad = 0, rxPartial = 0, rxTrans = 0, rxNonSnap = 0, rxProtected = 0,
             rxOwn = 0, rxDropped = 0;
    uint32_t deauths = 0, lastDeauth = 0, rejoins = 0, keepalives = 0;
    uint32_t rxTypes[32] = {};
    uint32_t txInFlight = 0, txInFlightPeak = 0, txCapped = 0, txQueued = 0;
    bool w6Pass = false;

    // W8 HT/VHT: the AP's capabilities from its beacon, and what the association settled on
    bool htOn = true;                          // MtkHt in Info.plist
    uint8_t maxBw = 2;                         // MtkMaxBw: 0 = 20, 1 = 40, 2 = 80 MHz
    bool apHt = false, apVht = false, htActive = false, vhtActive = false;
    uint16_t apHtCap = 0;
    uint8_t apAmpdu = 0, apRxMcs[10] = {}, apHtOp = 0;
    uint32_t apVhtCap = 0;
    uint16_t apVhtRx = 0xffff, apVhtTx = 0xffff;
    uint8_t chBw = 0, chCenter = 0, apNss = 1;  // bw: 0 = 20, 1 = 40, 2 = 80 MHz (CMD_CBW_*)
    uint32_t addbaDeclined = 0, rxAmsdu = 0;

    // 0.6.1 TX diagnostics: sampled TX status (rate, power, tries) and TX-free attempt counts
    uint32_t txsRecords = 0, txsAckErr = 0, txsCountSum = 0, txsSampleSeq = 0;
    uint32_t txsMode[16] = {}, txsMcs[16] = {}, txsNss[4] = {}, txsBw[4] = {};
    int32_t txsPwrSum = 0;
    uint32_t txsLast[8] = {};
    uint32_t tfHeaders = 0, tfAttempts = 0, tfStatErr = 0;

    // W9 block ack
    bool rxBaOn = true, txBaOn = true;         // MtkRxBa / MtkTxBa in Info.plist
    bool powerOn = true;                       // MtkPower in Info.plist
    int clcResult = 0;
    MtkRxTid rxTid[8] = {};
    uint8_t txBaState = 0;                     // 0 none, 1 requested, 2 operational, 3 refused
    uint8_t txBaDialog = 0;
    uint16_t txBaSsn = 0, txBaWin = 0;
    uint32_t txBaNextTick = 0, txBaRequests = 0, baTxStatus = 0xffff, baLastRc = 0;
    uint32_t baRxSessions = 0, baTxSessions = 0, baDup = 0, baBuffered = 0, baTimeoutRelease = 0;
    uint32_t barRx = 0, delbaRx = 0;
    // block-ack action frames wait here for tick(): firmware commands never run inside the RX path
    struct BaEvt { uint8_t f[40]; uint32_t len; };
    BaEvt baQ[8];
    uint8_t baQHead = 0, baQTail = 0;
    uint32_t baQDropped = 0;
    uint32_t txInflightMax = 32;               // txCapNoBa, or txCapBa while a TX block-ack session is up
    uint32_t txCapBa = 128, txCapNoBa = 32;    // 0.11.3: settable live (kMtkUcTune); 0.11.2's 512 / 64 was slower
    uint32_t txsDelaySum = 0, txsLastSeq = 0, txsSeqSpan = 0;
};

#endif

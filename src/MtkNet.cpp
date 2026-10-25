// <wtl> project: mtkwlan | file: src/MtkNet.cpp | author: watchthelight </wtl>
// SPDX-License-Identifier: ISC
// W6: data path over the BSS joined in W5 (the Ethernet front end itself is MtkWlanEth.cpp from 0.8.0).
// TX: Ethernet -> 802.11 data (ToDS, non-QoS, LLC/SNAP) on TX ring 0 in CT mode; TXD per
//   mt7925_mac_write_txwi / _80211 and hw txp per mt76_connac_write_hw_txp. The token is the slot
//   index, released by the firmware's TXRX_NOTIFY (mt7925_mac_tx_free).
// RX: RXD groups and header pad per mt7925_mac_fill_rx, then 802.11 (FromDS) -> Ethernet. RX header
//   translation stays off (STA_REC_HDR_TRANS dis_rx_hdr_tran), but a translated frame is accepted too.
// All of the above from Linux mt76 v6.18 (Copyright (C) 2023 MediaTek Inc., ISC).
// A 1 ms work-loop timer polls both RX rings, runs the scan / join / security machines, sends a null-data
// keepalive when idle. Counters are published as MtkW6* properties once a second.
// 0.5.1: at most TX_INFLIGHT_MAX data frames wait in the firmware and the output queue holds 64, so
// a full upload no longer builds a second of queue (measured: 1215 ms loaded latency at 9.6 Mb/s).
// 0.6.0 (W8): QoS data and firmware rate control after an HT/VHT association; ADDBA requests are declined
// (status 37), so there is no aggregation yet and no RX reordering is needed.
// 0.6.1: every 64th unicast data frame asks for a TX status record (rate, power, tries, ACK errors);
// TX-free headers add attempt counts. Published as MtkW9* to find why upload stays near 9 Mb/s.
// 0.8.0 (W0): the IONetworkController half moved to MtkWlanEth. This file keeps the 802.11 side and serves
// the ops table (MtkCoreApi.h): txEthernet is the old outputPacket body, received frames go to the
// front end's rx / rx_flush, link changes to its link callback, a cleared TX stall to tx_space. All of
// them run on the core work loop gate (tick, or the front end's gated output queue / command gate).
// 0.9.0 (W1/W2): the RX path only classifies, queues and delivers (a deauth sets deauthPending, EAPOL goes to
// the supplicant queue, block-ack actions to baQueue); it never issues a firmware command. Data is gated by the
// port: nothing but EAPOL passes before the keys are installed on a WPA2 network. Protected frames must have been
// decrypted by the hardware with the expected cipher (RXD2 SEC_MODE), plaintext data on a WPA2 association is
// dropped. TX sets FC Protected and TXD3 PROTECT_FRAME once the PTK is installed; the firmware adds the CCMP
// header and PN. The ops table v2 adds scan / results / join / disconnect / status for a native front end.

#include "MtkWlan.hpp"
#include "MtkCrypto.h"
#include <kern/clock.h>

namespace {
constexpr uint16_t AP_WCID = 1;                  // same as MtkJoin.cpp
constexpr uint8_t LMAC_AC_BE = 1;
constexpr uint8_t RATE_IDX_OFDM6 = 18, RATE_IDX_OFDM12 = 20, RATE_IDX_OFDM24 = 22;
constexpr uint32_t DMA_CTL_LAST_SEC0 = 1u << 30;
constexpr uint32_t TICK_US = 1000;
constexpr uint32_t TX_LOST_TICKS = 500;
constexpr uint64_t LINK_SPEED = 54000000ULL;    // what en1 reports while associated (0.5.0 on)
constexpr uint32_t REJOIN_DELAY_TICKS = 500;

inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[1] << 8 | p[0]); }
inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

const uint8_t kSnap[6] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};
const uint8_t kBridgeTunnel[6] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0xf8};
} // namespace

// ---------------------------------------------------------------- TX ring 0

// The head slot can be filled when the device has fetched every descriptor but one, fewer than
// TX_INFLIGHT_MAX frames wait for a TX-free, and the firmware has released the slot's token
// (or TX_LOST_TICKS passed without a release). force (management frames) skips the in-flight cap.
bool MtkWlan::txSlotReady(bool force) {
    uint16_t next = (uint16_t)((txData.head + 1) % txData.n);
    if (next == (rawRead(txData.regs + 12) & 0xffff)) { txRingFull++; return false; }
    if (!force && txInFlight >= txInflightMax) { txCapped++; return false; }
    uint16_t s = txData.head;
    if (txBusy[s]) {
        if (!force && ticks - txBusyAt[s] < TX_LOST_TICKS) return false;
        txRelease(s);
        txLostFree++;
    }
    return true;
}

void MtkWlan::txRelease(uint16_t slot) {
    if (!txBusy[slot]) return;
    txBusy[slot] = false;
    if (txInFlight) txInFlight--;
}

// Tokens the firmware never released (lost TX-free) stop counting as in flight
void MtkWlan::txSweep() {
    for (uint16_t i = 0; i < txData.n; i++)
        if (txBusy[i] && ticks - txBusyAt[i] >= TX_LOST_TICKS) {
            txRelease(i);
            txLostFree++;
        }
}

// TXD + hw txp for the frame already at slot + 128, then kick ring 0
// rate 0 = no FIXED_RATE (firmware rate control from STA_REC_RA); protect = TXD3 PROTECT_FRAME (hardware CCMP
// with the installed key, the frame carries FC Protected)
void MtkWlan::txQueue(uint32_t flen, uint8_t qIdx, uint8_t ftype, uint8_t stype, uint8_t rate, bool mcast, bool status,
                      uint32_t hdrLen, uint8_t tid, bool protect) {
    uint16_t slot = txData.head;
    uint8_t *w = txData.bufs.va + slot * txData.bufSize;
    uint32_t pa = txData.bufs.iova + slot * txData.bufSize;
    bzero(w, 128);
    put32(w + 0, (flen + 32) | (0u << 23) | ((uint32_t)qIdx << 25));            // TX_BYTES, PKT_FMT CT, Q_IDX
    put32(w + 4, AP_WCID | (2u << 14) | ((hdrLen / 2) << 16) | ((uint32_t)(tid & 0xf) << 21) |
                 (rate ? 1u << 31 : 0));                                       // WLAN_IDX, HDR 802.11, HDR_INFO, TID, FIXED_RATE
    put32(w + 8, ((uint32_t)ftype << 4) | stype);                                 // FRAME_TYPE, SUB_TYPE
    // BA_DISABLE with TX status or a fixed rate, as at the end of mt7925_mac_write_txwi; PROTECT_FRAME with a key
    put32(w + 12, (15u << 11) | (mcast ? 1u << 4 : 0) | ((status || rate) ? 1u << 28 : 0) | (protect ? 1u << 1 : 0));
    put32(w + 20, status ? (3 | (1u << 10)) : 0);                                 // PID, TX_STATUS_HOST
    put32(w + 24, (1u << 2) | (1u << 4) | (1u << 3) | ((uint32_t)rate << 16));    // DAS, MSDU_CNT 1, DIS_MAT, TX_RATE
    put16(w + 32, (uint16_t)(slot | 0x8000));                                     // txp.msdu_id[0] = token | VALID
    put32(w + 40, pa + 128);                                                      // txp.ptr[0].buf0
    put16(w + 44, (uint16_t)(flen | 0x8000));                                     // len0 | LAST
    txData.bufs.toDevice();
    volatile uint32_t *d = desc(txData, slot);
    d[0] = pa;
    d[2] = 0;
    d[3] = 0;
    d[1] = (64u << 16) | DMA_CTL_LAST_SEC0;                                       // SD_LEN0 = TXD + txp
    txBusy[slot] = true;
    txBusyAt[slot] = ticks;
    txInFlight++;
    txQueued++;
    if (txInFlight > txInFlightPeak) txInFlightPeak = txInFlight;
    txData.head = (uint16_t)((slot + 1) % txData.n);
    rawWrite(txData.regs + 8, txData.head);
    lastTxTick = ticks;
}

// Management frame (auth / assoc / deauth / block ack) at 6 Mb/s with TX status, waits until the device fetched it
bool MtkWlan::txMgmt(const uint8_t *frame, uint32_t len, uint8_t subtype, uint8_t qIdx, uint8_t tid) {
    if (len > 1900 || len < 24) return false;
    if (!txSlotReady(true)) return false;                              // ring full: do not overwrite the head slot
    memcpy(txData.bufs.va + txData.head * txData.bufSize + 128, frame, len);
    txQueue(len, qIdx, 0, subtype, RATE_IDX_OFDM6, false, true, 24, tid);
    for (int t = 0; t < 2000; t++) {
        if ((rawRead(txData.regs + 12) & 0xffff) == txData.head) return true;
        IODelay(100);
    }
    return false;
}

// mt7925_mac_tx_free: release the tokens (slot indexes) listed in a TXRX_NOTIFY packet
void MtkWlan::txFree(const uint8_t *p, uint32_t len) {
    txFreeEvents++;
    if (len < 8 || ((le32(p + 4) >> 16) & 0xf) < 4) { txFreeBadVer++; return; }
    uint32_t total = (le32(p) >> 16) & 0x3ff, count = 0;
    for (uint32_t o = 8; o + 4 <= len && count < total; o += 4) {
        uint32_t info = le32(p + o);
        if (info & (1u << 31)) continue;                          // new WLAN_ID pair
        if (info & (1u << 30)) {                                  // header: tx count / status
            if ((info >> 28) & 3) { txFailed++; tfStatErr++; }
            tfHeaders++;
            tfAttempts += (info >> 24) & 0xf;
            continue;
        }
        for (int i = 0; i < 2; i++) {
            uint32_t id = (info >> (15 * i)) & 0x7fff;
            if (id == 0x7fff) continue;
            count++;
            if (id < txData.n && txBusy[id]) {
                txRelease((uint16_t)id);
                txFreed++;
            }
        }
    }
}

// ---------------------------------------------------------------- RX

// A received frame from the association on: data to the stack once the port is open, EAPOL to the supplicant
// queue, beacons to the BSS cache, block-ack actions to baQueue, a deauth / disassoc from the AP flags the link
// lost. Nothing in here issues a firmware command or changes join / security state.
void MtkWlan::rxFrame(const uint8_t *p, uint32_t len) {
    if (!(rxCtrl & DMA_CTL_LAST_SEC0)) { rxPartial++; return; }
    if (len < 32) { rxBad++; return; }
    uint32_t rxd0 = le32(p), rxd1 = le32(p + 4), rxd2 = le32(p + 8), rxd3 = le32(p + 12);
    uint32_t total = rxd0 & 0xffff;
    if (total && total < len) len = total;
    if (rxd2 & ((1u << 23) | (1u << 24))) { rxBad++; return; }        // AMSDU_ERR, MAX_LEN_ERROR
    if (rxd3 & (1u << 24)) { rxBad++; return; }                       // FCS_ERR
    if (rxd1 & ((1u << 25) | (1u << 26))) { rxIcvErr++; return; }     // ICV / CCMP MIC / TKIP MIC error
    uint32_t secMode = (rxd2 >> 16) & 0x1f;                           // RXD2 SEC_MODE: 0 none, 2 TKIP, 4 CCMP
    bool decrypted = secMode != 0 && !(rxd1 & ((1u << 23) | (1u << 24)));   // and no CM / CLM error
    bool hasPn = false;
    uint64_t pn = 0;
    if (rxd1 & (1u << 16)) {                                          // group 1: the PN, little-endian in bytes 0..5
        uint32_t g1 = 32 + ((rxd1 & (1u << 19)) ? 16 : 0);            // after group 4 when present
        if (g1 + 16 <= len) {
            for (int i = 5; i >= 0; i--) pn = pn << 8 | p[g1 + i];
            hasPn = true;
        }
    }
    uint32_t off = rxFrameOffset(rxd1, rxd2);
    if (off + 14 > len) { rxBad++; return; }
    const uint8_t *f = p + off;
    uint32_t flen = len - off;

    if (rxd2 & (1u << 7)) {                                           // HDR_TRANS: already 802.3 (translation is off)
        if (rxd2 & (1u << 25)) { rxBad++; return; }
        rxTrans++;
        if (tgtRsn || !portOpen) { rxTransDrop++; return; }
        if (!memcmp(f + 6, mac, 6)) return;
        deliver(f, f + 6, f + 12, flen - 12);
        return;
    }

    uint16_t fc = le16(f);
    uint8_t type = (fc >> 2) & 3, sub = (fc >> 4) & 0xf;
    if (flen < 24) { rxBad++; return; }
    if (type == 1) {                                                  // control: BlockAckReq from the AP
        if (sub == 8 && !memcmp(f + 4, mac, 6) && !memcmp(f + 10, tgtBssid, 6)) rxBar(f, flen);
        return;
    }
    if (type == 0) {
        if ((sub == 8 || sub == 5) && flen >= 36 && !(f[10] & 1) && !memcmp(f + 10, f + 16, 6)) {
            bssNoteFrame(f, flen, rxRssi(p, len, rxd1));              // beacons keep the cache fresh while associated
            return;
        }
        if (sub == 13 && flen >= 28 && f[24] == 3 && !memcmp(f + 4, mac, 6) && !memcmp(f + 16, tgtBssid, 6))
            baQueue(f, flen);                                         // action: block ack (ADDBA / DELBA), handled in tick
        if ((sub == 12 || sub == 10) && !memcmp(f + 4, mac, 6) && !memcmp(f + 16, tgtBssid, 6) && flen >= 26) {
            lastDeauth = (uint32_t)sub << 16 | le16(f + 24);         // subtype << 16 | reason
            deauths++;
            deauthPendingReason = le16(f + 24);
            linkLost();                                               // flags it; the tick tears the link down
        }
        return;
    }
    if (type != 2 || (f[1] & 3) != 2 || memcmp(f + 10, tgtBssid, 6) != 0) return;   // data, FromDS, from our AP
    if (sub & 4) return;                                              // null / no payload
    if (!(f[4] & 1) && memcmp(f + 4, mac, 6)) return;                 // unicast to another station
    bool protFrame = (f[1] & 0x40) != 0;
    uint32_t h = 24;
    if (sub & 8) {
        h += 2;                                                       // QoS control
        if (f[24] & 0x80) { rxAmsdu++; return; }                      // A-MSDU (not advertised beyond 3839, dropped)
        if (f[1] & 0x80) h += 4;                                      // HT control (Order)
    }
    if (flen < h + 8) { rxBad++; return; }
    const uint8_t *l = f + h;
    if (memcmp(l, kSnap, 6) && memcmp(l, kBridgeTunnel, 6)) { rxNonSnap++; return; }
    bool eapol = l[6] == 0x88 && l[7] == 0x8e;
    bool group = (f[4] & 1) != 0;
    int pnSlot = -1;
    if (protFrame) {
        // decrypted by the hardware (CCMP header and MIC already stripped); the cipher must be the expected one
        uint32_t want = (group && tgtGroup == 2) ? 2u : 4u;
        if (!decrypted || secMode != want) { rxCipherMismatch++; return; }
        if (secMode == 4) {
            if (hasPn) pnSlot = (group ? 8 : 0) + ((sub & 8) ? (f[24] & 0x7) : 0);   // replay counter: key, TID
            else rxNoPn++;                                            // no group 1: the PN is not reported
        }
    } else if (tgtRsn && !eapol) {
        rxUnprotDrop++;                                               // plaintext data on a protected association
        return;
    }
    if (eapol) {
        if (!group && !memcmp(f + 10, tgtBssid, 6)) {
            rxEapol++;
            secQueueEapol(l + 8, flen - h - 8);
        }
        return;                                                       // never to the front end
    }
    if (!portOpen) { rxPortDrop++; return; }
    if (!memcmp(f + 16, mac, 6)) { rxOwn++; return; }                 // our broadcast, relayed back by the AP
    int tid = -1;
    uint16_t seq = 0;
    if ((sub & 8) && !group && ((f[24] >> 5) & 3) != 1) {             // unicast QoS data, not no-ack
        tid = f[24] & 0xf;
        seq = le16(f + 22) >> 4;
        if (tid >= 8 || !rxTid[tid].active) tid = -1;
    }
    deliver(f + 4, f + 16, l + 6, flen - h - 6, tid, seq, pnSlot, pn);
}

// Refuse a block-ack agreement (ADDBA response, status 37 = request declined): no A-MPDU until the
// driver does RX reordering
void MtkWlan::addbaDecline(const uint8_t *f, uint32_t flen) {
    uint8_t r[24 + 9] = {};
    r[0] = 0xd0;                                // action
    memcpy(r + 4, tgtBssid, 6);
    memcpy(r + 10, mac, 6);
    memcpy(r + 16, tgtBssid, 6);
    r[24] = 3;                                  // category block ack
    r[25] = 1;                                  // ADDBA response
    r[26] = f[26];                              // dialog token
    put16(r + 27, 37);                          // status: request declined
    r[29] = f[27];                              // BA parameter set as requested
    r[30] = f[28];
    put16(r + 31, 0);                           // timeout
    txMgmt(r, sizeof(r), 13);
    addbaDeclined++;
}

// PKT_TYPE_TXS (mt7925_queue_rx_skb, mt7925_mac_add_txs_skb): records of 8 dwords from dword 2.
// TXS0 rate (mode 9:6, nss 13:10, mcs 5:0), BW 31:29, ACK errors 18:16; TXS1 power 7:0; TXS2 WCID 27:16;
// TXS5 (format 0) tx count 29:25
void MtkWlan::txStatus(const uint8_t *p, uint32_t len) {
    for (uint32_t o = 8; o + 32 <= len; o += 32) {
        uint32_t w[8];
        for (int i = 0; i < 8; i++) w[i] = le32(p + o + 4 * i);
        if (((w[0] >> 23) & 3) > 1) continue;                     // TXS_FORMAT
        if (((w[2] >> 16) & 0xfff) != AP_WCID) continue;
        txsRecords++;
        memcpy(txsLast, w, sizeof(txsLast));
        uint32_t rate = w[0] & 0x3fff;
        txsMode[(rate >> 6) & 0xf]++;
        txsMcs[rate & 0xf]++;
        txsNss[((rate >> 10) & 0xf) & 3]++;
        txsBw[((w[0] >> 29) & 7) & 3]++;
        if (w[0] & (7u << 16)) txsAckErr++;
        txsPwrSum += (int8_t)(w[1] & 0xff);
        txsCountSum += (w[5] >> 25) & 0x1f;
        txsDelaySum += w[2] & 0xffff;                              // TXS2 TX_DELAY
        uint32_t seq = w[1] >> 20;                                 // TXS1 SEQNO: does the hardware number frames?
        if (txsRecords > 1) txsSeqSpan = (seq - txsLastSeq) & 0xfff;
        txsLastSeq = seq;
    }
}

// Hand one Ethernet frame (DA, SA, ethertype + payload) to the network stack; the PN and its replay-counter
// slot travel with the frame through the block-ack reorder buffer and are checked when it is delivered in order
void MtkWlan::deliver(const uint8_t *da, const uint8_t *sa, const uint8_t *typeAndPayload, uint32_t n, int tid,
                      uint16_t seq, int pnSlot, uint64_t pn) {
    if (!fe.rx || !ifEnabled || n < 2 || n > 1502) { rxDropped++; return; }
    mbuf_t m = fe.rx_alloc(fe.ctx, 12 + n);
    if (!m) { rxDropped++; return; }
    uint8_t eh[12];
    memcpy(eh, da, 6);
    memcpy(eh + 6, sa, 6);
    if (mbuf_copyback(m, 0, 12, eh, MBUF_DONTWAIT) || mbuf_copyback(m, 12, n, typeAndPayload, MBUF_DONTWAIT)) {
        mbuf_freem(m);
        rxDropped++;
        return;
    }
    if (tid >= 0) rxBaReorder((uint8_t)tid, seq, m, pnSlot, pn);
    else inputChecked(m, pnSlot, pn);
}

void MtkWlan::inputChecked(mbuf_t m, int pnSlot, uint64_t pn) {
    if (!pnAccept(pnSlot, pn)) { mbuf_freem(m); return; }
    inputQueue(m);
}

void MtkWlan::inputQueue(mbuf_t m) {
    if (!fe.rx) { mbuf_freem(m); return; }
    fe.rx(fe.ctx, m);
    rxQueued = true;
    rxPackets++;
}

// ---------------------------------------------------------------- link

// From the RX path: the AP deauthenticated or disassociated us. Only a flag; the tick does the teardown and the
// manager rejoins (a firmware command from here would re-dispatch the ring slot being read).
void MtkWlan::linkLost() {
    if (!linkUp && joinState < 3) return;
    deauthPending = true;
}

void MtkWlan::reportLink() {
    if (!fe.link) return;
    bool up = linkUp && portOpen && ifEnabled;
    if (up == linkReported) return;
    linkReported = up;
    fe.link(fe.ctx, up, up ? LINK_SPEED : 0);
}

void MtkWlan::keepalive(bool status) {
    if (!txSlotReady(false)) return;
    uint8_t *f = txData.bufs.va + txData.head * txData.bufSize + 128;
    bzero(f, 24);
    f[0] = 0x48;                                // null data
    f[1] = 0x01;                                // ToDS
    memcpy(f + 4, tgtBssid, 6);
    memcpy(f + 10, mac, 6);
    memcpy(f + 16, tgtBssid, 6);
    txQueue(24, LMAC_AC_BE, 2, 4, RATE_IDX_OFDM12, false, status);
    keepalives++;
}

// Link supervisor (I-096), once a second: MtkSuper.c decides from the counters, this does what it says
void MtkWlan::stallService() {
    if (!super.stall_ms || ticks % 1000) return;
    mtk_super_in in = {};
    in.now_ms = ticks;
    in.link = linkUp && portOpen;
    in.fw_dead = fwDead;
    in.rx_frames = rxFrames;
    in.tx_queued = txQueued;
    in.tx_freed = txFreed;
    in.txs_records = txsRecords;
    in.txs_ack_err = txsAckErr;
    switch (mtk_super_step(&super, &in)) {
    case MTK_SUPER_PROBE:
        keepalive(true);
        break;
    case MTK_SUPER_REJOIN:
        // the AP stopped acknowledging us without a deauth: the same path as a deauth, the manager rejoins
        step("stall: the AP no longer acknowledges frames, rejoining");
        deauthPendingReason = 0;
        deauthPending = true;
        break;
    case MTK_SUPER_RELOAD:
        reload(fwDead ? "firmware unresponsive" : stallFreezeRx ? "test (frozen rx)" : "tx tokens or tx status stopped");
        break;
    default:
        break;
    }
    if (super.probes || super.reloads || super.capped) {
        char s[160];
        snprintf(s, sizeof(s), "probes %u idle-ok %u rejoins %u reloads %u failed %u capped %u last-reload-ms %u", super.probes,
                 super.idle_ok, super.rejoins, super.reloads, stallReloadFails, super.capped, stallLastReloadMs);
        setProperty("MtkStall", s);
    }
}

// Chip reset, firmware reload and rejoin without detaching anything: the front end and its interface stay, the link
// goes down and the connection manager rejoins the wanted network (native: macOS sees a link loss and its join result).
// Runs on the gate from the tick; blocks it for the firmware download (about a second).
int MtkWlan::reload(const char *why) {
    uint64_t t0, t1;
    clock_get_uptime(&t0);
    char pm[200];
    snprintf(pm, sizeof(pm), "%s | tx cpu %u dma %u | rx cpu %u dma %u | mcu rx cpu %u dma %u | seq %u | pci cmd %04x | irq %08x",
             why, rawRead(txData.regs + 8), rawRead(txData.regs + 12), rawRead(rxData.regs + 8), rawRead(rxData.regs + 12),
             rawRead(rxMcu.regs + 8), rawRead(rxMcu.regs + 12), mcuSeq & 0xf, pci->configRead16(kIOPCIConfigCommand),
             irqStatusOr);
    setProperty("MtkStallPostMortem", pm);
    IOLog("MtkWlan: reload: %s\n", pm);
    fwDead = true;                              // the teardown below must not wait 3 s per firmware command
    bool scanWasRunning = scanRunning;
    joinTeardown(0);
    scanRunning = false;
    if (scanWasRunning && feScanDone && fe.ctx) feScanDone(fe.ctx, -1, 0);
    for (uint16_t i = 0; i < txData.n; i++) txBusy[i] = false;
    txInFlight = 0;
    stallFreezeRx = false;
    fwOwn();
    int rc = drvOwn() > 0 ? hwStart() : -4;
    if (rc && rc != -3 && pciFlr()) {
        fwOwn();
        if (drvOwn() > 0) rc = hwStart();
    }
    if (rc) {
        stallReloadFails++;
        step("stall: reload failed, the firmware stays stopped");
        return rc;
    }
    fwDead = false;
    uniCmdTimeouts = 0;
    if (hwInit(true)) {
        stallReloadFails++;
        return -8;
    }
    scanWanted = true;
    cmKick = true;
    rejoinPending = true;
    lastResult = MTK_JOIN_AP_LEFT;
    cmFails = 0;
    rejoinAt = ticks + REJOIN_DELAY_TICKS;
    clock_get_uptime(&t1);
    uint64_t ns;
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    stallLastReloadMs = (uint32_t)(ns / 1000000);
    step("stall: chip reset and firmware reloaded, rejoining");
    return 0;
}

// System sleep (setPowerState 0, gate held). What mac80211 does for a driver without WoWLAN, then mt7925_pci_suspend:
// leave the AP while the firmware still answers (deauthentication, STA_REC and BSS_INFO removed), so the front end sees
// the link go down now and not as a stale association after the wake; a join or scan in flight ends with a result; the
// tick stops; then DMA, interrupts and ownership (sleepHw). The wanted network, the keys macOS gave (PMK), the station
// address, the country and the BSS cache stay, so the manager can rejoin by itself after the wake.
void MtkWlan::sleepDown() {
    if (asleep || quiesced || !timer) return;   // no station yet (attach failed): nothing runs that sleep could break
    asleep = true;
    sleeps++;
    uint8_t prev = joinState;
    bool scanWasRunning = scanRunning;
    if (joinState) joinTeardown(linkUp ? 3 : 0);   // reason 3, leaving; reportLink tells the front end
    if (prev && prev < 5) {                     // a join in flight: its caller hears it ended (no backoff, see wakeUp)
        lastResult = MTK_JOIN_AP_LEFT;
        if (feJoinResult && fe.ctx) feJoinResult(fe.ctx, (int)MTK_JOIN_AP_LEFT, tgtBssid);
    }
    scanRunning = false;
    if (userScanPending) { userScanPending = false; userScanDoneAt = ticks; }
    if (scanWasRunning && feScanDone && fe.ctx) feScanDone(fe.ctx, -1, 0);
    fwDead = true;                              // from here until the wake every firmware command fails at once
    timer->cancelTimeout();
    for (uint16_t i = 0; i < txData.n; i++) txBusy[i] = false;
    txInFlight = 0;
    stallFreezeRx = false;
    sleepHw();
    setNum("MtkSleeps", sleeps);
    step(prev == 5 ? "sleep: left the network, chip handed to the firmware" : "sleep: chip handed to the firmware");
}

// System wake (setPowerState 1, gate held): the attach bring-up again (wakeHw: ownership, WFSYS reset, rings, firmware),
// the station (hwInit keeps the address macOS set, the country stays), then the tick, and the manager rejoins the wanted
// network at once. A native front end hears the link come back as after a reload; macOS's own auto-join may get there
// first, which the core answers as usual. If the chip does not come back, the firmware stays marked dead, the tick
// stays stopped (nothing touches the chip), the interface stays down, and the next sleep / wake cycle tries again.
void MtkWlan::wakeUp() {
    if (!asleep) return;                        // the initial power state, or a sleep that was never taken down
    asleep = false;
    wakes++;
    uint64_t t0, t1, ns;
    clock_get_uptime(&t0);
    bool lost = false;
    int rc = wakeHw(&lost);
    if (lost) wakePowerLost++;
    setProperty("MtkWakePower", rc == -9 ? "card not on the bus" : lost ? "lost (registers reset)" : "kept");
    if (!rc) {
        fwDead = false;
        uniCmdTimeouts = 0;
        if (hwInit(true)) rc = -8;
    }
    clock_get_uptime(&t1);
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    lastWakeMs = (uint32_t)(ns / 1000000);
    setNum("MtkWakes", wakes);
    setNum("MtkWakeMs", lastWakeMs);
    setNum("MtkWakePowerLost", wakePowerLost);
    if (rc) {
        wakeFails++;
        wakeFailed = true;
        fwDead = true;
        setNum("MtkWakeFails", wakeFails);
        setNum("MtkWakeLastResult", (uint32_t)rc);
        char s[96];
        snprintf(s, sizeof(s), "wake fail: bring-up returned %d, Wi-Fi stays down until the next wake or a reboot", rc);
        step(s);
        return;
    }
    wakeFailed = false;
    scanWanted = true;
    cmKick = true;
    rejoinPending = wantActive || autoOpen;
    if (wantActive) lastResult = MTK_JOIN_AP_LEFT;
    cmFails = 0;
    rejoinAt = ticks;                           // no backoff: the link went down on purpose
    timer->setTimeoutUS(TICK_US);
    char s[96];
    snprintf(s, sizeof(s), "wake: chip up again in %u ms (power %s), rejoining", lastWakeMs, lost ? "lost" : "kept");
    step(s);
}

void MtkWlan::timerFired(OSObject *owner, IOTimerEventSource *) {
    MtkWlan *self = OSDynamicCast(MtkWlan, owner);
    if (self) self->tick();
}

void MtkWlan::tick() {
    timer->setTimeoutUS(TICK_US);               // first, so nothing below can stop the clock
    ticks++;
    pollRx();
    uint32_t len = 0;
    const uint8_t *p;
    for (int guard = 0; guard < 64 && (p = rxPoll(rxMcu, &len)); guard++) {
        dispatchRx(p, len);
        rxRelease(rxMcu);
    }
    if (rxQueued && fe.rx_flush) {
        fe.rx_flush(fe.ctx);
        rxQueued = false;
    }
    if (txStalled && txSlotReady(false)) {
        txStalled = false;
        if (fe.tx_space) fe.tx_space(fe.ctx);
    }
    if (deauthPending) {
        deauthPending = false;
        uint8_t prev = joinState;
        uint32_t m2 = supp.m2;                  // before the teardown clears the supplicant
        bool wasUp = prev == 5;
        bool shortLink = wasUp && ticks - linkUpAt < 10000;
        joinTeardown(0);
        if (!wasUp) {
            // thrown out during the join: a deauth after our message 2 is the wrong-passphrase signature
            joinFailures++;
            cmResult(prev == 4 && m2 ? MTK_JOIN_WRONG_KEY : MTK_JOIN_AP_LEFT);
        } else if (shortLink) {
            joinFailures++;                     // connected, then thrown out within 10 s: back off like a failure
            cmResult(MTK_JOIN_AP_LEFT);
        } else {
            lastResult = MTK_JOIN_AP_LEFT;      // a long link ended: rejoin soon, no backoff
            cmFails = 0;
            rejoinAt = ticks + REJOIN_DELAY_TICKS;
        }
        rejoinPending = true;
        cmKick = true;
        char s[96];
        snprintf(s, sizeof(s), "W6 link lost (deauth / disassoc reason %u in state %u), result %u", deauthPendingReason, prev,
                 lastResult);
        step(s);
    }
    secService();
    joinService();
    scanService();
    cmService();
    frontEndService();
    stallService();
    if (linkUp && portOpen && ticks - lastTxTick > 10000) keepalive();
    if (ticks % 100 == 0) txSweep();
    baService();
    if (ticks % 10 == 0) rxBaTimeouts();
    if (ticks % 1000 == 0) bssAge();
    if (countryPending && ticks % 100 == 0) countryService();
    txInflightMax = txBaState == 2 ? txCapBa : txCapNoBa;
    if (linkUp && portOpen && htActive && txBaOn && txPackets > 200 && (txBaState == 0 || (txBaState == 1 && txBaRequests < 4)) &&
        (int32_t)(ticks - txBaNextTick) >= 0)
        txBaRequest();
    if (!w6Pass && ifEnabled && rxPackets && txPackets) {
        w6Pass = true;
        step("W6 pass: en1 passing traffic");
    }
    if (ticks % 1000 == 0) publishW6();
}

void MtkWlan::publishW6() {
    setNum("MtkW6Ticks", ticks);
    setNum("MtkW6TxPackets", txPackets);
    setNum("MtkW6TxStall", txStall);
    setNum("MtkW6TxDrop", txDrop);
    setNum("MtkW6TxRingFull", txRingFull);
    setNum("MtkW6TxFreeEvents", txFreeEvents);
    setNum("MtkW6TxFreed", txFreed);
    setNum("MtkW6TxFreeBadVer", txFreeBadVer);
    setNum("MtkW6TxLostFree", txLostFree);
    setNum("MtkW6TxFailed", txFailed);
    setNum("MtkW6TxCapped", txCapped);
    setNum("MtkW8AddbaDeclined", addbaDeclined);
    setNum("MtkW8RxAmsdu", rxAmsdu);
    if (txsRecords) {
        char t[200];
        snprintf(t, sizeof(t), "n %u ackerr %u avg_tries x10 %u avg_pwr x10 %d mode %u/%u/%u/%u/%u nss %u/%u mcs0-9 %u/%u/%u/%u/%u/%u/%u/%u/%u/%u bw %u/%u/%u",
                 txsRecords, txsAckErr, txsCountSum * 10 / txsRecords, (int)(txsPwrSum * 10 / (int32_t)txsRecords),
                 txsMode[0], txsMode[1], txsMode[2], txsMode[4], txsMode[8], txsNss[0], txsNss[1],
                 txsMcs[0], txsMcs[1], txsMcs[2], txsMcs[3], txsMcs[4], txsMcs[5], txsMcs[6], txsMcs[7], txsMcs[8], txsMcs[9],
                 txsBw[0], txsBw[1], txsBw[2]);
        setProperty("MtkW9Txs", t);
        snprintf(t, sizeof(t), "%08x %08x %08x %08x %08x %08x %08x %08x", txsLast[0], txsLast[1], txsLast[2], txsLast[3],
                 txsLast[4], txsLast[5], txsLast[6], txsLast[7]);
        setProperty("MtkW9TxsLast", t);
        setNum("MtkW9TxsAvgDelay", txsDelaySum / txsRecords);
        setNum("MtkW9TxsSeqSpan", txsSeqSpan);
    }
    setNum("MtkW9TxFreeHeaders", tfHeaders);
    setNum("MtkW9TxFreeAttempts", tfAttempts);
    setNum("MtkW9TxFreeStatErr", tfStatErr);
    char b[200];
    snprintf(b, sizeof(b), "rx sessions %u buffered %u dup %u timeout %u bar %u delba %u | tx state %u req %u status %u win %u ssn %u | rc %d",
             baRxSessions, baBuffered, baDup, baTimeoutRelease, barRx, delbaRx, txBaState, txBaRequests, baTxStatus, txBaWin,
             txBaSsn, (int)baLastRc);
    setProperty("MtkW9Ba", b);
    setNum("MtkW6TxInFlightPeak", txInFlightPeak);
    setNum("MtkW6RxPackets", rxPackets);
    setNum("MtkW6RxBad", rxBad);
    setNum("MtkW6RxPartial", rxPartial);
    setNum("MtkW6RxTrans", rxTrans);
    setNum("MtkW6RxNonSnap", rxNonSnap);
    setNum("MtkW6RxOwn", rxOwn);
    setNum("MtkW6RxDropped", rxDropped);
    setNum("MtkW6RxFrames", rxFrames);
    setNum("MtkW6Deauths", deauths);
    setNum("MtkW6LastDeauth", lastDeauth);
    setNum("MtkW6Rejoins", rejoins);
    setNum("MtkW6Keepalives", keepalives);
    setNum("MtkW6Link", (linkUp ? 1 : 0) | (ifEnabled ? 2 : 0) | (linkReported ? 4 : 0) | (portOpen ? 8 : 0));
    char s[200];
    size_t k = 0;
    s[0] = 0;
    for (int t = 0; t < 32 && k + 12 < sizeof(s); t++)
        if (rxTypes[t]) k += snprintf(s + k, sizeof(s) - k, "%s%d:%u", k ? " " : "", t, rxTypes[t]);
    setProperty("MtkW6RxTypes", s[0] ? s : "none");
    k = 0;
    s[0] = 0;
    for (int e = 0; e < 256 && k + 12 < sizeof(s); e++)
        if (evCount[e]) k += snprintf(s + k, sizeof(s) - k, "%s%02x:%u", k ? " " : "", e, evCount[e]);
    setProperty("MtkW6EventIds", s[0] ? s : "none");
    // 0.9.0 machines
    snprintf(s, sizeof(s), "scans %u fails %u bss %d running %u wanted %u userpending %u", scans, scanFails, nBss, scanRunning,
             scanWanted, userScanPending);
    setProperty("MtkW1Scan", s);
    snprintf(s, sizeof(s), "state %u joins %u fails %u last %u rejoins %u authtx %u assoctx %u tries %u", joinState, joins,
             joinFailures, lastResult, rejoins, authTx, assocTx, joinTries);
    setProperty("MtkW1Join", s);
    snprintf(s, sizeof(s), "m1 %u m2 %u m3 %u m4 %u g1 %u g2 %u badmic %u replays %u reinst %u pnreplay %u nopn %u icv %u mismatch %u unprot %u portdrop %u trans %u eapol rx %u tx %u qdrop %u installs %u/%u",
             supp.m1, supp.m2, supp.m3, supp.m4, supp.g1, supp.g2, supp.bad_mic, supp.replays, supp.reinstall_skipped, rxPnReplays, rxNoPn,
             rxIcvErr, rxCipherMismatch, rxUnprotDrop, rxPortDrop, rxTransDrop, rxEapol, txEapolCount, eapolQDropped, secInstalls,
             secInstallFails);
    setProperty("MtkW2Sec", s);
    snprintf(s, sizeof(s), "want %u pmk %u autoopen %u (%u ssids) fails %u runs %u switch %u disc %u rejoinpend %u", wantActive, wantHasPmk,
             autoOpen, nAutoOpenSsids, cmFails, cmRuns, switchPending, disconnectPending, rejoinPending);
    setProperty("MtkCm", s);
    snprintf(s, sizeof(s), "indispatch %u timeouts %u fails %u dead %u", uniCmdInDispatch, uniCmdTimeouts, uniCmdFails, fwDead);
    setProperty("MtkW2Cmd", s);
}

// Called at the end of start() once the hardware is ready: the Ethernet front end (medium, interface, link
// down), then the timer, armed whatever the front end did, so the machines and the user client always run
int MtkWlan::netStage() {
    IOWorkLoop *wl = getWorkLoop();
    if (!wl) { step("W6 fail: no work loop"); return -1; }
    timer = IOTimerEventSource::timerEventSource(this, &MtkWlan::timerFired);
    if (!timer || wl->addEventSource(timer) != kIOReturnSuccess) { step("W6 fail: timer"); return -2; }

    if (feNativeWanted) {
        // 0.10.0: MtkWlanNative matches us (registerService ran in start) and registers through the ops table; the Ethernet
        // front end only starts if it declines (MTK_CORE_FE_DECLINED) or has not registered within the fallback time
        feWantedAt = ticks;
        feFallbackCall = thread_call_allocate(&MtkWlan::feFallbackThread, this);
        setProperty("MtkFrontEndWanted", "native");
        timer->setTimeoutUS(TICK_US);
        step("W6 waiting for the native front end (MtkWlanNative)");
        return 0;
    }
    int fr = startFrontEnd();
    setNum("MtkW0FrontEnd", (uint32_t)fr);
    timer->setTimeoutUS(TICK_US);
    if (fr) { step("W6 fail: Ethernet front end (MtkWlanEth) did not start; machines running without it"); return -4; }
    step("W6 interface attached (link down until a network is joined)");
    return 0;
}

constexpr uint32_t FE_FALLBACK_TICKS = 15000;   // 15 s at the 1 ms tick: nothing has claimed the core
constexpr uint32_t FE_CLAIM_TICKS = 90000;      // 90 s once MtkWlanNative claimed it (its interface start took up to 26 s)

// Tick: while the native front end is wanted and nothing has registered, start the Ethernet one after a decline or
// FE_FALLBACK_TICKS. The start itself runs on its own thread (IONetworkController::start and the interface attach belong
// off the gate, as in the normal path), never from the tick.
void MtkWlan::frontEndService() {
    if (!feNativeWanted || feFallbackDone || fe.rx || !feFallbackCall) return;
    if (!feDeclined && ticks - feWantedAt < (feClaimed ? FE_CLAIM_TICKS : FE_FALLBACK_TICKS)) return;
    feFallbackDone = true;
    setProperty("MtkFrontEndFallback", feDeclined ? "native front end declined, Ethernet front end started"
                                       : feClaimed ? "native front end claimed the core but did not register in 90 s, Ethernet front end started"
                                                   : "native front end did not register in time, Ethernet front end started");
    thread_call_enter(feFallbackCall);
}

void MtkWlan::feFallbackThread(void *owner, void *) {
    MtkWlan *self = static_cast<MtkWlan *>(owner);
    int fr = self->startFrontEnd();
    self->setNum("MtkW0FrontEnd", (uint32_t)fr);
    self->step(fr ? "W6 fail: Ethernet front end (MtkWlanEth) did not start after the native fallback"
                  : "W6 interface attached by the fallback (link down until a network is joined)");
}

// The front end is a child in the service plane, found by class name so the core needs no MtkWlanEth header.
// A front end that fails start() stays attached and referenced: IONetworkController::start may already have
// put its command gate on our work loop, so it is never freed while the core runs.
int MtkWlan::startFrontEnd() {
    OSObject *o = OSMetaClass::allocClassWithName("MtkWlanEth");
    IOService *s = OSDynamicCast(IOService, o);
    if (!s) { OSSafeReleaseNULL(o); return -1; }
    if (!s->init() || !s->attach(this)) { s->release(); return -2; }
    frontEnd = s;
    return s->start(this) ? 0 : -3;
}

// ---------------------------------------------------------------- ops table (MtkCoreApi.h)

IOWorkLoop *MtkWlan::getWorkLoop() const {
    return workLoop;
}

const mtk_core_ops MtkWlan::kOps = {
    MTK_CORE_API_V1, sizeof(mtk_core_ops), &MtkWlan::opSetCallbacks, &MtkWlan::opTx, &MtkWlan::opSetEnabled,
    &MtkWlan::opGetMac, &MtkWlan::opGetLinkInfo,
};

const mtk_core_ops_v2 MtkWlan::kOpsV2 = {
    {MTK_CORE_API_V2, sizeof(mtk_core_ops_v2), &MtkWlan::opSetCallbacks, &MtkWlan::opTx, &MtkWlan::opSetEnabled,
     &MtkWlan::opGetMac, &MtkWlan::opGetLinkInfo},
    &MtkWlan::opScan, &MtkWlan::opResults, &MtkWlan::opJoin, &MtkWlan::opDisconnect, &MtkWlan::opStatus,
};

const mtk_core_ops_v3 MtkWlan::kOpsV3 = {
    {{MTK_CORE_API_V3, sizeof(mtk_core_ops_v3), &MtkWlan::opSetCallbacks, &MtkWlan::opTx, &MtkWlan::opSetEnabled,
      &MtkWlan::opGetMac, &MtkWlan::opGetLinkInfo},
     &MtkWlan::opScan, &MtkWlan::opResults, &MtkWlan::opJoin, &MtkWlan::opDisconnect, &MtkWlan::opStatus},
    &MtkWlan::opSetMode, &MtkWlan::opGetBss, &MtkWlan::opGetChannels, &MtkWlan::opTxBytes,
};

const mtk_core_ops_v4 MtkWlan::kOpsV4 = {
    {{{MTK_CORE_API_V4, sizeof(mtk_core_ops_v4), &MtkWlan::opSetCallbacks, &MtkWlan::opTx, &MtkWlan::opSetEnabled,
       &MtkWlan::opGetMac, &MtkWlan::opGetLinkInfo},
      &MtkWlan::opScan, &MtkWlan::opResults, &MtkWlan::opJoin, &MtkWlan::opDisconnect, &MtkWlan::opStatus},
     &MtkWlan::opSetMode, &MtkWlan::opGetBss, &MtkWlan::opGetChannels, &MtkWlan::opTxBytes},
    &MtkWlan::opSetMac,
};

const mtk_core_ops_v5 MtkWlan::kOpsV5 = {
    {{{{MTK_CORE_API_V5, sizeof(mtk_core_ops_v5), &MtkWlan::opSetCallbacks, &MtkWlan::opTx, &MtkWlan::opSetEnabled,
        &MtkWlan::opGetMac, &MtkWlan::opGetLinkInfo},
       &MtkWlan::opScan, &MtkWlan::opResults, &MtkWlan::opJoin, &MtkWlan::opDisconnect, &MtkWlan::opStatus},
      &MtkWlan::opSetMode, &MtkWlan::opGetBss, &MtkWlan::opGetChannels, &MtkWlan::opTxBytes},
     &MtkWlan::opSetMac},
    &MtkWlan::opGetRateInfo, &MtkWlan::opSetCountry,
};

IOReturn MtkWlan::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2,
                                       void *param3, void *param4) {
    if (functionName && functionName->isEqualTo(MTK_CORE_OPS_V1)) {
        if (!param1 || !param2) return kIOReturnBadArgument;
        *(const mtk_core_ops **)param1 = &kOps;
        *(void **)param2 = this;
        return kIOReturnSuccess;
    }
    if (functionName && functionName->isEqualTo(MTK_CORE_OPS_V2)) {
        if (!param1 || !param2) return kIOReturnBadArgument;
        *(const mtk_core_ops_v2 **)param1 = &kOpsV2;
        *(void **)param2 = this;
        return kIOReturnSuccess;
    }
    if (functionName && functionName->isEqualTo(MTK_CORE_OPS_V3)) {
        if (!param1 || !param2) return kIOReturnBadArgument;
        *(const mtk_core_ops_v3 **)param1 = &kOpsV3;
        *(void **)param2 = this;
        return kIOReturnSuccess;
    }
    if (functionName && functionName->isEqualTo(MTK_CORE_OPS_V4)) {
        if (!param1 || !param2) return kIOReturnBadArgument;
        *(const mtk_core_ops_v4 **)param1 = &kOpsV4;
        *(void **)param2 = this;
        return kIOReturnSuccess;
    }
    if (functionName && functionName->isEqualTo(MTK_CORE_OPS_V5)) {
        if (!param1 || !param2) return kIOReturnBadArgument;
        *(const mtk_core_ops_v5 **)param1 = &kOpsV5;
        *(void **)param2 = this;
        return kIOReturnSuccess;
    }
    if (functionName && functionName->isEqualTo(MTK_CORE_FE_DECLINED)) {
        feDeclined = true;                      // the tick's frontEndService starts the Ethernet front end
        return kIOReturnSuccess;
    }
    if (functionName && functionName->isEqualTo(MTK_CORE_FE_CLAIM)) {
        // usually before netStage: MtkWlanNative probes as soon as start() publishes the core
        feClaimed = true;
        if (feFallbackCall) setNum("MtkFrontEndClaimMs", ticks - feWantedAt);
        else setProperty("MtkFrontEndClaimMs", "before the wait began");
        return kIOReturnSuccess;
    }
    return IOService::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

// Register or remove the front end under the gate, so a running tick never sees half a table. A v1 table
// (MtkWlanEth) or a v2 table (scan_done / join_result added) is accepted.
int MtkWlan::opSetCallbacks(void *core, const mtk_fe_callbacks *cb) {
    MtkWlan *self = static_cast<MtkWlan *>(core);
    if (cb) {
        bool v1 = cb->version == MTK_CORE_API_V1 && cb->size >= sizeof(mtk_fe_callbacks);
        bool v2 = (cb->version == MTK_CORE_API_V2 || cb->version == MTK_CORE_API_V3 || cb->version == MTK_CORE_API_V4) &&
                  cb->size >= sizeof(mtk_fe_callbacks_v2);
        if ((!v1 && !v2) || !cb->rx_alloc || !cb->rx || !cb->rx_flush || !cb->link || !cb->tx_space) return -1;
    }
    if (!self->workLoop) return -1;
    return self->workLoop->runAction(&MtkWlan::gatedSetCallbacks, self, (void *)cb) == kIOReturnSuccess ? 0 : -1;
}

IOReturn MtkWlan::gatedSetCallbacks(OSObject *owner, void *arg, void *, void *, void *) {
    MtkWlan *self = static_cast<MtkWlan *>(owner);
    const mtk_fe_callbacks *cb = static_cast<const mtk_fe_callbacks *>(arg);
    if (cb) {
        if (self->fe.rx) return kIOReturnBusy;  // one front end at a time
        memcpy(&self->fe, cb, sizeof(self->fe));
        // how long the native front end took from the wait start: the margin the fallback window has to cover
        if (self->feNativeWanted && self->feFallbackCall && cb->version >= MTK_CORE_API_V3)
            self->setNum("MtkFrontEndBoundMs", self->ticks - self->feWantedAt);
        if (cb->version >= MTK_CORE_API_V2) {
            const mtk_fe_callbacks_v2 *v2 = reinterpret_cast<const mtk_fe_callbacks_v2 *>(cb);
            self->feScanDone = v2->scan_done;
            self->feJoinResult = v2->join_result;
        } else {
            self->feScanDone = nullptr;
            self->feJoinResult = nullptr;
        }
        return kIOReturnSuccess;
    }
    bzero(&self->fe, sizeof(self->fe));
    self->feScanDone = nullptr;
    self->feJoinResult = nullptr;
    self->feMode = MTK_FE_ETHERNET;             // a front end that leaves hands the joins back to the manager
    self->feModeFlags = 0;
    self->ifEnabled = false;
    self->linkReported = false;
    self->rxQueued = false;
    return kIOReturnSuccess;
}

int MtkWlan::opTx(void *core, mbuf_t m) {
    return static_cast<MtkWlan *>(core)->txEthernet(m);
}

// v3 (gate held by the caller)
int MtkWlan::opSetMode(void *core, uint32_t mode, uint32_t flags) {
    MtkWlan *self = static_cast<MtkWlan *>(core);
    if (mode != MTK_FE_ETHERNET && mode != MTK_FE_NATIVE) return -1;
    self->feMode = mode;
    self->feModeFlags = flags;
    if (mode == MTK_FE_NATIVE && !(flags & MTK_FE_KEEP_AUTOOPEN)) self->autoOpen = false;   // macOS drives the joins
    else if (!self->wantActive) self->autoOpen = self->autoAllowed();
    self->setProperty("MtkFrontEndActive", mode == MTK_FE_NATIVE ? (flags & MTK_FE_KEEP_AUTOOPEN ? "native (auto-open kept)" : "native")
                                                            : "ethernet");
    return 0;
}
int MtkWlan::opGetBss(void *core, uint32_t index, mtk_bss_info *out) { return static_cast<MtkWlan *>(core)->coreGetBss(index, out); }
int MtkWlan::opGetChannels(void *core, uint8_t *list, uint32_t max, uint32_t *count) {
    if (!list || !count) return -1;
    *count = static_cast<MtkWlan *>(core)->coreChannels(list, max);
    return 0;
}
int MtkWlan::opTxBytes(void *core, const uint8_t *frame, uint32_t len) {
    return static_cast<MtkWlan *>(core)->txEthernetBytes(frame, len);
}

// v4 (gate held by the caller): the station address the front end was given (macOS's private address per network)
int MtkWlan::opSetMac(void *core, const uint8_t m[6]) { return static_cast<MtkWlan *>(core)->coreSetMac(m); }
int MtkWlan::coreSetMac(const uint8_t m[6]) {
    if (!m || (m[0] & 1) || !(m[0] | m[1] | m[2] | m[3] | m[4] | m[5])) return -1;
    if (!memcmp(mac, m, 6)) return 0;
    if (asleep) {
        // no station in the firmware while asleep: keep the address, the wake's hwInit(true) gives it to DEV_INFO
        memcpy(mac, m, 6);
        char s[18];
        snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        setProperty("MtkMacAddr", s);
        setNum("MtkMacChanges", ++macChanges);
        return 0;
    }
    if (joinState) joinTeardown(3);                 // reason 3, leaving; the manager rejoins the wanted network with the new address
    uint8_t old[6];
    memcpy(old, mac, 6);
    devInfo(false);
    memcpy(mac, m, 6);
    int rc = devInfo(true);
    if (rc) {
        memcpy(mac, old, 6);
        devInfo(true);
        setNum("MtkMacChangeRefused", (uint32_t)rc);
        return -2;
    }
    char s[18];
    snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    setProperty("MtkMacAddr", s);
    setNum("MtkMacChanges", ++macChanges);
    cmKick = true;
    return 0;
}

void MtkWlan::opSetEnabled(void *core, int enabled) {
    MtkWlan *self = static_cast<MtkWlan *>(core);
    self->ifEnabled = enabled != 0;
    self->reportLink();
}

// 802.11 data rates in 100 kbit/s for one stream, 800 ns guard interval: HT/VHT MCS 0-9 at 20, 40, 80 MHz
static const uint16_t kRate20[10] = { 65, 130, 195, 260, 390, 520, 585, 650, 780, 867 };
static const uint16_t kRate40[10] = { 135, 270, 405, 540, 810, 1080, 1215, 1350, 1620, 1800 };
static const uint16_t kRate80[10] = { 293, 585, 878, 1170, 1755, 2340, 2633, 2925, 3510, 3900 };
// legacy rates in 100 kbit/s by hardware rate code: CCK 0-3 (4-7 short preamble), OFDM codes 8-15 (mt76 mt76_rates)
static const uint16_t kCck[4] = { 10, 20, 55, 110 };
static const uint16_t kOfdm[8] = { 480, 240, 120, 60, 540, 360, 180, 90 };

static uint32_t phyKbps(uint8_t mcs, uint8_t nss, uint8_t bw, bool sgi) {
    if (mcs > 9) mcs = 9;
    uint32_t r = (bw == 2 ? kRate80 : bw == 1 ? kRate40 : kRate20)[mcs] * 100u * (nss ? nss : 1);
    return sgi ? r * 10 / 9 : r;
}

// Rate of the last TX status (TXS0 rate: mode 9:6, nss 13:10, index 5:0; BW 31:29), else the negotiated maximum
void MtkWlan::rateInfo(mtk_rate_info *out) const {
    uint8_t bw = htActive ? chBw : 0;
    bool sgi = htActive && (bw == 2 ? (apVhtCap & 0x20) : bw == 1 ? (apHtCap & 0x40) : (apHtCap & 0x20));
    uint8_t maxMcs = 0;
    if (vhtActive) {
        uint8_t m = apVhtRx & 3;                  // the AP's 1-stream RX map: 0 = MCS 0-7, 1 = 0-8, 2 = 0-9
        maxMcs = m == 2 ? 9 : m == 1 ? 8 : 7;
        if (bw == 0 && maxMcs == 9) maxMcs = 8;   // VHT20 MCS 9 is not valid for one or two streams
    } else if (htActive) {
        maxMcs = 7;
    }
    uint8_t nss = htActive ? apNss : 1;
    out->ap_short_gi = sgi;
    out->max_mcs = maxMcs;
    out->max_kbps = htActive ? phyKbps(maxMcs, nss, bw, sgi) : 54000;
    uint32_t r = txsLast[0] & 0x3fff, mode = (r >> 6) & 0xf, idx = r & 0x3f;
    if (txsRecords && mode <= 4) {
        uint8_t txBw = (uint8_t)((txsLast[0] >> 29) & 7);
        if (txBw > 2) txBw = 2;
        out->from_txs = 1;
        out->bw_mhz = (uint16_t)(20u << txBw);
        out->short_gi = mode >= 2 && sgi;         // TXS carries no GI; the firmware uses short GI when both allow it
        if (mode <= 1) {
            out->phy = MTK_PHY_LEGACY;
            out->nss = 1;
            out->mcs = 0;
            out->tx_kbps = (mode == 0 ? kCck[idx & 3] : kOfdm[idx & 7]) * 100u;
            out->short_gi = 0;
            return;
        }
        if (mode == 4) {
            out->phy = MTK_PHY_VHT;
            out->mcs = (uint8_t)(idx & 0xf);
            out->nss = (uint8_t)(((r >> 10) & 0xf) + 1);
        } else {                                  // HT / HT greenfield: the index carries the stream count
            out->phy = MTK_PHY_HT;
            out->mcs = (uint8_t)(idx & 7);
            out->nss = (uint8_t)(idx / 8 + 1);
        }
        if (out->nss > 2) out->nss = 2;
        out->tx_kbps = phyKbps(out->mcs, out->nss, txBw, out->short_gi);
        return;
    }
    out->from_txs = 0;
    out->phy = vhtActive ? MTK_PHY_VHT : htActive ? MTK_PHY_HT : MTK_PHY_LEGACY;
    out->mcs = maxMcs;
    out->nss = nss;
    out->bw_mhz = (uint16_t)(20u << bw);
    out->short_gi = sgi;
    out->tx_kbps = out->max_kbps;
}

int MtkWlan::opGetRateInfo(void *core, mtk_rate_info *out) {
    MtkWlan *self = static_cast<MtkWlan *>(core);
    if (!out || out->size < 8) return -1;
    mtk_rate_info r;
    bzero(&r, sizeof(r));
    if (!self->linkUp) return -2;
    self->rateInfo(&r);
    uint32_t n = out->size < sizeof(r) ? out->size : (uint32_t)sizeof(r);
    r.size = n;
    memcpy(out, &r, n);
    return 0;
}

int MtkWlan::opSetCountry(void *core, const char alpha2[2]) {
    MtkWlan *self = static_cast<MtkWlan *>(core);
    if (!alpha2) return -1;
    char cc[3] = { alpha2[0], alpha2[1], 0 };
    if ((cc[0] == 'X' && cc[1] == 'Z') || (cc[0] == '0' && cc[1] == '0')) { cc[0] = '0'; cc[1] = '0'; }
    else if (cc[0] < 'A' || cc[0] > 'Z' || cc[1] < 'A' || cc[1] > 'Z') return -1;
    if (!memcmp(cc, self->countryWanted, 2)) return 0;
    memcpy(self->countryWanted, cc, 3);
    self->countryPending = true;
    return 0;
}

// A new country goes to the firmware between scans and outside a join (domainStage sends three commands)
void MtkWlan::countryService() {
    if (!countryPending || fwDead) return;
    if (joinState && !(linkUp && portOpen)) return;
    if (scanStartedAt && !scanDone) return;
    countryPending = false;
    if (!memcmp(country, countryWanted, 2)) return;
    memcpy(country, countryWanted, 3);
    int rc = domainStage();
    char t[64];
    snprintf(t, sizeof(t), "country %s applied (rc %d)", country, rc);
    step(t);
}

int MtkWlan::opGetMac(void *core, uint8_t out[6]) {
    memcpy(out, static_cast<MtkWlan *>(core)->mac, 6);
    return 0;
}

int MtkWlan::opGetLinkInfo(void *core, mtk_link_info *out) {
    MtkWlan *self = static_cast<MtkWlan *>(core);
    bzero(out, sizeof(*out));
    out->up = self->linkUp;
    out->enabled = self->ifEnabled;
    memcpy(out->bssid, self->tgtBssid, 6);
    out->channel = self->tgtCh;
    out->bw = self->htActive ? self->chBw : 0;
    out->center = out->bw ? self->chCenter : self->tgtCh;
    out->nss = self->htActive ? self->apNss : 1;
    out->ht = self->htActive;
    out->vht = self->vhtActive;
    out->aid = self->aid;
    strlcpy(out->ssid, self->tgtSsid, sizeof(out->ssid));
    out->speed = self->linkUp && self->portOpen && self->ifEnabled ? LINK_SPEED : 0;
    return 0;
}

// v2 (gate held by the caller: a front end on the shared work loop, or runAction)
int MtkWlan::opScan(void *core) { return static_cast<MtkWlan *>(core)->coreScan(); }
int MtkWlan::opResults(void *core, mtk_scan_result *out, uint32_t max, uint32_t *count) {
    if (!out || !count) return -1;
    *count = static_cast<MtkWlan *>(core)->coreResults(out, max);
    return 0;
}
int MtkWlan::opJoin(void *core, const mtk_join_params *p) { return static_cast<MtkWlan *>(core)->coreJoin(p); }
int MtkWlan::opDisconnect(void *core) { return static_cast<MtkWlan *>(core)->coreDisconnect(); }
int MtkWlan::opStatus(void *core, mtk_status *out) {
    if (!out) return -1;
    static_cast<MtkWlan *>(core)->coreStatus(out);
    return 0;
}

// Ethernet -> 802.11 data: ToDS, A1 BSSID, A2 us, A3 DA, LLC/SNAP + ethertype, then the payload.
// The 0.7.0 outputPacket; the front end frees m unless the result is MTK_TX_STALL. Nothing leaves before the
// port is open; once the PTK is installed every frame goes out protected (hardware CCMP).
int MtkWlan::txEthernet(mbuf_t m) {
    uint32_t len = (uint32_t)mbuf_pkthdr_len(m);
    if (len < 14 || len > 1514) { txDrop++; return MTK_TX_DROPPED; }
    uint8_t eh[14];
    mbuf_copydata(m, 0, 14, eh);
    return txFrame(eh, len, m, nullptr);
}

// 0.10.0: the same frame as bytes (the native front end hands the Skywalk packet's buffer straight in)
int MtkWlan::txEthernetBytes(const uint8_t *frame, uint32_t len) {
    if (!frame || len < 14 || len > 1514) { txDrop++; return MTK_TX_DROPPED; }
    return txFrame(frame, len, nullptr, frame);
}

// eh = the 14-byte Ethernet header; the payload comes from the mbuf (m) or follows the header in bytes
int MtkWlan::txFrame(const uint8_t *eh, uint32_t len, mbuf_t m, const uint8_t *bytes) {
    if (!linkUp || !portOpen) {
        if (linkUp && !portOpen) txPortDrop++;
        txDrop++;
        return MTK_TX_DROPPED;
    }
    if (!txSlotReady(false)) {
        txStall++;
        txStalled = true;
        return MTK_TX_STALL;
    }
    uint8_t *f = txData.bufs.va + txData.head * txData.bufSize + 128;
    bool qos = htActive;                        // WMM was negotiated with the HT association
    bool protect = ptkInstalled;
    uint32_t h = qos ? 26 : 24;
    f[0] = qos ? 0x88 : 0x08;                   // QoS data / data
    f[1] = (uint8_t)(0x01 | (protect ? 0x40 : 0));   // ToDS, Protected once the keys are in the card
    f[2] = f[3] = 0;
    memcpy(f + 4, tgtBssid, 6);
    memcpy(f + 10, mac, 6);
    memcpy(f + 16, eh, 6);
    f[22] = f[23] = 0;
    if (qos) f[24] = f[25] = 0;                 // QoS control: TID 0, normal ack
    memcpy(f + h, kSnap, 6);
    f[h + 6] = eh[12];
    f[h + 7] = eh[13];
    if (m) mbuf_copydata(m, 14, len - 14, f + h + 8);
    else memcpy(f + h + 8, bytes + 14, len - 14);
    bool mc = eh[0] & 1;
    uint8_t rate = mc ? RATE_IDX_OFDM12 : (dataRate ? dataRate : htActive ? 0 : RATE_IDX_OFDM24);
    bool sample = !mc && (++txsSampleSeq & 63) == 0;
    // TXD3 BCM stays clear: addr1 is the AP (mt7925_mac_write_txwi derives it from addr1), only the rate follows the DA
    txQueue(h + 8 + len - 14, LMAC_AC_BE, 2, qos ? 8 : 0, rate, false, sample, h, 0, protect);
    txPackets++;
    return MTK_TX_OK;
}

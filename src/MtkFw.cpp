// <wtl> project: mtkwlan | file: src/MtkFw.cpp | author: watchthelight </wtl>
// SPDX-License-Identifier: ISC
// W2 (WFDMA rings, interrupts) and W3 (patch + RAM firmware download, N9 ready).
// Ported from Linux mt76 v6.18 (Copyright (C) 2023 MediaTek Inc., ISC):
//   mt7925/pci.c mt7925_pci_probe + mt7925_dma_init, mt792x_dma.c dma_disable/dma_enable/
//   dma_prefetch/wfsys_reset, dma.c queue reset/add_buf/kick, mt7925/pci_mcu.c mcu_init,
//   mt7925/mcu.c fill_message/parse_response, mt76_connac_mcu.c load_patch/load_ram helpers.
// Responses are taken by polling the RX MCU ring (no tasklet); the MSI handler only counts and acks.

#include "MtkWlan.hpp"

extern "C" {
extern const unsigned char mtk_fw_patch[];
extern const unsigned int mtk_fw_patch_len;
extern const char mtk_fw_patch_sha256[];
extern const unsigned char mtk_fw_ram[];
extern const unsigned int mtk_fw_ram_len;
extern const char mtk_fw_ram_sha256[];
}

namespace {
// --- registers (BAR offsets below 0x200000 are used as is, chip addresses go through the remap)
constexpr uint32_t WFDMA0                  = 0xd4000;
constexpr uint32_t WFDMA0_RST              = WFDMA0 + 0x100;
constexpr uint32_t WFDMA0_MCU2HOST_SW_INT_ENA = WFDMA0 + 0x1f4;
constexpr uint32_t WFDMA0_HOST_INT_STA     = WFDMA0 + 0x200;
constexpr uint32_t WFDMA0_HOST_INT_ENA     = WFDMA0 + 0x204;
constexpr uint32_t WFDMA0_GLO_CFG          = WFDMA0 + 0x208;
constexpr uint32_t WFDMA0_RST_DTX_PTR      = WFDMA0 + 0x20c;
constexpr uint32_t WFDMA0_RST_DRX_PTR      = WFDMA0 + 0x280;
constexpr uint32_t WFDMA0_INT_RX_PRI       = WFDMA0 + 0x298;
constexpr uint32_t WFDMA0_INT_TX_PRI       = WFDMA0 + 0x29c;
constexpr uint32_t WFDMA0_GLO_CFG_EXT0     = WFDMA0 + 0x2b0;
constexpr uint32_t WFDMA0_PRI_DLY_INT_CFG0 = WFDMA0 + 0x2f0;
constexpr uint32_t TX_RING_BASE            = WFDMA0 + 0x300;
constexpr uint32_t RX_RING_BASE            = WFDMA0 + 0x500;
constexpr uint32_t TX_RING0_EXT_CTRL       = WFDMA0 + 0x600;
constexpr uint32_t PCIE_MAC_INT_ENABLE     = 0x10188;
constexpr uint32_t PCIE_MAC_PM             = 0x10194;
constexpr uint32_t UWFDMA0_GLO_CFG_EXT1    = 0x7c0242b4;
constexpr uint32_t DMASHDL_SW_CONTROL      = 0x7c026004;
constexpr uint32_t WFDMA_DUMMY_CR          = 0x54000120;
constexpr uint32_t HW_EMI_CTL              = 0x18011100;
constexpr uint32_t WFSYS_SW_RST_B_REG      = 0x7c000140;
constexpr uint32_t CONN_ON_MISC            = 0x7c0600f0;

constexpr uint32_t GLO_TX_DMA_EN = 1u << 0, GLO_TX_DMA_BUSY = 1u << 1, GLO_RX_DMA_EN = 1u << 2,
                   GLO_RX_DMA_BUSY = 1u << 3, GLO_TX_WB_DDONE = 1u << 6, GLO_FIFO_DIS_CHECK = 1u << 11,
                   GLO_FIFO_LITTLE_ENDIAN = 1u << 12, GLO_RX_WB_DDONE = 1u << 13,
                   GLO_CSR_DISP_BASE_PTR_CHAIN_EN = 1u << 15, GLO_OMIT_RX_INFO_PFET2 = 1u << 21,
                   GLO_OMIT_RX_INFO = 1u << 27, GLO_OMIT_TX_INFO = 1u << 28, GLO_CLK_GAT_DIS = 1u << 30;
constexpr uint32_t GLO_DMA_SIZE_3 = 3u << 4;

// interrupt masks (mt7925/regs.h, mt792x_regs.h)
constexpr uint32_t INT_RX_DONE_WM = 1u << 0, INT_RX_DONE_DATA = 1u << 2, INT_TX_DONE_BAND0 = 1u << 4,
                   INT_TX_DONE_MCU = (1u << 25) | (1u << 27) | (1u << 26), INT_MCU_CMD = 1u << 29;
constexpr uint32_t INT_TX_DONE_ALL = INT_TX_DONE_MCU | INT_TX_DONE_BAND0 | 0x7fff0; // GENMASK(18, 4)

// descriptors (dma.h)
constexpr uint32_t DMA_CTL_SD_LEN0_SHIFT = 16, DMA_CTL_LAST_SEC0 = 1u << 30, DMA_CTL_DMA_DONE = 1u << 31;

// rings: Linux sizes are 2048/256/128/1536/512; smaller rings are enough for bring-up
constexpr uint16_t TXQ_BAND0 = 0, TXQ_MCU_WM = 15, TXQ_FWDL = 16, RXQ_MCU_WM = 0, RXQ_BAND0 = 2;

// MCU commands (mt76_connac_mcu.h)
constexpr uint8_t CMD_TARGET_ADDRESS_LEN_REQ = 0x01, CMD_FW_START_REQ = 0x02, CMD_PATCH_START_REQ = 0x05,
                  CMD_PATCH_FINISH_REQ = 0x07, CMD_PATCH_SEM_CONTROL = 0x10, CMD_FW_SCATTER = 0xee;
constexpr uint32_t PATCH_SEM_RELEASE = 0, PATCH_SEM_GET = 1;
constexpr int PATCH_IS_DL = 1, PATCH_NOT_DL_SEM_SUCCESS = 2, PATCH_REL_SEM_SUCCESS = 3;
constexpr uint32_t DL_MODE_ENCRYPT = 1u << 0, DL_MODE_RESET_SEC_IV = 1u << 3, DL_CONFIG_ENCRY_MODE_SEL = 1u << 6,
                   DL_MODE_NEED_RSP = 1u << 31;
constexpr uint8_t FW_FEATURE_SET_ENCRYPT = 1u << 0, FW_FEATURE_ENCRY_MODE = 1u << 4,
                  FW_FEATURE_OVERRIDE_ADDR = 1u << 5, FW_FEATURE_NON_DL = 1u << 6;
constexpr uint32_t FW_START_OVERRIDE = 1u << 0;
constexpr uint32_t FW_CHUNK = 4096;
constexpr uint32_t MCU_TXD_LEN = 64;   // struct mt76_connac2_mcu_txd
constexpr uint32_t MCU_RXD_SEQ = 37;   // struct mt7925_mcu_rxd: rxd[8], len, pkt_type_id, eid, seq
constexpr uint32_t MCU_RXD_STATUS = 40;

inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
} // namespace

// ---------------------------------------------------------------- rings

int MtkWlan::ringAlloc(MtkRing &r, bool rx, uint16_t idx, uint16_t n, uint32_t bufSize) {
    r.rx = rx;
    r.n = n;
    r.bufSize = bufSize;
    r.regs = (rx ? RX_RING_BASE : TX_RING_BASE) + idx * 0x10;
    int e = r.desc.alloc(n * 16, mapper);
    if (e) return 10 + e;
    e = r.bufs.alloc(n * bufSize, mapper);
    if (e) return 20 + e;
    ringReset(r);
    return 0;
}

// mt76_dma_queue_reset: every descriptor DMA_DONE (free), indexes to 0, base and size; also after a chip reset
void MtkWlan::ringReset(MtkRing &r) {
    for (uint16_t i = 0; i < r.n; i++)
        desc(r, i)[1] = DMA_CTL_DMA_DONE;
    r.desc.toDevice();
    rawWrite(r.regs + 8, 0);            // cpu_idx
    rawWrite(r.regs + 12, 0);           // dma_idx
    rawWrite(r.regs + 0, r.desc.iova);  // desc_base
    rawWrite(r.regs + 4, r.n);          // ring_size
    r.head = r.tail = (uint16_t)rawRead(r.regs + 12);
    if (r.rx) {
        // mt76_dma_rx_fill: hand ndesc - 1 buffers to the device
        for (uint16_t k = 0; k + 1 < r.n; k++)
            rxArm(r);
        rawWrite(r.regs + 8, r.head);
    }
}

void MtkWlan::rxArm(MtkRing &r) {
    volatile uint32_t *d = desc(r, r.head);
    d[0] = r.bufs.iova + r.head * r.bufSize;
    d[2] = 0;
    d[3] = 0;
    d[1] = r.bufSize << DMA_CTL_SD_LEN0_SHIFT;
    r.head = (r.head + 1) % r.n;
}

// Queue one buffer on a TX ring and wait until the device has fetched it (dma_idx caught up)
int MtkWlan::txSend(MtkRing &r, const void *data, uint32_t len) {
    if (len > r.bufSize) return -1;
    uint16_t i = r.head;
    memcpy(r.bufs.va + i * r.bufSize, data, len);
    volatile uint32_t *d = desc(r, i);
    d[0] = r.bufs.iova + i * r.bufSize;
    d[2] = 0;
    d[3] = 0;
    d[1] = (len << DMA_CTL_SD_LEN0_SHIFT) | DMA_CTL_LAST_SEC0;
    r.bufs.toDevice();
    r.head = (r.head + 1) % r.n;
    rawWrite(r.regs + 8, r.head); // mt76_dma_kick_queue
    for (int t = 0; t < 1000; t++) {
        if (rawRead(r.regs + 12) == r.head)
            return 0;
        IODelay(100);
    }
    return -2; // device did not take it within 100 ms
}

// Next completed RX buffer (mt76_dma_dequeue), or nullptr. Caller must call rxRelease after use.
const uint8_t *MtkWlan::rxPoll(MtkRing &r, uint32_t *len) {
    r.desc.fromDevice();
    volatile uint32_t *d = desc(r, r.tail);
    uint32_t ctrl = d[1];
    if (!(ctrl & DMA_CTL_DMA_DONE))
        return nullptr;
    *len = (ctrl >> DMA_CTL_SD_LEN0_SHIFT) & 0x3fff;
    rxCtrl = ctrl;
    return r.bufs.va + r.tail * r.bufSize;
}

void MtkWlan::rxRelease(MtkRing &r) {
    desc(r, r.tail)[1] = 0;
    r.tail = (r.tail + 1) % r.n;
    rxArm(r);
    rawWrite(r.regs + 8, r.head);
}

// ---------------------------------------------------------------- interrupts

void MtkWlan::interruptHandler(OSObject *owner, void *, IOService *, int) {
    MtkWlan *self = OSDynamicCast(MtkWlan, owner);
    if (!self) return;
    self->irqCount++;
    // mt792x_irq_handler / tasklet: read and acknowledge (write 1 to clear) the host status
    uint32_t sta = self->rawRead(WFDMA0_HOST_INT_STA);
    self->irqLastStatus = sta;
    self->irqStatusOr |= sta;
    self->rawWrite(WFDMA0_HOST_INT_STA, sta);
}

void MtkWlan::setupInterrupt() {
    int msi = -1;
    for (int i = 0; i < 8; i++) {
        int type = 0;
        if (pci->getInterruptType(i, &type) != kIOReturnSuccess) break;
        if (type & kIOInterruptTypePCIMessaged) { msi = i; break; }
    }
    setNum("MtkW2MsiIndex", (uint32_t)msi);
    if (msi < 0) return;
    IOReturn r = pci->registerInterrupt(msi, this, &MtkWlan::interruptHandler, nullptr);
    setNum("MtkW2IrqRegister", (uint32_t)r);
    if (r == kIOReturnSuccess) {
        pci->enableInterrupt(msi);
        irqIndex = msi;
    }
}

// ---------------------------------------------------------------- MCU messages

uint8_t MtkWlan::nextSeq() {
    uint8_t seq = ++mcuSeq & 0xf;
    if (!seq) seq = ++mcuSeq & 0xf;
    return seq;
}

// mt7925_mcu_fill_message for a legacy (non-UNI) command, sent on TX ring 15, then the reply
// matched by sequence number on RX ring 0 (mt7925_mcu_parse_response). Returns the status byte
// for PATCH_SEM_CONTROL / PATCH_FINISH_REQ, 0 for others, negative on failure.
int MtkWlan::mcuCmd(uint8_t cid, const void *payload, uint32_t plen) {
    uint8_t msg[MCU_TXD_LEN + 64] = {};
    if (plen > 64) return -10;
    uint8_t seq = nextSeq();
    uint32_t total = MCU_TXD_LEN + plen;
    uint32_t txd0 = (total & 0xffff) | (2u << 23) | (0x20u << 25); // TX_BYTES, PKT_FMT CMD, Q_IDX MCU_PORT_RX_Q0
    uint32_t txd1 = 1u << 14;                                        // HDR_FORMAT CMD (connac3 bits 15:14)
    OSWriteLittleInt32(msg, 0, txd0);
    OSWriteLittleInt32(msg, 4, txd1);
    OSWriteLittleInt16(msg, 32, (uint16_t)(total - 32)); // len: everything after txd[8]
    OSWriteLittleInt16(msg, 34, 0x8000);                 // pq_id: MCU_PQ_ID(MT_TX_PORT_IDX_MCU, RX_Q0)
    msg[36] = cid;
    msg[37] = 0xa0;                                      // MCU_PKT_ID
    msg[38] = 3;                                         // set_query MCU_Q_NA
    msg[39] = seq;
    msg[41] = 0;                                         // ext_cid
    msg[42] = 0;                                         // s2d_index MCU_S2D_H2N
    memcpy(msg + MCU_TXD_LEN, payload, plen);
    if (txSend(txMcu, msg, total) < 0) return -11;
    return mcuWait(cid, seq);
}

int MtkWlan::mcuWait(uint8_t cid, uint8_t seq) {
    for (int t = 0; t < 3000; t++) {       // mdev->mcu.timeout = 3 * HZ
        uint32_t len = 0;
        const uint8_t *p = rxPoll(rxMcu, &len);
        if (!p) { IOSleep(1); continue; }
        rxEvents++;
        bool match = len > MCU_RXD_STATUS && p[MCU_RXD_SEQ] == seq;
        int status = (len > MCU_RXD_STATUS) ? p[MCU_RXD_STATUS] : -1;
        rxRelease(rxMcu);
        if (!match) { rxUnmatched++; continue; }
        if (cid == CMD_PATCH_SEM_CONTROL || cid == CMD_PATCH_FINISH_REQ)
            return status;
        return 0;
    }
    return -12; // timeout
}

// FW_SCATTER: raw data, no header, TX ring 16, chunks of 4096; uses a sequence number but no reply
int MtkWlan::sendScatter(const uint8_t *data, uint32_t len) {
    while (len) {
        uint32_t cur = len < FW_CHUNK ? len : FW_CHUNK;
        (void)nextSeq();
        if (txSend(txFwdl, data, cur) < 0) return -1;
        data += cur;
        len -= cur;
        fwBytes += cur;
    }
    return 0;
}

int MtkWlan::initDownload(uint32_t addr, uint32_t len, uint32_t mode) {
    uint8_t req[12];
    OSWriteLittleInt32(req, 0, addr);
    OSWriteLittleInt32(req, 4, len);
    OSWriteLittleInt32(req, 8, mode);
    // mt76_connac_mcu_init_download: patch addresses use PATCH_START_REQ on mt7925
    uint8_t cid = (addr == 0x900000 || addr == 0xe0002800) ? CMD_PATCH_START_REQ : CMD_TARGET_ADDRESS_LEN_REQ;
    return mcuCmd(cid, req, sizeof(req));
}

int MtkWlan::patchSem(bool get) {
    uint8_t req[4];
    OSWriteLittleInt32(req, 0, get ? PATCH_SEM_GET : PATCH_SEM_RELEASE);
    return mcuCmd(CMD_PATCH_SEM_CONTROL, req, sizeof(req));
}

// mt76_connac2_load_patch
int MtkWlan::loadPatch() {
    const uint8_t *fw = mtk_fw_patch;
    int sem = patchSem(true);
    setNum("MtkW3PatchSemGet", (uint32_t)sem);
    if (sem == PATCH_IS_DL) return 0;
    if (sem != PATCH_NOT_DL_SEM_SUCCESS) return -1;

    uint32_t nRegion = be32(fw + 44);        // hdr->desc.n_region (desc at 32, n_region at +12)
    setNum("MtkW3PatchSections", nRegion);
    int ret = 0;
    for (uint32_t i = 0; i < nRegion && !ret; i++) {
        const uint8_t *sec = fw + 96 + i * 64;  // sizeof(patch_hdr) = 96, sizeof(patch_sec) = 64
        if ((be32(sec) & 0xffff) != 0x2) { ret = -2; break; }   // PATCH_SEC_TYPE_INFO
        uint32_t offs = be32(sec + 4), addr = be32(sec + 12), len = be32(sec + 16), info = be32(sec + 20);
        uint32_t mode = DL_MODE_NEED_RSP;     // mt76_connac2_get_data_mode
        if (info != 0xffffffff) {
            uint32_t enc = info >> 24;
            if (enc == 1) mode |= DL_MODE_ENCRYPT | (((info & 0xff) << 1) & 0x6) | DL_MODE_RESET_SEC_IV;
            else if (enc == 2) mode |= DL_MODE_ENCRYPT | DL_CONFIG_ENCRY_MODE_SEL | DL_MODE_RESET_SEC_IV;
        }
        if (offs + len > mtk_fw_patch_len) { ret = -3; break; }
        int r = initDownload(addr, len, mode);
        setNum(i ? "MtkW3PatchInit1" : "MtkW3PatchInit0", (uint32_t)r);
        if (r) { ret = -4; break; }
        if (sendScatter(fw + offs, len)) { ret = -5; break; }
    }
    if (!ret) {
        uint8_t req[4] = {0};               // check_crc = 0
        int st = mcuCmd(CMD_PATCH_FINISH_REQ, req, sizeof(req));
        setNum("MtkW3PatchFinish", (uint32_t)st);
        if (st) ret = -6;
    }
    int rel = patchSem(false);
    setNum("MtkW3PatchSemRelease", (uint32_t)rel);
    if (rel != PATCH_REL_SEM_SUCCESS && !ret) ret = -7;
    return ret;
}

// mt76_connac2_load_ram + mt76_connac_mcu_send_ram_firmware (WM only, no WA on mt7925)
int MtkWlan::loadRam() {
    const uint8_t *fw = mtk_fw_ram;
    const uint32_t trailerLen = 36, regionLen = 40;   // sizeof fw_trailer / fw_region (checked against the blob)
    const uint8_t *hdr = fw + mtk_fw_ram_len - trailerLen;
    uint32_t nRegion = hdr[2];
    setNum("MtkW3RamRegions", nRegion);
    char ver[32] = {};
    memcpy(ver, hdr + 7, 10);
    ver[10] = '-';
    memcpy(ver + 11, hdr + 17, 15);
    setProperty("MtkFwVersion", ver);

    uint32_t offset = 0, override = 0;
    for (uint32_t i = 0; i < nRegion; i++) {
        const uint8_t *reg = hdr - (nRegion - i) * regionLen;
        uint32_t addr = le32(reg + 16), len = le32(reg + 20);
        uint8_t feature = reg[24];
        if (!(feature & FW_FEATURE_NON_DL)) {
            if (feature & FW_FEATURE_OVERRIDE_ADDR) override = addr;
            uint32_t mode = DL_MODE_NEED_RSP | (((feature >> 1) & 3) << 1);   // mt76_connac_mcu_gen_dl_mode
            if (feature & FW_FEATURE_SET_ENCRYPT) mode |= DL_MODE_ENCRYPT | DL_MODE_RESET_SEC_IV;
            if (feature & FW_FEATURE_ENCRY_MODE) mode |= DL_CONFIG_ENCRY_MODE_SEL;
            if (offset + len > mtk_fw_ram_len) return -1;
            if (initDownload(addr, len, mode)) return -2;
            if (sendScatter(fw + offset, len)) return -3;
        }
        offset += len;
    }
    uint8_t req[8];
    OSWriteLittleInt32(req, 0, override ? FW_START_OVERRIDE : 0);   // option
    OSWriteLittleInt32(req, 4, override);                           // addr
    int st = mcuCmd(CMD_FW_START_REQ, req, sizeof(req));
    setNum("MtkW3FwStart", (uint32_t)st);
    return st ? -4 : 0;
}

// ---------------------------------------------------------------- W2 + W3 sequence

// W2 + W3 on a chip we own: WFSYS reset, WFDMA rings (allocated once, reset on later calls), firmware, N9 ready.
// Runs at attach and again from the attach recovery and the link supervisor (reload).
int MtkWlan::hwStart() {
    // rest of mt7925_pci_probe after drv_own
    rmw(HW_EMI_CTL, 1u << 1, 1u << 1);                         // SLPPROT_EN

    // mt792x_wfsys_reset: drop the firmware Windows left running
    rmw(WFSYS_SW_RST_B_REG, 1u << 0, 0);
    IOSleep(50);
    rmw(WFSYS_SW_RST_B_REG, 1u << 0, 1u << 0);
    bool rstDone = pollMsec(WFSYS_SW_RST_B_REG, 1u << 4, 1u << 4, 500, 10);
    setNum("MtkW2ConnOnMiscAfterReset", rr(CONN_ON_MISC));
    if (!rstDone) { step("W2 fail: wfsys reset"); return -1; }

    rawWrite(WFDMA0_HOST_INT_ENA, 0);
    rawWrite(PCIE_MAC_INT_ENABLE, 0xff);
    if (irqIndex < 0) setupInterrupt();

    // mt7925_dma_init -> mt792x_dma_disable(force)
    rawWrite(WFDMA0_GLO_CFG, rawRead(WFDMA0_GLO_CFG) & ~(GLO_TX_DMA_EN | GLO_RX_DMA_EN |
             GLO_CSR_DISP_BASE_PTR_CHAIN_EN | GLO_OMIT_TX_INFO | GLO_OMIT_RX_INFO | GLO_OMIT_RX_INFO_PFET2));
    if (!pollMsec(WFDMA0_GLO_CFG, GLO_TX_DMA_BUSY | GLO_RX_DMA_BUSY, 0, 100, 1)) {
        step("W2 fail: WFDMA still busy");
        return -2;
    }
    rawWrite(WFDMA0_GLO_CFG_EXT0, rawRead(WFDMA0_GLO_CFG_EXT0) & ~(1u << 6));   // DMASHDL off
    rmw(DMASHDL_SW_CONTROL, 1u << 28, 1u << 28);                                  // DMASHDL bypass
    rawWrite(WFDMA0_RST, rawRead(WFDMA0_RST) & ~((1u << 5) | (1u << 4)));
    rawWrite(WFDMA0_RST, rawRead(WFDMA0_RST) | (1u << 5) | (1u << 4));

    // rings
    int e;
    auto ring = [&](MtkRing &r, bool rx, uint16_t idx, uint16_t n, uint32_t size) {
        if (r.desc.va) { ringReset(r); return 0; }
        return ringAlloc(r, rx, idx, n, size);
    };
    if ((e = ring(txData, false, TXQ_BAND0, 1024, 2048)))  { setNum("MtkW2RingFail", 100 + e); step("W2 fail: ring alloc"); return -3; }
    rawWrite(TX_RING0_EXT_CTRL, 0x4);
    if ((e = ring(txMcu, false, TXQ_MCU_WM, 32, 2048)))  { setNum("MtkW2RingFail", 200 + e); step("W2 fail: ring alloc"); return -3; }
    if ((e = ring(txFwdl, false, TXQ_FWDL, 32, 4096)))   { setNum("MtkW2RingFail", 300 + e); step("W2 fail: ring alloc"); return -3; }
    if ((e = ring(rxMcu, true, RXQ_MCU_WM, 128, 2048)))   { setNum("MtkW2RingFail", 400 + e); step("W2 fail: ring alloc"); return -3; }
    if ((e = ring(rxData, true, RXQ_BAND0, 512, 2048)))   { setNum("MtkW2RingFail", 500 + e); step("W2 fail: ring alloc"); return -3; }
    setNum("MtkW2RxMcuIova", rxMcu.desc.iova);
    setNum("MtkW2RxMcuPhys", rxMcu.desc.phys, 64);   // equal to the iova: no VT-d translation

    // mt792x_dma_enable (mt7925 branch of mt792x_dma_prefetch)
    auto pf = [](uint32_t base, uint32_t depth) { return (base << 16) | depth; };
    rawWrite(WFDMA0 + 0x680, pf(0x0000, 0x4));
    rawWrite(WFDMA0 + 0x684, pf(0x0040, 0x4));
    rawWrite(WFDMA0 + 0x688, pf(0x0080, 0x4));
    rawWrite(WFDMA0 + 0x68c, pf(0x00c0, 0x4));
    rawWrite(WFDMA0 + 0x600, pf(0x0100, 0x10));
    rawWrite(WFDMA0 + 0x604, pf(0x0200, 0x10));
    rawWrite(WFDMA0 + 0x608, pf(0x0300, 0x10));
    rawWrite(WFDMA0 + 0x60c, pf(0x0400, 0x10));
    rawWrite(WFDMA0 + 0x63c, pf(0x0500, 0x4));
    rawWrite(WFDMA0 + 0x640, pf(0x0540, 0x4));
    rawWrite(WFDMA0_RST_DTX_PTR, ~0u);
    rawWrite(WFDMA0_RST_DRX_PTR, ~0u);
    rawWrite(WFDMA0_PRI_DLY_INT_CFG0, 0);
    rawWrite(WFDMA0_GLO_CFG, rawRead(WFDMA0_GLO_CFG) | GLO_TX_WB_DDONE | GLO_FIFO_LITTLE_ENDIAN |
             GLO_CLK_GAT_DIS | GLO_OMIT_TX_INFO | GLO_DMA_SIZE_3 | GLO_FIFO_DIS_CHECK | GLO_RX_WB_DDONE |
             GLO_CSR_DISP_BASE_PTR_CHAIN_EN | GLO_OMIT_RX_INFO_PFET2);
    rawWrite(WFDMA0_GLO_CFG, rawRead(WFDMA0_GLO_CFG) | GLO_TX_DMA_EN | GLO_RX_DMA_EN);
    rmw(UWFDMA0_GLO_CFG_EXT1, 1u << 28, 1u << 28);
    rawWrite(WFDMA0_INT_RX_PRI, rawRead(WFDMA0_INT_RX_PRI) | 0x0f00);
    rawWrite(WFDMA0_INT_TX_PRI, rawRead(WFDMA0_INT_TX_PRI) | 0x7f00);
    rmw(WFDMA_DUMMY_CR, 1u << 1, 1u << 1);                                       // NEED_REINIT
    rawWrite(WFDMA0_HOST_INT_ENA, INT_TX_DONE_ALL | INT_RX_DONE_DATA | INT_RX_DONE_WM | INT_MCU_CMD);
    rawWrite(WFDMA0_MCU2HOST_SW_INT_ENA, rawRead(WFDMA0_MCU2HOST_SW_INT_ENA) | 1u << 0);
    setNum("MtkW2GloCfg", rawRead(WFDMA0_GLO_CFG));
    step("W2 pass: WFDMA rings up");

    // mt7925e_mcu_init
    int fw = fwOwn(), drv = drvOwn();
    setNum("MtkW3FwOwnTries", (uint32_t)fw);
    setNum("MtkW3DrvOwnTries", (uint32_t)drv);
    if (drv < 0) { step("W3 fail: drv_own before firmware"); return -4; }
    rmw(PCIE_MAC_PM, 1u << 8, 1u << 8);                                           // L0S_DIS

    setProperty("MtkFwPatchSha256", mtk_fw_patch_sha256);
    setProperty("MtkFwRamSha256", mtk_fw_ram_sha256);
    int p = loadPatch();
    setNum("MtkW3PatchResult", (uint32_t)p);
    if (p) { step("W3 fail: patch download"); return -5; }
    step("W3 patch done");
    int r = loadRam();
    setNum("MtkW3RamResult", (uint32_t)r);
    if (r) { step("W3 fail: RAM download"); return -6; }

    bool ready = pollMsec(CONN_ON_MISC, 3, 3, 1500, 10);   // MT_TOP_MISC2_FW_N9_RDY
    setNum("MtkConnOnMisc", rr(CONN_ON_MISC));
    setNum("MtkW3FwBytes", fwBytes);
    setNum("MtkIrqCount", irqCount);
    setNum("MtkIrqLastStatus", irqLastStatus);
    setNum("MtkRxEvents", rxEvents);
    setNum("MtkRxUnmatched", rxUnmatched);
    if (!ready) { step("W3 fail: N9 not ready after FW_START"); return -7; }
    step("W3 pass: firmware running (N9 ready)");
    return 0;
}

int MtkWlan::bringUp() {
    int rc = hwStart();
    // I-081: a warm restart can leave the chip running the old firmware with its DMA pointing at freed memory, and the
    // patch download then fails. One more WFSYS reset and a second try, then a function level reset and a third.
    if (rc && rc != -3) {
        attachRecover++;
        setNum("MtkAttachRecover", attachRecover);
        setNum("MtkAttachFirstResult", (uint32_t)rc);
        IOLog("MtkWlan: attach failed (%d), WFSYS reset and retry\n", rc);
        fwOwn();
        drvOwn();
        rc = hwStart();
        if (rc && rc != -3 && pciFlr()) {
            fwOwn();
            if (drvOwn() > 0) rc = hwStart();
        }
        setNum("MtkAttachRecoverResult", (uint32_t)rc);
    }
    if (rc) return rc;
    setProperty("MtkMilestone", "W6");
    // 0.9.0: station ready, then the front end (link down) and the tick; the connection manager scans and joins
    if (hwInit() == 0) {
        scanWanted = true;
        netStage();
    }
    setNum("MtkIrqCount", irqCount);
    setNum("MtkRxEvents", rxEvents);
    setNum("MtkRxUnmatched", rxUnmatched);
    return 0;
}

// PCIe function level reset, the last resort when WFSYS reset did not bring the chip back. Config space (BARs, MSI)
// is saved and restored around it by IOPCIFamily, as for sleep. false when the card does not offer FLR.
bool MtkWlan::pciFlr() {
    uint8_t cap = 0;
    if (!pci->findPCICapability(kIOPCIPCIExpressCapability, &cap) || !cap) { flrUnsupported++; return false; }
    if (!(pci->configRead32(cap + 4) & (1u << 28))) {            // Device Capabilities: Function Level Reset
        flrUnsupported++;
        setNum("MtkFlrUnsupported", flrUnsupported);
        return false;
    }
    if (irqIndex >= 0) pci->disableInterrupt(irqIndex);
    pci->saveDeviceState();
    pci->configWrite16(cap + 8, pci->configRead16(cap + 8) | (1u << 15));   // Device Control: Initiate FLR
    IOSleep(100);                                                 // PCIe: 100 ms before the function is used again
    pci->restoreDeviceState();
    pci->setMemoryEnable(true);
    pci->setBusMasterEnable(true);
    if (irqIndex >= 0) pci->enableInterrupt(irqIndex);
    attachFlr++;
    setNum("MtkFlr", attachFlr);
    IOLog("MtkWlan: PCIe function level reset done\n");
    return true;
}

// mt7925_pci_shutdown -> mt7925e_unregister_device: driver own, DMA off, WFSYS reset. Without it a warm restart
// leaves the firmware running and the DMA rings pointing at freed host memory; the next boot's patch download fails
// (N-177, "W3 fail: patch download") and the card can drop off the bus until a cold power-off (I-081)
void MtkWlan::quiesce(const char *why) {
    if (quiesced || !pci || !mmio) return;
    quiesced = true;
    if (timer) timer->cancelTimeout();
    if (irqIndex >= 0) pci->disableInterrupt(irqIndex);
    rawWrite(WFDMA0_HOST_INT_ENA, 0);
    rawWrite(PCIE_MAC_INT_ENABLE, 0);
    int drv = drvOwn();
    rawWrite(WFDMA0_GLO_CFG, rawRead(WFDMA0_GLO_CFG) & ~(GLO_TX_DMA_EN | GLO_RX_DMA_EN));
    bool idle = pollMsec(WFDMA0_GLO_CFG, GLO_TX_DMA_BUSY | GLO_RX_DMA_BUSY, 0, 100, 1);
    rmw(WFSYS_SW_RST_B_REG, 1u << 0, 0);
    IOSleep(50);
    rmw(WFSYS_SW_RST_B_REG, 1u << 0, 1u << 0);
    bool rst = pollMsec(WFSYS_SW_RST_B_REG, 1u << 4, 1u << 4, 500, 10);
    pci->setBusMasterEnable(false);                               // no DMA into host memory past this point
    IOLog("MtkWlan: quiesced for %s (drv_own %d, dma idle %d, wfsys reset %d)\n", why, drv, idle, rst);
}

// mt7925_pci_suspend, hardware half (the station is already gone, see MtkNet.cpp sleepDown): driver own, wait for the
// WFDMA to go idle, DMA off, host interrupts off, then the chip back to the firmware (mt792xe_mcu_fw_pmctrl) so it can
// enter its own low-power state, and bus mastering off. The deep-sleep and HIF-suspend MCU commands Linux sends first
// are left out: the wake path resets WFSYS and downloads the firmware again whether or not the chip kept power.
void MtkWlan::sleepHw() {
    if (irqIndex >= 0) pci->disableInterrupt(irqIndex);
    int drv = drvOwn();
    bool idle = pollMsec(WFDMA0_GLO_CFG, GLO_TX_DMA_BUSY | GLO_RX_DMA_BUSY, 0, 1000, 1);   // "wait until dma is idle"
    rawWrite(WFDMA0_GLO_CFG, rawRead(WFDMA0_GLO_CFG) & ~(GLO_TX_DMA_EN | GLO_RX_DMA_EN));
    bool off = pollMsec(WFDMA0_GLO_CFG, GLO_TX_DMA_BUSY | GLO_RX_DMA_BUSY, 0, 100, 1);
    rawWrite(WFDMA0_HOST_INT_ENA, 0);
    rawWrite(PCIE_MAC_INT_ENABLE, 0);
    rawWrite(WFDMA0_HOST_INT_STA, rawRead(WFDMA0_HOST_INT_STA));  // nothing left pending for the wake
    int fw = fwOwn();
    pci->setBusMasterEnable(false);                               // no DMA into host memory while asleep
    char s[96];
    snprintf(s, sizeof(s), "drv_own %d, dma idle %d off %d, fw_own %d", drv, idle, off, fw);
    setProperty("MtkSleepHw", s);
    IOLog("MtkWlan: sleep: %s\n", s);
}

// mt7925_pci_resume up to the point where Linux falls back to mt792x_reset: bus master and memory on, the chip back for
// the driver (mt792xe_mcu_drv_pmctrl), then whether it kept power, read the way mt792x_dma_need_reinit does it (the
// WFDMA dummy register loses the NEED_REINIT bit hwStart sets when the chip lost power). Either way the chip then gets
// the attach bring-up: WFSYS reset, rings reset, patch and RAM download (as in bringUp: one more WFSYS reset and try,
// then a function level reset and a third). Returns hwStart's code, -9 when the card does not answer on the bus.
int MtkWlan::wakeHw(bool *powerLost) {
    *powerLost = false;
    pci->setMemoryEnable(true);
    pci->setBusMasterEnable(true);
    uint32_t misc = rr(CONN_ON_MISC);
    setNum("MtkWakeConnOnMisc", misc);
    if (misc == 0xffffffff) {                                     // all ones: the function is not there (or still in D3cold)
        IOSleep(100);
        misc = rr(CONN_ON_MISC);
        if (misc == 0xffffffff) return -9;
    }
    fwOwn();
    int drv = drvOwn();
    setNum("MtkWakeDrvOwnTries", (uint32_t)drv);
    if (drv < 0 && pciFlr()) {
        fwOwn();
        drv = drvOwn();
    }
    if (drv > 0) *powerLost = !(rr(WFDMA_DUMMY_CR) & (1u << 1));    // MT_WFDMA_NEED_REINIT
    if (irqIndex >= 0) pci->enableInterrupt(irqIndex);
    int rc = drv > 0 ? hwStart() : -4;
    if (rc && rc != -3) {
        IOLog("MtkWlan: wake bring-up failed (%d), WFSYS reset and retry\n", rc);
        fwOwn();
        rc = drvOwn() > 0 ? hwStart() : -4;
        if (rc && rc != -3 && pciFlr()) {
            fwOwn();
            if (drvOwn() > 0) rc = hwStart();
        }
    }
    return rc;
}

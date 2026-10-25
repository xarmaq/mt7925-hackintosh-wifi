# MtkWlan: MediaTek MT7925 Wi-Fi driver for macOS Hackintosh

Wi-Fi for the MediaTek MT7925 (Filogic 360, Wi-Fi 7, PCI `14C3:7925`, also the AMD RZ717 `14C3:0717`) on a
Hackintosh: a macOS kext written from scratch for OpenCore on macOS Sequoia 15 and Tahoe 26. It brings the card up,
loads its firmware, scans, joins open and WPA2-PSK networks and moves traffic at 190 Mb/s down, 175 Mb/s up on a
5 GHz network. macOS has no driver for this card and, as far as I know, no other project drives the MT7925.

With the second kext, [mt7925-hackintosh-wifi-native](https://github.com/watchthelight-labs/mt7925-hackintosh-wifi-native),
macOS treats the card as a real Wi-Fi card: the Wi-Fi icon in the menu bar, the Wi-Fi pane in System Settings, saved
networks, auto-join at boot and a private address per network, without a menu-bar client app or the command line.
SimpleMtkWlan and its forks show the card as Ethernet and join networks through a client app or a tool (comparison
below).

## The MT7925 set

There are three kexts for the one card: install the first two together for Wi-Fi and the third for Bluetooth.

| Repo | What it does | Needs |
|---|---|---|
| [mt7925-hackintosh-wifi](https://github.com/watchthelight-labs/mt7925-hackintosh-wifi) (this one) | the driver: firmware, DMA, scan, join, WPA2, data path; on its own macOS sees an Ethernet port | nothing (no Lilu) |
| [mt7925-hackintosh-wifi-native](https://github.com/watchthelight-labs/mt7925-hackintosh-wifi-native) | the native front end: macOS's own Wi-Fi menu, settings pane, saved networks, auto-join | this driver, macOS 26.7.1 (25G241) |
| [mt7925-hackintosh-bluetooth](https://github.com/watchthelight-labs/mt7925-hackintosh-bluetooth) | loads the Bluetooth firmware of the same card (USB `0489:E111` and the other MT7925 ids) | BlueToolFixup; on Tahoe also [LegionBtFix](https://github.com/watchthelight-labs/LegionBtFix) |

The quickest install is the bundle on the [releases page](../../releases): both Wi-Fi kexts, the licences and an
`INSTALL.md` with the exact `config.plist` lines. A full EFI that uses all three is
[Legion-5-15IAX10-OpenCore](https://github.com/watchthelight-labs/Legion-5-15IAX10-OpenCore).

## Which cards

| Card | PCI id | State |
|---|---|---|
| MediaTek MT7925 (Filogic 360), Lenovo Legion 5 15IAX10 and other 2025 laptops | `14C3:7925`, subsystem `17AA:E0FF` | tested daily, every number in this README comes from it |
| AMD RZ717 (MT7925 on AM5 desktop boards: ASRock X870 Steel Legend WiFi and others) | `14C3:0717` | matched since 0.11.1; the Bluetooth half is confirmed on an X870, the Wi-Fi half is waiting for a tester's report |
| Other MT7925 boards (different subsystem ids) | `14C3:7925` | should work, untested; report either way |
| MT7921, MT7921K (RZ608), MT7922 (RZ616), MT7920, MT7902 | `14C3:7961`, `0608`, `7922`, `0616`, ... | **not supported**: different firmware and a different init path. See "Other MediaTek cards" below for what it would take and how to help; until then SimpleMtkWlan and its forks cover those cards |

## Compared with SimpleMtkWlan and AirportMtkWlan

Both projects are good work and they cover cards this one does not. The table is from their READMEs as of
2026-10-20:

| | mt7925-hackintosh-wifi + -native | SimpleMtkWlan (pengubow and forks) | AirportMtkWlan (pengubow, mnnobre) |
|---|---|---|---|
| Cards | MT7925, RZ717 | MT7921, MT7921K/RZ608, MT7922, RZ616 | MT7922/RZ616 (pengubow), MT7921/RZ608 (mnnobre) |
| How you join a network | macOS's own Wi-Fi menu and System Settings; networks saved by macOS, auto-join at boot, private address per network | the card is an Ethernet port; `mtkwifi join` on the command line, HeliPort "not tested yet", or the SimpleMTK menu-bar client some forks ship | macOS's own Wi-Fi menu (experimental) |
| macOS | Sequoia 15 and Tahoe 26 for the driver; the native menu on Tahoe 26.7.1 (25G241) | "tested in the macOS 15 installer" | macOS 15.2 to 15.x only |
| Measured | 190.8 / 174.8 Mb/s, 16 ms idle ping (0.9.0, Ethernet front end); native front end holds the link, auto-joins at boot | not stated on the page | 83 / 44 Mb/s on a G713PV |
| 802.11 | HT/VHT at 80 MHz; no ax yet | n/ac/ax, 40/80 MHz | as SimpleMtkWlan |
| WPA2-PSK | code passes the host tests; **not yet joined on air** (only an open network here) | yes | yes |
| Licence | ISC (plus Clear BSD parts) | GPLv2 | GPLv2 |

On an MT7925 use this driver; with the native front end it behaves like Apple's own Wi-Fi. On an MT7921 or MT7922 use
SimpleMtkWlan for now and watch the issue linked below.

## Status

Everything below was measured on one laptop (Legion 5 15IAX10, OpenCore 1.0.8) on the same open 5 GHz network, with `tools/wifi.sh`. The Windows driver does about 208/211 Mb/s on that network.

| Version | What it adds | Tested on | Result |
|---|---|---|---|
| 0.11.0 | core ops v4: `set_mac` changes the station address after bring-up (the firmware station interface is re-added with it), so a native front end can apply the per-network private address macOS picks; `MtkMacAddr`, `MtkMacChanges` properties | macOS 26.7.1 (25G241) | with the native front end MtkWlanNative 0.4.9: Wi-Fi menu, macOS-driven joins with a private address, auto-join at boot, link held for ten quiet minutes; Ethernet front end unchanged |
| 0.10.0 | core ops v3 (`set_mode`, `get_bss`, `get_channels`, `tx_bytes`), front-end selection (`MtkFrontEnd`: Ethernet, native front end optional) with Ethernet fallback, `mtkwifi` shows the front end | macOS 26.7.1 (25G241) | 192.7 Mb/s down, 134.6 Mb/s up, 16 ms idle ping, 0 % loss; same auto join at boot |
| 0.9.0 | scan and join machines, `mtkwifi` tool, saved networks, auto join at boot, WPA2-PSK code | macOS 26.7.1 (25G241) | 190.8 Mb/s down, 174.8 Mb/s up, 16 ms idle ping, 0 % loss; joins the network by itself at boot |
| 0.8.0 | core / front end split, native macOS build | macOS 26.7.1 | 195 / 97 Mb/s |
| 0.7.0 | block ack both ways, TX power tables | macOS 15.8.1 (24H32) | 193 / 146 Mb/s |
| 0.6.0 | HT/VHT association, firmware rate control | macOS 15.4.1 Recovery | 47.4 / 9.2 Mb/s |
| 0.5.0 | first join, Ethernet interface, DHCP | macOS 15.4.1 Recovery | Safari and an Ookla speedtest: 19.4 / 9.6 Mb/s |

**WPA2 is not proven on air yet.** The WPA2-PSK code (4-way handshake, group rekey, key install, CCMP replay check) passes the host tests in `tests/` (standard test vectors plus simulated handshakes, every derived value checked against an independent Python oracle), but I have not joined a real WPA2 network with it: the only network where I live is an open one. Until this paragraph changes, treat WPA2 as untested. If you try it, open an issue with the result either way.

## What it can't do

- With this kext alone there is no Wi-Fi menu: macOS sees an Ethernet card and you pick networks with the `mtkwifi` command line tool. There is no Wi-Fi icon, no Wi-Fi pane in System Settings, no AirDrop, no Continuity. The native front end is a second kext, [MtkWlanNative](https://github.com/watchthelight-labs/mt7925-hackintosh-wifi-native) (0.4.9, macOS Tahoe 26.7.1 only), built against the same core ops table: with it the Wi-Fi menu works, macOS runs the scans and joins, saves networks and auto-joins at boot.
- It can't join WPA3-SAE only, WPA2 with PSK-SHA256 only, networks that require management frame protection, WPA (TKIP pairwise) or enterprise (802.1X) networks. `mtkwifi list` marks these.
- No roaming between access points of one network. No HE (802.11ax) or EHT; the association is VHT at 80 MHz.
- Sleep and wake: upstream has none (the laptop it was written on has sleep disabled). The `stealth-a16` fork adds it (see the changelog, 0.12.1-stealth.1): the station is torn down before sleep and the chip gets the full attach bring-up on wake. Not yet tested on hardware; `-mtknosleep` turns it off.
- Three firmware command timeouts in a row stop the driver until a reboot.
- RX is polled every millisecond from a timer, not driven by the interrupt.
- Bluetooth on this card is a separate USB device (`0489:E111`) and needs its own firmware loader, which is a separate project.
- Tested on one laptop and one card revision.

## Installation

The bundle zip from the [releases](../../releases) holds `MtkWlan.kext`, `MtkWlanNative.kext`, the licences and
`INSTALL.md` with these steps. To install from the parts instead:

1. Get `MtkWlan.kext` from the [releases](../../releases) (the release zip carries `LICENSE`, `NOTICE.md` and
   `LICENSES/` because the licences require them next to the binary), or build it (below). For the Wi-Fi menu also get
   `MtkWlanNative.kext` from [its releases](https://github.com/watchthelight-labs/mt7925-hackintosh-wifi-native/releases)
   (macOS 26.7.1 build 25G241 only; on Sequoia use the driver alone with the `mtkwifi` tool).
2. Copy the kext(s) to `EFI/OC/Kexts/` and add them to `config.plist`, `Kernel > Add`, in this order:

   ```
   BundlePath      MtkWlan.kext
   ExecutablePath  Contents/MacOS/MtkWlan
   PlistPath       Contents/Info.plist
   MinKernel       (empty)
   MaxKernel       (empty)
   Enabled         true

   BundlePath      MtkWlanNative.kext          (optional, after MtkWlan: it links against the core)
   ExecutablePath  Contents/MacOS/MtkWlanNative
   PlistPath       Contents/Info.plist
   Enabled         true
   ```

   Neither needs Lilu: the driver depends on Apple's IONetworkingFamily and IOPCIFamily, the front end on IO80211Family
   and IOSkywalkFamily. Tested on Darwin 24 (macOS 15) and 25 (macOS 26).
3. With the native front end, add `mtkfe=native mtknativeif=1` to `boot-args` (`NVRAM > Add >
   7C436110-AB2A-4BBB-A880-FE41995C9F82`). Without them the driver keeps its Ethernet front end and the second kext
   stays idle, so a wrong build still leaves you with network. Make sure the card's PCI slot is not in a USB map or
   DeviceProperties entry that disables it.
4. Reboot and check it loaded:

   ```
   kmutil showloaded | grep Mtk
   ioreg -r -c MtkWlan -l -w0 | grep MtkState
   ifconfig en1
   ```

   `MtkState` names the bring-up step (`W1` PCI to `W6` interface) and says `pass` or `fail` with the reason; after
   bring-up it reads `W6 interface attached (link down until a network is joined)`, then `W5 pass: associated to ...`
   and `W6 pass: en1 passing traffic` once joined. With the native front end the Wi-Fi icon appears in the menu bar and
   you join from there, like on a Mac. Without it, build the tool and save your network (it asks for the passphrase at
   the prompt and stores only the derived PMK, root-only):

   ```
   cd tools/mtkwifi && ./build.sh && sudo bash install.sh
   sudo mtkwifi add "<your SSID>"          # WPA2-PSK; add --open for an open network
   sudo mtkwifi join "<your SSID>"         # or just reboot: the autojoin daemon joins the first saved network in range
   mtkwifi status
   ```

   `tools/mtkwifi/README.md` has the full command list.
5. Bluetooth is the same card's USB half and its own kext: [mt7925-hackintosh-bluetooth](https://github.com/watchthelight-labs/mt7925-hackintosh-bluetooth).
6. Rollback: set `Enabled` to false on the kext entries, or boot from a backup EFI. The tool's undo lines are in the
   header of `tools/mtkwifi/install.sh`. Nothing is written to NVRAM or firmware.

## Properties

Set in `Info.plist` under the `MtkWlan` personality (edit the copy inside the kext or rebuild):

| Property | Default | Effect |
|---|---|---|
| `MtkAutoOpenSsids` | `Boingo 5GHz`, `Boingo` | Open networks the driver joins by itself when nothing is saved or asked for (exact names, 20 s after the first scan). Put your café's name here, or empty the list. |
| `MtkPower` | true | Send the firmware's country limit tables and per-rate TX power limits (US, indoor). Off means the firmware's conservative default power. |
| `MtkRxBa`, `MtkTxBa` | true | Block ack in each direction. Off declines ADDBA requests (slower, as in 0.6.x). |
| `MtkHt` | true | HT/VHT association. Off joins with legacy rates only. |
| `MtkMaxBw` | 2 | Widest channel: 0 = 20 MHz, 1 = 40 MHz, 2 = 80 MHz. |
| `MtkDataRate` | 0 | 0 lets the firmware pick the rate; 18 to 25 forces a fixed legacy rate (debugging). |

## Diagnostics

`ioreg -r -c MtkWlan -l -w0` holds every counter the driver keeps: `MtkState`, `MtkScan` (the cached networks with channel, RSSI and security), `MtkW1Join` (join results), `MtkW2Sec` (handshake messages, MIC failures, replays, drops), `MtkCm` (what the connection manager decided), `MtkW2Cmd` (firmware command timeouts), and the data path and block-ack sets. It never contains a key or passphrase. `mtkwifi status --json` gives the same picture from user space. `tools/wifi.sh` runs the measurements the table above came from. `tools/mtk-report.sh` packs all of it for a bug report (see the section below). The `docs/HOW-IT-WORKS.md` property list explains each one.

## Other MediaTek cards: what it would take, and how to help

This driver has been run on one card (MT7925, PCI `14c3:7925`, subsystem `17aa:e0ff`, in one laptop). From 0.11.1 it
also matches `14c3:0717`, the AMD RZ717 card on AM5 desktop boards; a first test is under way. The chip check in
`MtkWlan.cpp` refuses anything whose chip id is not `0x7925`, on purpose: the MT7921 and MT7922 (RZ608, RZ616) need
their own firmware files (`WIFI_RAM_CODE_MT7961_1.bin` and the MT7961 patch instead of the MT7925 pair), a few MCU
commands in the older non-UNI form, and their own TX power tables, and a kext that loads on a card it cannot bring up
would cost you the Wi-Fi SimpleMtkWlan gives you today. The port is tracked in
[issue #2](https://github.com/watchthelight-labs/mt7925-hackintosh-wifi/issues/2) with the file-by-file plan; it needs
a card to test on, so if you have an MT7921 or MT7922 and an OpenCore machine, say so there. If you try the kext on a
different MT7925 board, I want the result either way:

1. Install the kext as above and reboot. Wait a minute after the desktop appears.
2. Run `bash tools/mtk-report.sh` (no sudo). It writes `~/Desktop/mtk-report-<date>.tar.gz` with: the PCI ids of the
   MediaTek device, loaded kexts, every `ioreg` property of `MtkWlan` (and `MtkWlanNative` if installed), interfaces and
   routes, `mtkwifi status --json` and `mtkwifi list`, the Wi-Fi system profile, the kernel log lines of the driver and
   the IO80211 family, the Wi-Fi daemons' log, and the last three kernel panic reports if any. MAC addresses and SSIDs
   are replaced by placeholders (`--no-redact` keeps them; `--minutes N` widens the log window, default 30). Nothing in
   it is a key or passphrase.
3. Open an issue at github.com/watchthelight-labs/mt7925-hackintosh-wifi/issues with the archive attached and three lines: the card
   (PCI ids), the macOS build (`sw_vers`), and what happened (did the kext load, did `mtkwifi list` show networks, did a
   join get a lease). If the machine panicked, say what was on screen; the report includes the panic file.

What the properties mean is in `docs/HOW-IT-WORKS.md`. The usual first failures on a new card are `MtkFwState` (firmware
not found or rejected: the file names in `Info.plist` and `gen_firmware.py` are for the MT7925) and `MtkW2Cmd` timeouts
(the MCU did not answer: register map or DMA layout).

## Building

On macOS, with the Xcode Command Line Tools:

```
tools/build-mac.sh      # out/MtkWlan.kext, prints the sha256
bash tests/run.sh       # host tests for the WPA2 code
```

On Linux (Ubuntu 24.04, also fine in WSL), no Mac needed:

```
sudo bash tools/build-toolchain.sh   # clang, Apple ld64 (cctools-port), libdispatch, MacKernelSDK
make                                 # out/MtkWlan.kext
```

The firmware in `firmware/` is checked against `firmware/SHA256SUMS` and embedded in the binary. The macOS build is reproducible (the same sources with the same Apple clang 17.0.0, clang-1700.0.13.5, give the same binary). The 0.10.0 release zip holds the binary from the tested boot (sha256 `00f68f99d8e6ab2ea4cc3ba36f7b44a7d7bcd9fc32440ecc80674a0b62de3394`). It was built a few minutes before the last 0.10.0 edit (`MtkNet.cpp` publishing the `MtkFrontEndActive` registry property), so a clean build of this tag gives `a1645f7e39749d13c13adb302518cf0e63bbc29e7fe60dc198c08e587689bb90` instead; the string tables differ only by that property name. The next boot uses the clean build.

## How it works

The driver claims the card from its firmware, sets up the WFDMA rings, downloads the patch and RAM firmware, and from then on talks to the MCU with the same UNI commands Linux mt76 sends. A 1 ms timer on one work loop drives everything: ring drains, the scan and join state machines, the WPA2 supplicant, the connection manager and block ack. The Ethernet front end translates between Ethernet and 802.11 data frames and opens its port only after the keys are installed. The long version, with the threading rules and why RX is polled: [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

## Layout

| Path | What |
|---|---|
| `src/MtkWlan.cpp`, `MtkRegs.cpp` | PCI attach, register remap, ownership handshake |
| `src/MtkFw.cpp`, `MtkDma.cpp` | rings, MSI, firmware download |
| `src/MtkScan.cpp` | MCU commands, hardware init, scan machine, BSS cache, RX dispatch |
| `src/MtkJoin.cpp` | join machine, connection manager, BSS/STA records |
| `src/MtkSec.cpp`, `MtkSupp.c`, `MtkCrypto.c` | WPA2-PSK glue, supplicant, crypto primitives |
| `src/MtkNet.cpp`, `MtkBa.cpp`, `MtkPower.cpp` | data path, block ack, TX power tables |
| `src/MtkWlanEth.cpp`, `MtkCoreApi.h` | Ethernet front end and the C ops table between it and the core |
| `src/MtkUserClient.cpp`, `MtkUserApi.h` | the IOUserClient the tool talks to |
| `tools/mtkwifi/` | the command line tool and its autojoin daemon |
| `tests/` | host tests and the Python oracle for the WPA2 code |

## Credits

- Linux **mt76** (MediaTek, Felix Fietkau and contributors, ISC): nearly everything about how the hardware works.
- **itlmt7925** by thanhnndev and contributors (Clear BSD): the join sequence, the post-firmware setup and the WPA2 crypto primitives started there, and the scan-cache eviction fix (2.4 GHz networks lost in crowded places) came from its 4257353.
- **itlmt7922-wifi** by hbyonly1 ([hbyonly1/itlmt7922-wifi](https://github.com/hbyonly1/itlmt7922-wifi), Clear BSD): an MT7922/RZ616 port of itlmt7925 for the older Connac2 firmware family; its porting notes are where MT7922 testers should start.
- **SimpleMtkWlan** fork by eiyuumiru ([eiyuumiru/SimpleMtkWlan](https://github.com/eiyuumiru/SimpleMtkWlan), GPL-2.0, read only): on an RZ616, beacon-loss events (0x13) dropped on the floor left the link up after the AP was gone, and deep sleep switched on before the association finished made joins time out. Both are on our list to check against the 38-minute stall.
- **itlwm** (OpenIntelWireless): the idea of a Wi-Fi kext that presents itself as Ethernet.
- **RealtekRTL8111** (Mieze): the usual shape of an IOEthernetController kext.
- hostap's **wpa_supplicant**: the handshake rules, read, not copied.
- MediaTek firmware from linux-firmware, unmodified.
- Toolchain: acidanthera **MacKernelSDK**, tpoechtrager **cctools-port**, swiftlang **libdispatch**.

`NOTICE.md` says which file comes from where.

## Licence

ISC, except `src/MtkCrypto.c` / `.h` (Clear BSD, from itlmt7925), `src/MtkSupp.c` / `.h` (ISC and Clear BSD) and the parts of `MtkScan.cpp` and `MtkJoin.cpp` that follow itlmt7925 (Clear BSD). The firmware is MediaTek's under `LICENSES/LICENCE.mediatek`. `LICENSE`, `NOTICE.md` and `LICENSES/` must travel with any built copy of the kext.

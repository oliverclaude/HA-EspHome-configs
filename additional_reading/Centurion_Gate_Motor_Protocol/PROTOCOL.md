# Centurion MyCentsys Pro BLE protocol

This document describes the Bluetooth Low Energy protocol used by the
**MyCentsys Pro** phone app to talk to Centurion **Vantage / V-Series SMART**
gate controllers (product code `VX_CORE` = 26 and related boards). It is meant
for Home Assistant / ESPHome integrators and anyone reverse-engineering the
same GATT surface.

Values that identify a specific install (MAC address, enrolled phone number,
controller serial, cipher-key bytes taken from a live advert) are **never**
shown here. Use placeholders and your own captured advertising data.

---

## 1. Overview

| Role | Device |
|------|--------|
| BLE Central / GATT client | Phone (MyCentsys Pro) or your ESP32 / Pi bridge |
| BLE Peripheral / GATT server | Gate controller (Centurion SMART comms board) |

There is **no wake packet**. The controller advertises on its own. The app only
scans for the Centurion serial service (and Nordic DFU). If you cannot see the
gate, it is not advertising, another central already holds the link, or your
radio cannot hear the advert.

**Only one BLE central can use the gate at a time.** While a session is up, the
advertising `Connectable` byte becomes `USER_CONNECTED` (0) and a second client
cannot usefully attach. Close the phone app (or drop your bridge’s link) before
switching tools.

Open / close / pedestrian / holiday lock are **not** separate GATT opcodes.
Motion and most UI buttons are one **Trigger** command whose payload carries a
`TriggerInd` looked up from the controller’s Triggers database.

---

## 2. GATT UUIDs

| Role | UUID |
|------|------|
| SerialCommunication service | `57b53956-da15-4005-8a1f-fcacde09af51` |
| Server → Client (notify) | `57b5db3b-da15-4005-8a1f-fcacde09af51` |
| Client → Server (write **without** response) | `57b5f92c-da15-4005-8a1f-fcacde09af51` |
| Nordic Secure DFU (bootloader mode) | `0000fe59-0000-1000-8000-00805f9b34fb` |

Writes use `withResponse: false`. Replies, unsolicited status (URCs), RAM
objects and file chunks all arrive as notifications on the notify characteristic.

---

## 3. Advertising / manufacturer data

The app reads **Manufacturer Specific Data** only. The Bluetooth company ID is
**not** checked in app code; a live scan should still record it from AD type
`0xFF`.

### Length 18 — Android “ConfigApp” layout

| Offset | Field |
|-------:|-------|
| 0 | Subtype / unused by the serial parse |
| 1–4 | `SerialNumber1` (little-endian int32) |
| 5–8 | `SerialNumber2` (little-endian int32) |
| 9 | Skipped |
| 10–13 | **`DeviceMacAddress` (4 bytes)** — used as the auth XOR key |
| 14 | `Connectable` (`CONNECT_STATE`) |
| 15 | Firmware major |
| 16 | Firmware minor |
| 17 | `ProductCode` (`INFRAVERSION_PRODUCTCODE`) |

### Length 20 — iOS “ConfigApp” layout

Same fields, shifted: MAC key at **12–15**, Connectable at **16**, firmware at
**17–18**, ProductCode at **19**.

Remote / MyCentsys Remote adverts use lengths **36 / 38** and follow a separate
path; they are not decoded by `ParseCentSysAdvertisingData`.

### `CONNECT_STATE` (advert byte)

| Value | Meaning |
|------:|---------|
| 0 | `USER_CONNECTED` |
| 1 | `FILE_SYSTEM_BUSY` |
| 2 | `FACTORY_TEST` |
| 3 | `DFU_MODE` |
| 4 | `REMOTE` |
| 255 | `OK` — free to connect |

### Product codes (relevant)

| Code | Name |
|-----:|------|
| 25 | `VX_CORE_PLUS` |
| 26 | `VX_CORE` (Vantage) |
| 27 | `VX_BLUETOOTH` (comms board product in version info) |
| 38 | `VX_ESP` |

Local name is the peripheral name, or `"SMART Operator"` if empty. The friendly
name shown in the app can be changed with `ChangeAdvertisingName`; it is not
the serial number.

**Example (made-up):** if `DeviceMacAddress` in the advert is
`AA BB CC DD`, that 4-byte value is the auth key. On many units it equals the
last four bytes of the BLE device address; do not hard-code it — read it from
the advert (or derive it from the address you connect to).

---

## 4. TransferProtocol framing

Framed over the two serial characteristics. **There is no CRC on the BLE
frame.** CRC32 is used for filesystem / firmware file integrity after bulk
transfer, not per-packet framing.

### Phone → gate (10-byte header, big-endian multi-byte fields)

```
offset  size  field
0       1     TransferType
1       4     CommandLogicalAddress  (BE uint32) — opcode OR RAM/file address
5       4     PayloadSizeBytes       (BE uint32)
9       1     IsLastChunk            (0/1; meaningful for WRITE chunks)
10…     N     payload (COMMAND / WRITE only)
```

| TransferType | Name | Typical send size |
|-------------:|------|-------------------|
| 0 | `APPIPPMM_TRANSFER_READ_REQUEST` | header only (10) |
| 1 | `APPIPPMM_TRANSFER_WRITE_REQUEST` | 10 + data |
| 2 | `APPIPPMM_TRANSFER_COMMAND_RESPONSE` | 10 + payload |
| 3 | `APPIPPMM_TRANSFER_COMMAND_NO_RESPONSE` | defined; rarely used on the send path |

Payload structs (Trigger, encrypted phone bytes, …) are marshalled in
**little-endian** host layout (Android / typical ESP32).

### Gate → phone (2-byte header)

```
offset  size  field
0       1     TransferStatus
1       1     PacketPayloadSize
2…      N     payload (URC / RAM object / …)
```

| TransferStatus | Name |
|---------------:|------|
| 0 | `COMPLETE` (ACK / URC / RAM data) |
| 1 | `READ_REPLY` |
| 2 | `ACK` |
| 3 | `NACK` |
| 4 | `TIMEOUT` |

URC detection: when idle or awaiting a command ACK, if `PacketPayloadSize >= 2`
and the first payload byte (`packet[2]`) is a known `BUFFER_STATE` URC code,
treat the notify as a URC. Large writes are sliced to an MTU-ish max (often
18–100). Commands typically time out after about 7 seconds with no reply.

---

## 5. High-value command opcodes (V-Series / Vantage)

`BLECommands` enum values are mapped to wire `CommandLogicalAddress` integers
(big-endian on the wire). Useful ones:

| Purpose | Wire BE hex |
|---------|-------------|
| `AuthMobilePhoneA` | `08 00 00 04` |
| `AuthMobilePhoneB` | `08 00 00 05` |
| `AuthMobilePhoneFromLCDSingleSession` | `08 00 00 06` |
| `AuthMobilePhoneFromLCDPermanentSession` | `08 00 00 07` |
| `AuthBootload` | `08 00 00 0F` |
| `KeepAlive` | `08 00 00 20` |
| `EnterStatusOverviewMode` | `08 00 00 22` |
| `ExitStatusOverviewMode` | `08 00 00 23` |
| `Trigger` | `08 00 00 24` |
| `EnableURCBlock` | `08 00 00 2C` |
| `DisableURCBlock` | `08 00 00 2D` |
| `EnterStatusOverviewModeV2` | `08 00 00 35` |
| `ExitStatusOverviewModeV2` | `08 00 00 36` |
| `EnterStatusIoMode` | `08 00 00 37` |
| `VxConfig` | `08 00 00 0D` |
| `SetTime` | `08 00 00 19` |
| `ChangeBLEConnectionParameters` | `0F FF FF FC` |
| `ChangeAdvertisingName` | `0F FF FF FB` |
| `Disconnect` | `0F FF FF FA` |

`RequestRamObject` is **not** a command packet: it uses `READ_REQUEST` with a
logical address (see §8).

---

## 6. Authentication

### EncryptPhoneNumber

Inputs:

- Enrolled phone number as UTF-8 bytes, **exactly** as the app stores / sends it
  (typically E.164 with a leading `+` and country code).
- 4-byte key = `DeviceMacAddress` from the advert (example: `AA BB CC DD`).

For each phone byte at index `i`:

1. `b = phone[i] XOR key[i % 4]`
2. Nibble-swap: `out[i] = ((b << 4) | (b >> 4)) & 0xFF`

`EncryptionKeyLength` must be exactly 4.

### Registered-phone flow (`AuthMobilePhoneA`)

1. Scan and parse manufacturer data; keep the 4-byte key.
2. GATT connect; enable notify; resolve the write characteristic.
3. Send `ChangeBLEConnectionParameters`. On Android the app uses roughly
   min=10, max=12, latency=0, supervision=4000 (units as in
   `BleConnectionParameters.serializeParameters`).
4. On ACK, send `AuthMobilePhoneA` (`08 00 00 04`) with the encrypted phone
   payload (`TransferType` = command-with-response).
5. ACK → session up. NACK → number not enrolled on the controller
   (`CONNECT_NUMBER_NOT_IN_D5`).

### LCD / first-time pairing (`AuthMobilePhoneB`, …)

Used when the phone is not yet enrolled: connect, connection parameters,
`AuthMobilePhoneB` (no phone payload), then LCD button prompts and either
single-session or permanent-session auth. App PIN / biometrics only gate the
**UI**; they are not a second BLE crypto layer.

### KeepAlive

While connected, periodically send `KeepAlive` (`08 00 00 20`, empty payload).
An 8-second interval works in practice on Vantage.

---

## 7. Status stream (overview URC)

After auth, enable unsolicited status:

1. Optionally poke `EnableURCBlock` / info reads (see recipe below).
2. Send **`DisableURCBlock`** (`08 00 00 2D`).
3. Send **`EnterStatusOverviewMode`** (`08 00 00 22`).

On a live **Vantage** dual-swing operator, classic overview mode (not V2) is
what produced a working stream. `EnterStatusOverviewModeV2` exists in the app
enum; treat it as **unverified** for Vantage until you confirm it on your
hardware.

### Notify shape

```
00 42 a2 40  +  64-byte body
│  │  │  └─ body length byte (0x40 = 64) as seen live
│  │  └──── BUFFER_STATE / URC code for overview (0xA2)
│  └─────── PacketPayloadSize (0x42)
└────────── TransferStatus COMPLETE (0x00)
```

So the GATT value is a 2-byte TransferProtocol header, then URC framing, then a
**64-byte** `INFRATP_OVERVIEW_VX`-style body. An older 52-byte decode from the
managed structs does **not** match live Vantage packets; use the offsets below.

Packets arrive about 1–2 per second during motion and are change-driven when
idle. Full open or close on a dual swing is on the order of ~30 seconds.

### Overview body field offsets (little-endian, Pack=1)

| Off | Type | Field |
|----:|------|-------|
| 0 | u8 | `version` (seen as 1) |
| 1 | u8 | `temperature` |
| 2 | u16 | Battery voltage in units of 10 mV (e.g. `0x053C` → 13.40 V) |
| 4 | u32 | `notificationFlags1` |
| 8 | u32 | `notificationFlags2` |
| 12 | u32 | `conditionFlags` |
| 16 | u32 | `secondsRemaining` |
| 20 | u8 | Master leaf status |
| 21 | u8 | pad |
| 22 | s16 | Master position (mm-scale; open positive, closed near 0 / slightly negative) |
| 24 | i32 | Master encoder counts |
| 28 | u8 | Slave leaf status |
| 29 | u8 | pad |
| 30 | s16 | Slave position |
| 32 | i32 | Slave encoder counts |
| 36 | u8 | **Combined gate status** |
| 37 | u8 | IRBO status |
| 38 | u8 | IRBC status |
| 39 | u8 | Gate lock status |
| 40 | u16 | Timer flags |
| 42 | u8 | Power status (`0` = normal / mains on, `1` = low seen with mains fail) |
| 43 | u8 | Feedback handle |
| 44 | u8 | Trigger handle / counter (increments on each trigger) |
| 45 | u8 | `isDoubleGate` (`0xFF` = true on dual swing) |
| 46 | u16 | Feedback index |
| 48 | u16 | **Last TriggerInd** |
| 50 | u16 | Input voltage (often 0 when not populated) |
| 52–63 | — | Padding / unused (stayed zero in testing) |

### Combined / leaf gate status enum

| Value | Meaning |
|------:|---------|
| 0 | OPEN |
| 1 | CLOSED |
| 2 | PARTLY_OPEN |
| 3 | PARTLY_CLOSED |
| 4 | OPENING |
| 5 | CLOSING |

### Useful `conditionFlags` bits (live-confirmed where noted)

| Bit | Meaning |
|----:|---------|
| 0 | Holiday lock active (confirmed) |
| 5 | FRX (free exit) latch |
| 9 | IRB alarms armed |
| 11 | “Gate is lost” flag (app may still show Good when notif LOST is clear) |
| 13 | Keep open |

Holiday lock also sets notification bit 31. Mains failure sets notification
bit 0 and drives `powerStatus` toward low.

---

## 8. Triggers

Wire command: **`08 00 00 24`** with a 4-byte little-endian payload:

```
struct INFRATP_TRIGGER {
  uint16_t flags;       // normally 0
  uint16_t triggerInd;  // Triggers DB index
};
```

Example frame (flags=0, TriggerInd=34 / `0x0022`):

```
02 08 00 00 24 00 00 00 04 00   00 00 22 00
│  └─ cmd addr BE               └─ LE payload
└─ TransferType COMMAND_RESPONSE
```

`TriggerInd` comes from the controller’s **Triggers** table (synced / default
DB in the app), keyed by `FkTriggerFunction`. Defaults that matched a live
Vantage dual-swing:

| UI / function | TriggerInd | Live status |
|---------------|-----------:|-------------|
| TRG (Gate0 open/close toggle) | **34** | **Verified** — toggles open/close |
| PED (Gate1 pedestrian) | **35** | **Verified** — from CLOSED opens master only then auto-closes; from OPEN may fully close both leaves (config-dependent) |
| Holiday lock on | **2** | **Verified** — sets condition bit 0 + notif bit 31; no motion |
| Holiday lock off | **3** | **Verified** |
| Holiday lock toggle | 1 | Untested |
| Keep open on / off | 5 / 6 | Untested (app code) |
| FRX on / off | 40 / 41 | Untested (app code) |
| Gate lock (electric lock) | 20 | Untested (app code) |

There is no dedicated BLE “stop” opcode. On Centurion, sending **TRG while the
gate is moving** is the usual way to stop; confirm that on your install before
relying on it.

**Do not send Trigger until auth and overview URCs are proven.**

---

## 9. RAM reads (version / controller info)

`RequestRamObject(logicalAddress, objectSize)`:

1. Look up the logical address → uint32.
2. Send **READ_REQUEST** header: that address (BE), `PayloadSizeBytes` =
   objectSize (BE), no payload.
3. Expect `COMPLETE` + `objectSize` bytes of object data in one notify
   (`00 <size> <bytes…>` for small objects).

### Key V-Series logical addresses

| Name | Wire BE hex | Typical size |
|------|-------------|-------------:|
| `CP_BLE_CONNECTION_STATUS` | `00 90 02 00` | — |
| `CP_D5_INFO_FOR_APP` | `00 90 03 00` | — |
| `CP_VERSION_INFO` | `00 90 04 00` | 38 |
| `CP_GATE_READY_STATUS` | `00 90 05 50` | — |
| `CP_CONTROLLER_INFO` | `00 90 0A 00` | 40 |
| `CP_STATS` | `00 90 0B 00` | — |
| `CP_BLE_PERIPHERAL_INFO` | `00 90 18 00` | — |
| `CP_VXCONFIG_FLASH` | `08 00 08 00` | — |

### `CP_VERSION_INFO` (38 bytes, Pack=1 sketch)

| Off | Field |
|----:|-------|
| 0 | u32 `packageVersion` |
| 4–7 | Bluetooth / comms firmware as four version bytes |
| 8 | BT product code (e.g. 27 = `VX_BLUETOOTH`) |
| 9 | BT board hardware |
| 10–20 | PSU / MC blocks (often zero) |
| 21–24 | Core firmware four version bytes |
| 25 | Core product code (26 = `VX_CORE` on Vantage) |
| 26 | Main board hardware |
| 27–33 | Expansion / charger fields |

### `CP_CONTROLLER_INFO` (40 bytes)

Layout used by V-Series: `u32 flags` + three `u32` core serial words + three
PSU serial words + three photon serial words. The app formats the core serial
as three `%08X` groups concatenated (24 hex digits). **Do not publish your
serial.** PSU / photon words are often zero on a plain Vantage.

---

## 10. Health / fault categories

The app builds a 64-bit notification word
`(notificationFlags2 << 32) | notificationFlags1` and maps set bits to health
categories. Any set bit in a category marks that category as in error;
otherwise “Ok”.

Approximate bit → category mapping from the managed assemblies:

| Category | Example notification indices |
|----------|------------------------------|
| Power supply | 0 MAINS, 1 MAINS_LOW, 2 PSU_HARDWARE_FAULT, 10 PSU_UNHEALTHY_COMMS |
| Battery | 3 BATTERY, 4 REPLACE_BATTERY, 5 NO_BATTERIES |
| Controller | 6 BATTERY_USAGE, 7 FILE_CONFIG_BUSY, 8 INVALID_FILES, 9 ENCODER_WHILE_RUNNING, 11 IO_CONFIG_ERROR, 12–14 IO overcurrent, 23 H_BRIDGE_TEMP, 26 CPU_TEMP, 27 TEMP_WARNING, 32/33 IRBO/IRBC_TEST_FAIL |
| General | 22 AUX_12V_FAILURE, 30 EMERGENCY_STOP, 31 HOLIDAY_LOCK, 36 BEAMS_OPEN, 37 PHOTONS_BLE_DISCONNECTED, 38 BLE_PHOTONS_BATTERY_LOW |
| Master motor | 15 MASTER_COLLISION, 17 MAX_COLLISION, 18 MASTER_GATE_STALLED, 20 LOST, 21 LIMITS_NOT_SET, 24 MASTER_BRIDGE_OR_FUSE, 28 MASTER_MOTOR_DISCONNECTED, 34/35 IRBO/IRBC_ALARM |
| Slave motor | 16 SLAVE_COLLISION, 19 SLAVE_GATE_STALLED, 25 SLAVE_BRIDGE_OR_FUSE, 29 SLAVE_MOTOR_DISCONNECTED |

### Swing-gate ignore list

For swing products the app **suppresses** some bits from the health UI so they
never show as faults: LOST, LIMITS_NOT_SET, HOLIDAY_LOCK, IRBO/IRBC_ALARM,
KEEP_OPEN, FRX. Mirror that if you want Home Assistant “health” sensors to match
the phone app.

---

## 11. Practical session recipe

Order that worked on a live Vantage dual-swing (all steps expecting COMPLETE):

1. **Connect** GATT; subscribe to notify; find the write-without-response char.
2. **`ChangeBLEConnectionParameters`**.
3. **`AuthMobilePhoneA`** with `EncryptPhoneNumber(...)`.
4. **`KeepAlive`** (and keep sending it on an interval).
5. Optional: **`EnableURCBlock`**, then **RAM reads** of `CP_VERSION_INFO` and
   `CP_CONTROLLER_INFO`.
6. **`DisableURCBlock`** (`08 00 00 2D`).
7. **`EnterStatusOverviewMode`** (`08 00 00 22`) — wait until overview URCs
   (`… a2 40` + 64-byte body) look sane (battery, combined status, positions).
8. **Only then** send **Trigger** commands.
9. To free the radio for the phone app: disconnect (or stop auto-connect) so
   `Connectable` returns to `OK` (255).

LCD pairing / DFU / file transfer / VxConfig flash writes are out of scope for
a minimal open/close bridge; see the app assemblies if you need them.

---

## 12. Caveats

- **One central only.** Kill the phone app (or disable your bridge’s BLE link)
  before the other side connects.
- **Battery disconnect** may raise **no** fault while charger / aux voltage is
  still present. Overview battery voltage can still look like a healthy float
  voltage. The controller may only flag `NO_BATTERIES` / `REPLACE_BATTERY`
  after a diagnostic load test or when mains fails.
- **Stop via TRG during motion** is expected Centurion behaviour but was not
  fully verified on every install — confirm before automating it.
- **TriggerInd is DB-backed.** Defaults 34/35/2/3 matched live Vantage testing;
  a controller with a customized Triggers table may differ. Prefer reading the
  synced DB (app path) or probing carefully.
- Overview **V2** and IO-view modes exist in the enum; Vantage live success used
  classic `EnterStatusOverviewMode` after `DisableURCBlock`.
- Advertising with missing / wrong-length manufacturer data is dropped by the
  app parse (needs length 18 or 20 for ConfigApp, unless DFU name special-cases).

---

## Sources

Reverse-engineered from the **MyCentsys Pro** Android APK managed assemblies
(around **1.5.0.x**; native AOT builds in the same series expose the same
GATT / TransferProtocol surface), principally:

- `TransferProtocol` / `CentsysBleManager` (Infrastructure)
- `BLECommands`, V-Series `BLEObjects` / logical addresses (SmartLibrary)
- `TransferProtocolTypes` (Domain)

Validated with live BLE against a **Centurion Vantage dual-swing** SMART
operator (auth, keepalive, overview URCs, TRG / PED / holiday-lock triggers,
version and controller info reads). Site-specific identifiers are omitted on
purpose.

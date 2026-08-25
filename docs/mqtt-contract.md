# TriageBox station — MQTT integration handoff

Everything the broker/backend side needs in order to accept traffic from a
TriageBox receiving station. Authoritative source for the payload shapes is
`triagebox-backend/src/common/mqtt-payload.ts` and the "MQTT topics" /
"Canonical vital JSON" sections of `TriageBox-LCD/AGENTS.md`; this file is the
station's side of that contract and does not invent anything.

## What the station is

An ESP32 + W5500 gateway on wired Ethernet. It receives binary packets from
STM32 sensor nodes over 433 MHz LoRa and republishes each one as JSON. It is
the only MQTT client in the system — the sensor nodes have no IP stack and
never touch the broker.

## Connection

Plain TCP MQTT 3.1.1, no TLS today. The station takes a single URI of the form
`mqtt://host:1883`, an optional username and an optional password, all three
compiled in as build-time configuration. Clean session, default keepalive
(120 s), QoS 1 on everything that matters.

The MQTT client id is the station id, e.g. `st-01`. That is deliberate — one
station, one id, one connection — but it means two boards flashed with the same
`station_id` will fight over the connection and repeatedly evict each other.
Each physical station needs its own id.

## One number per board

**`CONFIG_TB_STATION_NUM` is the only value that must differ between two
physical stations.** Everything that collides silently is derived from it:

| Derived from `TB_STATION_NUM = N` | Value |
| --- | --- |
| LoRa radio address (`lora_poll_t.station_id`) | `N` |
| Static IP | `<TB_ETH_SUBNET>.(10 + N)` → `.11`, `.12`, … |
| MQTT node id offset | `(N - 1) * 20` |

This replaced three separate settings — `TB_STATION_ADDR`, `TB_ETH_IP`, and an
implicit assumption that node ids started at 1 everywhere. All three had the same
failure mode: a second board flashed from this repo unchanged would collide, and
none of the three collisions produces an error message. An IP conflict shows up as
ARP thrash and an MQTT link that connects and drops; a shared radio address shows
up as nodes answering polls that were not for them; a shared node id range shows up
as a perfect radio link whose packets are all discarded at the backend.

`TB_STATION_ID` (the MQTT topic segment) is still its own string, because it must
match whatever the backend registered and getting it wrong is **loud** — the
backend logs an unknown station and the dashboard lists it as awaiting adoption.
Keep it in step by convention (`num 3` → `st-03`).

The IP is derived rather than pushed from the dashboard for a reason worth
recording: a station must already be able to reach the broker in order to receive
configuration, so an address cannot be handed out over the link it is needed for.
DHCP would solve the bootstrap, but there is no DHCP server in this deployment —
a direct cable runs none, an unmanaged field switch runs none, and Windows desktop
cannot supply one (the DHCP server role is Windows Server only; Internet
Connection Sharing forces the fixed `192.168.137.x` subnet and needs an internet
connection to share).

## Topics

`{station_id}` and `{node_id}` are topic segments, never JSON fields.

| Topic | Direction | QoS | Retain | Notes |
| --- | --- | --- | --- | --- |
| `triagebox/{station_id}/{node_id}/vital` | station → broker | 1 | no | one measurement set from one node |
| `triagebox/{station_id}/{node_id}/status` | station → broker | 1 | **yes** | node liveness, RSSI/SNR |
| `triagebox/{station_id}/status` | station → broker | 1 | **yes** | station liveness, also the LWT |
| `triagebox/{station_id}/announce` | station → broker | 1 | **yes** | adoption candidate, published on connect |

Demo seed ids are `st-01`/`st-02` and `node-01`…`node-10`. The backend drops
messages from unregistered ids on the first three topics — but `announce` is
exempt, which is how an unregistered station becomes visible instead of vanishing.
See "Adoption" below.

`{node_id}` is **not** the raw radio address. It is `radio_address +
(TB_STATION_NUM - 1) * 20`, formatted `node-%02u`: station 1 publishes radio 1..20
as `node-01`..`node-20`, station 2 publishes its own radio 1..20 as
`node-21`..`node-40`. Radio addresses restart at 1 on every station, but the
backend binds a node id to exactly one station and silently drops a vital arriving
under any other — so without the offset a second station's packets would all be
discarded with a perfect radio link. The offset must match the `nodeIdBase` used
when the station was adopted; nothing can detect a mismatch.

The station subscribes to nothing. It is publish-only.

## Adoption

`triagebox/{station_id}/announce`, retained, published on every
`MQTT_EVENT_CONNECTED` right after the ONLINE status:

```json
{
  "station_id": "st-01",
  "mac": "24:6F:28:AA:BB:CC",
  "ip": "192.168.50.11",
  "firmware": "1.0.0",
  "node_count": 20
}
```

**This is the only topic the backend accepts from an unregistered station**, and
it does not create one. The backend records a `PendingStation` keyed on the MAC,
emits `station.pending`, and the candidate appears in the dashboard's Perangkat
page under "Menunggu adopsi". An operator adopts it, which creates the station
plus `node-NN` for `node_count` nodes in one transaction. So "MQTT never creates a
device" still holds — the door is just visible now.

`mac` is the identity because it is the only field the station cannot change from
its own configuration; `station_id`, `ip` and `node_count` are suggestions the
operator may override. Retained so a dashboard opened an hour after boot still
sees the candidate. An announce whose `station_id` is already registered is
ignored rather than treated as an error, which is what makes republishing on every
reconnect safe.

## Station liveness

`triagebox/{station_id}/status` carries JSON: `{"status":"ONLINE"}` and
`{"status":"OFFLINE"}`. It is registered as the MQTT last-will, so the **broker**
publishes `{"status":"OFFLINE"}` on its behalf when the station drops off — power
loss, yanked cable, crash — without the station having to do anything. On every
successful connect the station immediately publishes a retained
`{"status":"ONLINE"}` over the top of it.

An earlier revision of this document said this topic carried the bare strings
`ONLINE`/`OFFLINE`, and the firmware did that until 2026-08-18. It was corrected
to JSON to match the frozen `api-contract.md`. If anything on the backend was
built against the bare-string version, it needs the same correction.

Both are retained on the same topic, so a dashboard that subscribes an hour
later still gets the station's current state rather than nothing. Anything
consuming this should treat the retained value as current state, not as an
event, and should expect a duplicate `ONLINE` after each reconnect.

## Vital payload

Canonical snake_case. This is what the firmware emits today, byte for byte —
`main/tb_vital_json.c` is the only thing that builds it, and
`tools/test_vital_json.c` is an executable copy of the rules below:

```json
{
  "victim_rfid": "04A2B3",
  "hr": 118,
  "spo2": 91,
  "rr": 28,
  "battery": 76,
  "priority": "RED",
  "confidence": 0.87,
  "packet_counter": 1421,
  "device_status": 23,
  "ts": 1755500000
}
```

`priority` is one of `RED`, `YELLOW`, `GREEN`, `BLACK` as a string. The numeric
form (`0=BLACK, 1=RED, 2=YELLOW, 3=GREEN`) exists only inside the binary LoRa
packet and must never appear in JSON. `IMMEDIATE`/`DELAYED`/`MINOR`/`EXPECTANT`
are not wire values anywhere.

Keys are omitted rather than zeroed when a value is unknown: a node that cannot
read its battery sends no `battery`. This matters because `0` is a legitimate
priority (BLACK) and `0` is a legitimate battery level, so a substituted zero
would be indistinguishable from real data. The backend agrees — as of 2026-08-21
`victim_rfid`, `hr`, `spo2`, `rr`, `battery` and `confidence` are optional in its
Zod schema and nullable in its database, and it has a regression check
(`npm run check:station`) holding six of this firmware's real outputs. Consumers
must handle absent keys.

**`priority` is the exception and is always present.** It is the one field the
backend still requires, because it drives the triage board, the KPIs and the
alerts — a reading without it has nothing to say. So when the ESP32 has not scored
yet (`LORA_VITAL_PRIORITY_NONE`, `0xFF`), the station does **not** publish the
vital at all rather than publishing one the backend would reject. `lora_vital.h`
already required omitting `priority` and `confidence` for `0xFF`; withholding the
whole packet is the rule that follows from it.

Withholding the vital does not affect liveness: node status is published every
cycle regardless, so an unscored node stays ONLINE with fresh RSSI. Without that,
suppressing vitals would strand a live node as OFFLINE after 45 s — see "Node
status payload".

In practice this costs the first few cycles after a node boots. `mon_priority` on
the node is sticky once the ESP32 scores, so a vital between scores carries the
standing triage level rather than dropping back to unscored.

The full list of keys that may be absent is `victim_rfid`, `hr`, `spo2`, `rr`,
`bp_sys`, `bp_dia`, `battery`, `confidence`, `ts`. Always present:
`packet_counter`, `device_status`, `priority`, and `confidence` whenever
`priority` is (they ride together).

Three differences from the earlier revision of this document, all of them
firmware reality rather than proposals:

`bp_sys` and `bp_dia` are **never** sent. Nothing on the node measures blood
pressure; the fields exist in the binary packet with a validity bit that is
permanently clear. On-board BP estimation was tried and abandoned in August 2026.
If a cuff is ever added, the flag flips and the keys appear with no other change.

`ts` is **epoch seconds as a number**, not an ISO-8601 string, and today it is
**omitted entirely**. The station has no RTC, and the bench topology (a direct
Ethernet cable to a PC) has no gateway, no DNS and no reachable NTP server, so
SNTP cannot work yet. The backend must stamp arrival time. A node's reading is at
most one poll cycle old when it is published, so arrival time is accurate to
about 15 s — which is why this is acceptable rather than merely convenient.

Confirmed by the backend owner 2026-08-21, and it is more than acceptable — it is
**preferred**. A `ts` below `1e12` is read as epoch seconds, so an unsynchronised
clock (`millis()`, or a 1970 `time()`) is parsed as 1970 and the reading looks
older than the victim's stored snapshot. The packet is not rejected: the
`VitalReading` row is stored but the victim's priority is left alone, so the
symptom is vital history growing while the triage level freezes. Sending a wrong
clock is worse than sending none. Do not add SNTP for this.

`confidence` is `0..1` with two decimals, converted from the packet's `0..100`.
The contract also accepts `0..100`; the station emits the 0–1 form because that
is what the LCD repo and the simulator already speak.

`packet_counter` and `device_status` are new keys the station now emits. Both are
diagnostics and both are always present. `packet_counter` increments once per
node transmit and wraps at 65535, so a gap in it is a lost reply and not a lost
patient. `device_status` is a sensor-health bitmask: `0x01` ECG, `0x02` MAX30102,
`0x04` microphone, `0x08` RFID reader, `0x10` LoRa — bit set means that sensor
initialised. Both are accepted; `device_status` is stored on the reading, and
`packet_counter` is currently **received but not stored** — the backend keeps its
own incrementing count, so the gap detection this field exists for is not yet
available from the dashboard. Keep sending it; the backend fix is one line.

`reasons` is **not** sent. It was documented as always `[]`; emitting a constant
empty array 20 times per cycle is airtime and bytes for no information, so the
station omits it and the backend defaults it — confirmed, `reasons` defaults to
`[]` in the backend schema. Nothing to change.

`victim_rfid` may be absent when no card was scanned — a null or missing RFID
must not create a victim record. When present it is ASCII hex from a PN532 UID,
at most 20 characters, and the station JSON-escapes it even though hex needs no
escaping: the bytes arrive over a radio, and the packet's length field is
validated while its contents are not.

## Node status payload

```json
{ "status": "ONLINE", "rssi": -72, "snr": 9.5, "battery": 76, "packet_count": 1421 }
```

`{"status":"OFFLINE"}` is published with no other keys: there is no fresh
measurement to report for a node that is not answering, and a last-known RSSI on
an offline node reads as current.

RSSI and SNR are properties of the radio hop and are measured by the station's
receiver, so they belong here and never on the vital.

`battery` is a real reading: the node's SW6106 PMIC has a fuel gauge, read over
the node's internal I²C link. It is absent only when that read has not happened
yet (the first seconds after a node boots) or failed, which is deliberate — the
node reports "no reading" rather than freezing the last good percentage, because a
stuck 80% while a pack drains is worse than a blank cell for one cycle.

Both are retained, because node liveness is state rather than an event.

**`ONLINE` is published every cycle; `OFFLINE` only on the transition.** A node
goes `ONLINE` on its first good reply and `OFFLINE` after three consecutive missed
polls — about 45 s of silence. One miss is normal on a radio link, so the station
does not flap a node's status on a single dropped frame.

This used to be transition-only in both directions, which was cheaper and became
wrong once vitals could be legitimately withheld. `lastSeen` on the backend is
refreshed by a vital **or** a node status, and a node with no vital and no status
for 45 s is marked OFFLINE. An unscored node publishes no vital — so with
transition-only status the backend would mark it OFFLINE and the station would have
no transition left to correct it: `s_online[]` is still true, so nothing would ever
be published again. A live node answering every poll would look dead permanently.
The case is real, not theoretical: a node whose MAX30102 is dead never gets scored,
so it never has a vital worth sending.

The cost is one message per answering node per 15 s — 1.3 msg/s at 20 nodes, which
no broker notices. The benefit beyond correctness is that `rssi`, `snr` and
`packet_count` are now current values rather than "whatever they were when the node
came up", which is what makes a link-quality column worth looking at.

## Cadence and volume

**The LoRa link is polled, and this replaced the free-running design the earlier
revision of this document described.** The station transmits a 5-byte request to
one node at a time and that node answers with its newest reading; nodes never
transmit unless asked. A full cycle covers node addresses 1..N in order and
repeats every 15 s.

So: **one vital per node per 15 s, and no decimation anywhere.** The station
publishes exactly what it polls — there is no buffer, nothing is discarded, and
the MQTT cadence is the LoRa cadence. The earlier claim that "the nodes transmit
far faster than that, so the station decimates" is no longer true of any part of
the system.

Twenty nodes is therefore 20 vitals plus 20 node-status messages per 15 s, so
about 2.7 messages/second at full deployment. Retained storage is a few kilobytes.
Any broker will do.

One consequence worth knowing on the backend side: because polling is strictly
round-robin and nodes never interrupt it, **an urgent result waits up to one full
cycle.** A patient scored RED at the wrong moment is published up to 15 s later.
This was a deliberate choice — an unsolicited transmission can land on top of
another node's reply and destroy both — but it means the backend's alert latency
has a 15 s floor that no amount of backend work can remove.

A missing vital is a lost radio frame, not a lost patient: `packet_counter` will
show the gap and the next cycle carries a fresh reading.

## Answered by the backend owner, 2026-08-21

**`ts` omitted, backend stamps arrival — confirmed, and it is preferred rather
than merely tolerated.** See the vital payload section: a `ts` below `1e12` is read
as epoch seconds, so an unsynchronised clock parses as 1970 and freezes the
victim's triage level while vital history keeps growing. Sending a wrong clock is
worse than sending none. Do not add SNTP for this.

**Missing `reasons` — confirmed**, it defaults to `[]` in the backend schema.

**`packet_counter` and `device_status` — both accepted.** `device_status` is stored
on the reading. `packet_counter` is received but **not yet stored**: the backend
keeps its own incrementing count, so the gap detection this field exists for is not
available from the dashboard yet. Keep sending it; the fix is one line on the
backend.

**Everything else the vital may omit is now optional in the backend schema and
nullable in its database** — `victim_rfid`, `hr`, `spo2`, `rr`, `battery`,
`confidence`. Only `priority` stayed mandatory, which is why this firmware now
withholds unscored vitals instead of sending ones that would be rejected.

## Still open

Anonymous access is fine on an isolated bench segment and is not fine on a
shared network. If credentials are wanted, the station needs a username and
password pair; per-station credentials with an ACL restricting each station to
publishing under its own `triagebox/{station_id}/#` prefix is the sane version,
and prevents one compromised station from forging another's data. Nothing has been
built on either side yet.

TLS is not implemented. If it is required, the station needs the CA certificate
compiled in and the URI scheme becomes `mqtts://`, which is a firmware change
rather than a configuration one — worth knowing early.

## What the station needs back

Broker host and port, whether TLS is required, a username and password if
authentication is enabled. Registration is no longer on this list: the station
announces itself and an operator adopts it from the dashboard, which also creates
the node ids.

Node ids still matter more than they look. The station derives the topic segment as
`node-%02u` from `radio_address + (TB_STATION_NUM - 1) * 20`, so station 2's radio
address 7 publishes under `node-27`. Adoption generates exactly that range from
`node_count` and `nodeIdBase`, so the two agree by construction — but if a station
is registered by hand instead, the offset must match. The station cannot detect a
mismatch and the backend drops unrecognised segments silently.

## Current implementation status

Ethernet, static addressing, the MQTT connection and the retained station LWT all
work on hardware. The SX1278 driver, the poll cycle, the vital JSON and the node
status publish are all written and compile-clean but have **never run against a
real node**. The STM32 side of the poll-response conversion is also written now
(2026-08-18), so both halves exist and neither has met the other: expect the first
real traffic to need a round of debugging on the radio, not on the topics.

**Update, later on 2026-08-18: the radio half now works.** The station logged
`poll: node 1 online (rssi -99 snr 9.8)`, which means a real node's reply arrived
inside its slot, passed CRC and validated. What has *not* run yet is this
document's half — the broker was unreachable at the time, so no vital has ever
been published. The remaining risk is therefore in the topics and payloads, not
in the link.

One consequence for the backend while that is happening: a node can be silent for
reasons that are not a dead node. It answers a poll only if it noticed the poll
early enough to reply inside its slot, and declines otherwise, so an intermittently
`OFFLINE` node may be a node whose own firmware was busy rather than one that has
lost the link. `docs/lora-air-protocol.md` lists the counters that tell those apart.

The topic and payload contract above is stable, so backend work can proceed in
parallel: publish a hand-written vital JSON from the example above to
`triagebox/st-01/node-01/vital` with any MQTT client to exercise the pipeline
end to end.

## Host self-checks

`tools/run_selftests.sh` needs no ESP-IDF and no hardware. It covers the two
things whose failure is invisible on the board:

- `tools/test_vital_json.c` — which JSON keys are emitted and, more importantly,
  which are **not**. A fabricated zero reaching the backend is a fabricated
  patient reading and nothing downstream can tell.
- `tools/lora_budget.c` — airtime, slot timing, duty cycle and link budget from
  the datasheet formulas, asserting the airtime figures quoted in
  `lora-air-protocol.md`. A slot that is too short fails as a timeout
  indistinguishable from a dead node, so those numbers are asserted rather than
  remembered.

The backend has a matching check in the other direction: `npm run check:station`
holds six of this firmware's real payloads and fails if the backend schema ever
stops accepting them.

## Bench broker

`tools/bench-broker.cmd` starts Mosquitto the way the station expects, after
checking the three things that each produce an identical `select() timeout` in the
station's log: no adapter holding 192.168.50.1, no inbound rule for TCP 1883, and
the Windows `mosquitto` service holding the port with its default loopback-only
config. `tools/mosquitto.conf` binds 192.168.50.1 rather than 0.0.0.0 on purpose,
because `allow_anonymous true` on a Wi-Fi interface would be an open broker.

To watch every topic the station produces:

```
"C:\Program Files\mosquitto\mosquitto_sub.exe" -h 192.168.50.1 -t "triagebox/#" -v
```

Subscribe *before* starting the station and the retained
`triagebox/st-01/status` arrives first, which is the fastest confirmation that
the MQTT half is alive independent of whether any node answers.

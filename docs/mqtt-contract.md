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

## Topics

`{station_id}` and `{node_id}` are topic segments, never JSON fields.

| Topic | Direction | QoS | Retain | Notes |
| --- | --- | --- | --- | --- |
| `triagebox/{station_id}/{node_id}/vital` | station → broker | 1 | no | one measurement set from one node |
| `triagebox/{station_id}/{node_id}/status` | station → broker | 1 | no | node liveness, RSSI/SNR |
| `triagebox/{station_id}/status` | station → broker | 1 | **yes** | station liveness, also the LWT |

Demo seed ids are `st-01`/`st-02` and `node-01`…`node-10`. The backend drops
unregistered ids, so whoever owns the database must pre-register both the
station id and every node id before any of this is visible.

The station never subscribes to anything. It is publish-only.

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

Keys are omitted rather than zeroed when a value is unknown: a node that has
not been scored sends no `priority`, and a node that cannot read its battery
sends no `battery`. This matters because `0` is a legitimate priority (BLACK)
and `0` is a legitimate battery level, so a substituted zero would be
indistinguishable from real data. Consumers must handle absent keys. The full
list of keys that may be absent is `victim_rfid`, `hr`, `spo2`, `rr`, `bp_sys`,
`bp_dia`, `battery`, `priority`, `confidence`, `ts`. Only `packet_counter` and
`device_status` are always present.

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

`confidence` is `0..1` with two decimals, converted from the packet's `0..100`.
The contract also accepts `0..100`; the station emits the 0–1 form because that
is what the LCD repo and the simulator already speak.

`packet_counter` and `device_status` are new keys the station now emits. Both are
diagnostics and both are always present. `packet_counter` increments once per
node transmit and wraps at 65535, so a gap in it is a lost reply and not a lost
patient. `device_status` is a sensor-health bitmask: `0x01` ECG, `0x02` MAX30102,
`0x04` microphone, `0x08` RFID reader, `0x10` LoRa — bit set means that sensor
initialised. Store or ignore them; nothing breaks either way.

`reasons` is **not** sent. It was documented as always `[]`; emitting a constant
empty array 20 times per cycle is airtime and bytes for no information, so the
station omits it and the backend should default it. Say so if a missing `reasons`
breaks a query and it goes back in — it costs 14 bytes.

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
receiver, so they belong here and never on the vital. `battery` is absent when
the node has no fuel gauge, which is the case on all current hardware.

Both are retained, because node liveness is state rather than an event.

**Published on transition only, not on a timer.** A node goes `ONLINE` on its
first good reply and `OFFLINE` after three consecutive missed polls — about 45 s
of silence. One miss is normal on a radio link, so flapping a node's status on
every dropped frame would be noise rather than information. The consequence is
that `rssi`, `snr` and `packet_count` on this topic are as old as the last
transition, which on a healthy link means "since the node came up". If the
dashboard wants a live RSSI trend, ask — republishing every cycle is one extra
message per node per 15 s and a two-line firmware change.

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

Twenty nodes is therefore 20 messages per 15 s, plus a node status message only
when something changes. Under 1.5 messages/second at full deployment. Retained
storage is a few hundred bytes. Any broker will do.

One consequence worth knowing on the backend side: because polling is strictly
round-robin and nodes never interrupt it, **an urgent result waits up to one full
cycle.** A patient scored RED at the wrong moment is published up to 15 s later.
This was a deliberate choice — an unsolicited transmission can land on top of
another node's reply and destroy both — but it means the backend's alert latency
has a 15 s floor that no amount of backend work can remove.

A missing vital is a lost radio frame, not a lost patient: `packet_counter` will
show the gap and the next cycle carries a fresh reading.

## Open questions for the broker/backend owner

`ts` is currently omitted and the backend must stamp arrival time — see the vital
payload section. Confirm that is acceptable. If a node-side timestamp is genuinely
needed, the fix is not SNTP on this bench (no gateway, no DNS, no NTP server on a
direct cable) but a route to a time server, which is a network change first.

Anonymous access is fine on an isolated bench segment and is not fine on a
shared network. If credentials are wanted, the station needs a username and
password pair; per-station credentials with an ACL restricting each station to
publishing under its own `triagebox/{station_id}/#` prefix is the sane version,
and prevents one compromised station from forging another's data.

TLS is not implemented. If it is required, the station needs the CA certificate
compiled in and the URI scheme becomes `mqtts://`, which is a firmware change
rather than a configuration one — worth knowing early.

Confirm that a missing `reasons` key and the two extra keys (`packet_counter`,
`device_status`) are tolerated. Both are cheap to change in either direction.

## What the station needs back

Broker host and port, whether TLS is required, a username and password if
authentication is enabled, and confirmation that `st-01` and the node ids are
registered in the database. Those four things are the entire configuration
surface on this end.

Node ids matter more than they look: the station derives the topic segment from
the one-byte radio address as `node-%02u`, so radio address 7 publishes under
`node-07`. Register the zero-padded two-digit form for every node that exists,
and keep the radio address and the registered id in step — the station has no way
to detect a mismatch, and the backend drops unregistered segments silently.

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

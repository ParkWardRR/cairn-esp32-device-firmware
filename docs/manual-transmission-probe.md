# Manual-transmission probe

Gear and clutch are not standard OBD-II values, so the capture format has no field
for them. Gear can be estimated after the fact from RPM over road speed (the
recorded `obd` rows already fall into clean per-gear clusters), but the clutch
pedal, the neutral switch and engine torque only exist if this DME offers them.
The `cairn-mtprobe` builds find out, by asking for everything that might describe a
gearbox and logging the raw answers.

Capture is unchanged. The probe is a second consumer of the OBD link, on the sensing
task, behind the same bus-silence gate as production: a parked car is never asked
anything.

## What it asks

All requests are standard SAE J1979 Mode 01. Nothing is written to the car, no
diagnostic session is opened, and no manufacturer-specific service is used.

1. **Support bitmaps** (`0100`, `0120`, … `01C0`), walked while each bitmap says the
   next exists. This is the ECU's own list. A `claims` line follows it, giving each
   candidate as `Y` (claimed), `n` (not claimed) or `?` (bitmap unread).
2. **Candidates**, three per request alongside RPM, speed and throttle, so every log
   line carries what a shift is read against:
   - `A4` transmission actual gear (J1979-2, so a long shot on a 2014 DME)
   - `62` actual torque %, `61` driver demand torque %, `63` reference torque Nm,
     `64` percent torque data, `8E` friction torque %
   - `49` `4A` `4B` `5A` accelerator pedal, `45` `47` `4C` throttle variants
   - `5E` fuel rate, `5D` injection timing, `5C` oil temperature, `42` module voltage,
     `22` `23` rail pressure, `03` `2C` `2E` `3C` and the counters `1F` `21` `31` `4D`

   Every candidate is asked twice regardless of the bitmaps (the bitmaps are what is
   being tested). One that never answers is then retried every five minutes; one that
   has answered stays in the rotation.
3. **Passive CAN sniff windows** (`cairn-mtprobe-sniff` only): two seconds every 90 s,
   all frames accepted, nothing transmitted. On an F32 the OBD connector is the
   diagnostic CAN behind the gateway and is expected to be silent apart from our own
   conversation, so an empty result is the likely one. It is cheap to confirm and
   expensive to assume.

## Running it

```sh
cd firmware/cairn-v2
pio run -e cairn-mtprobe          # start here
pio run -e cairn-mtprobe-sniff    # then this, once the plain probe is clean
```

Flash as in `docs/flashing-and-testing.md`. The sniff variant takes the OBD link out
of request mode for each window and re-initialises the ECU session if a plain
request fails afterwards; if that fails too, sniffing is switched off for the boot.
It has not met the real adapter, so run the plain probe first and watch the first
sniff window on serial.

Suggested drive, taken from the usual approach to finding a clutch signal:

1. Ignition on, engine off: clutch untouched, fully down, released; ten seconds each.
2. Engine running, stationary: the same.
3. Steady speed in each gear for several seconds, then neutral, then coasting.
4. Slow, deliberate 1-2, 2-3, 3-4 shifts, then some brisk ones.

Leave a few seconds between phases and note the order; the log has no annotations.

## Reading the result

```sh
grep ' MTP' /path/to/log        # everything
grep 'answers' …                # first time each candidate replied
grep 'claims' …                 # what the ECU's bitmaps said
```

Request lines look like:

```
t=731241 req=010C0D11A4 62 61 dt=58 ans=0C,0D,11,62 rx=41 0C 0B B8 0D 3C 11 40 62 7D
```

`t` is the device millisecond clock, the same one the bundle's `mono_ms` is based on.
`ans` lists the PIDs found in the reply, so a missing candidate is unsupported, not
lost. `rx` is the raw reply, kept verbatim so decoding stays correctable offline.
`dt` is the round trip, which shows what the extra requests cost the bus.

What each outcome means:

| Result | Meaning |
|---|---|
| `A4` answers | The ECU reports gear directly. Unlikely. |
| `62`/`63` answer | Torque is available; horsepower follows from torque and RPM. |
| Pedal PIDs answer | Driver input is available independent of throttle. |
| Nothing new answers | Standard OBD gives RPM and speed only. Gear is estimated; clutch needs another route. |
| Sniff shows only 7E8-style replies | The connector is diagnostic-only, as expected. |
| Sniff shows other IDs | Capture them; those are the candidates for a clutch or gear frame. |

## What is deliberately absent

- **BMW UDS reads on the DME.** That is the likeliest software route to a clutch
  switch value, but the identifiers are not public and trying them blind at an
  engine ECU from a moving car should follow a bench test.
- **A clutch input.** If nothing in the car carries it, a Hall sensor on the pedal
  arm into the molex GPIO socket (GPIO26/34, which the external GNSS also uses) is
  the dependable answer. It needs hardware and a level shifter and is not built here.

## Cost

Each request adds a bus round trip on top of the 200 ms production batch, so the
production cadence may stretch while the probe runs. Compare `poll_cadence_ms` in the
recorded rows against a drive on the plain `cairn` build. The log tree is capped at
16 MiB and rotates at 2 MiB, and the probe writes roughly 300 bytes a second.

Host tests for the reply parser: `make -C firmware/cairn-v2/test/host mtprobe`.

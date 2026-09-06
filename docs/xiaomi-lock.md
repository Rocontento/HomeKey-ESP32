# Xiaomi lock over local miIO

Tap a HomeKey → the ESP32 sends an **unlatch** to a Xiaomi lock directly over the LAN
(miIO, UDP 54321). No Home Assistant, no Xiaomi cloud at unlock time, no Matter.

Tested target: `xiaomi.lock.d100e` (Xiaomi Self-Install Smart Lock).

## Why miIO and not Matter

The lock speaks Matter, but the Matter Door Lock cluster only gives you lock/unlock —
there is no unlatch. The MIoT action set does have it:

| siid | aiid | action | params |
|------|------|--------|--------|
| 18 | 4 | `emergency-unlock` — motor pull, the unlatch | none |
| 18 | 9 | `ble-unlock` | none |
| 18 | 1 | `remote-unlock-e` | **encrypted cloud secret** |
| 18 | 5 | `voice-unlock` | **encrypted cloud secret** |

Actions 4 and 9 take no parameters, so they can be called from the device with nothing
but the lock's local token. That is exactly what `hass-xiaomi-miot` does in local mode
(`local.async_send('action', {did, siid, aiid, in})`).

## Configuration

Three values, stored in the `misc` config (NVS) like every other setting:

| key | meaning |
|-----|---------|
| `xiaomiLockIp` | the lock's LAN address (give it a DHCP reservation) |
| `xiaomiLockToken` | 32 hex chars, the lock's local token |
| `xiaomiLockDid` | numeric device id |
| `xiaomiUnlatchAiid` | `4` (default) or `9` |

They can be set from the web portal's Xiaomi login (which pulls them from the Xiaomi
account), or written by hand through the config API.

Not sure which aiid your unit wants? Turn on debug logging for `xiaomi_miot` in Home
Assistant, press the button that already works, and read the line
`Call miot action {'did': ..., 'siid': 18, 'aiid': N, 'in': []}`.

## How it hooks in

`MiioLock` subscribes to `NFC_EVENT/NFC_TAP_EVENT` and fires on an authenticated
`HOMEKEY_TAP`, with a 3 s cooldown so one tap is one unlatch. Nothing else in the
firmware changes: the GPIO relay path is untouched, so a wired strike and the Xiaomi
lock can coexist.

After the round-trip it publishes `LOCK_UPDATE_STATE` with the real outcome — `UNLOCKED`
on `{"code":0}`, `JAMMED` if the lock never answered — so the HomeKit tile reflects
reality instead of assuming success.

## Protocol note

miIO packet: `0x2131 | len | 0000 0000 | device_id | stamp | md5(header+token+data) | data`,
where `data` is AES-128-CBC(`key=md5(token)`, `iv=md5(key+token)`) over the JSON payload
plus a trailing NUL, PKCS7-padded. `MiioLock::selftest()` runs at boot and checks the
packer against a known-answer vector generated with openssl; a failure is logged loudly.

## Not done yet

- Reading lock state back (`get_properties` siid 19 / piid 12), so openings by fingerprint
  or keypad are not reflected in HomeKit.
- Retry when the lock's radio is asleep.

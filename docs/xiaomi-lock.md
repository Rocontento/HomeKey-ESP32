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

### From the web portal

Open **Xiaomi lock** in the portal, pick your server region, and log in with the Xiaomi
account the lock is registered to. The device list comes back with each device's local
token; press **Use** on the lock, then **Save**. **Test unlatch now** fires one command
so you can confirm it works without walking to the door.

The password is used for that one login and is never stored — only the selected lock's
ip/token/did are written to NVS. If the account has two-factor enabled, the portal shows
the verification URL: open it once in a browser, confirm, then log in again.

Endpoints behind the page (all under the portal's basic auth):

| endpoint | purpose |
|----------|---------|
| `POST /xiaomi/login` | log in, return devices with tokens |
| `POST /xiaomi/select` | store the chosen lock, apply it live |
| `POST /xiaomi/test` | fire one unlatch |

### By hand

The same three fields can be typed into the form, or PUT through the normal config API,
if you would rather run [Xiaomi-cloud-tokens-extractor](https://github.com/PiotrMachowski/Xiaomi-cloud-tokens-extractor)
yourself and never give the firmware your password.

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

## Cloud login flow

`XiaomiCloud` mirrors the reference implementations: `serviceLogin` for a `_sign`,
`serviceLoginAuth2` with `MD5(password)` for `ssecurity` + a location URL, a GET on that
location to pick up the `serviceToken` cookie, then `/home/device_list` signed the way
Xiaomi wants it — SHA1 over the ordered parameters, values RC4-encrypted with a key
derived as `SHA256(ssecurity || nonce)` and the customary 1024-byte RC4 warm-up.

It only runs when you press the login button. Nothing polls the cloud, and unlocking
never touches it.

## Not done yet

- Reading lock state back (`get_properties` siid 19 / piid 12), so openings by fingerprint
  or keypad are not reflected in HomeKit.
- Retry when the lock's radio is asleep.

# Key storage

Every example here follows one rule: **the device's private key is made on the device and never leaves it.** Only a certificate signing request (CSR) goes to your computer, and AWS IoT signs a certificate from it. AWS never sees the key, and neither does anything in between.

What differs is how the device keeps that key once it has it.

## The levels

| Level | Where the key is | Protects against | Does not protect against |
|---|---|---|---|
| **Plain file key** | Raspberry Pi: `/etc/rpi-aws-examples/device.key`, readable only by root and the service user. Pico: plain in flash | Other users and processes on the device | Anyone with the SD card, a backup or copy of it, or the Pico and `picotool`: they have the key |
| **Key ladder** (wrapped; the default here) | Encrypted under a key that only this chip can derive, from its one-time-programmable (OTP) memory. Plain only in RAM | Copies: the SD card or its image, a backup, a support bundle, a flash dump. Useless on any other board | Someone holding the board **without secure boot** (they boot their own software and ask the chip). Remote code running on the device (it can use the key, or read the RAM copy) |
| **Key ladder with secure boot** | As above; only your signed software runs | As above, and someone holding the board, to the level of the hardware | Remote code running on the device. A hardware attack |
| **Secure element or TPM** (not covered here) | In separate hardware that signs but never releases the key | All of the above, including root on the device copying the key | Root on the device *using* the key while it runs |

The READMEs use the key ladder: it costs nothing extra once OTP holds a device key, and a copy of a card or a flash dump gives nothing away. Add secure boot for products. Plain file keys are [below](#plain-file-keys), for when you need them.

## The key ladder on Raspberry Pi computers

```
OTP key 1: a device-unique key that the firmware holds and never releases
  └─ KEK = HMAC-SHA256(OTP key 1, "AWS_cert_keys:v1")    the firmware computes it: rpi-fw-crypto hmac
       └─ AES-256-GCM wrap of the AWS device key          /var/lib/rpi-aws-examples/device.key.wrapped (root only)
            └─ plain copy for TLS, made at each boot       /run/rpi-aws-examples/device.key (RAM, service user)
```

- `rpi-aws-keyhelper` is the only program that asks the firmware for the KEK. It runs as root and does a few fixed operations on fixed files.
- `rpi-aws-keyunwrap.service` writes the plain copy into `/run` (RAM) at every boot, before the client or Greengrass starts. The plain key is never written to the card.
- The label `AWS_cert_keys:v1` names this use of the OTP key. The same OTP key can serve other uses (such as Raspberry Pi Connect, which signs with it) under other labels; an HMAC reveals nothing about the key itself.
- **OTP key 1 must be programmed.** `sudo rpi-aws-otp-init` checks, without writing anything. If the key is there (as on a board set up by `rpi-sb-provisioner`, or used with Connect), you are ready. If not, the script explains what `--write` would do and asks you to type the board's serial: the write is **permanent**, and on a Pi 5 or CM5 that one key slot is also the one Raspberry Pi Connect uses.
- `rpi-aws-keygen --store wrapped` refuses to work without OTP key 1. It never falls back to a plain file.
- **Zero 2 W** has OTP key 1 too, so the key ladder works on it. It has no secure boot, so it protects copies of the card only.
- **Pi 4:** the firmware can lock the OTP key against being read out (`lock_device_private_key=1` in `config.txt`), but on BCM2711 kernel code can still read OTP directly, so root on a Pi 4 could take the device key off the board. Secure boot keeps unsigned kernels from running; it does not help against your own kernel being compromised.

The firmware crypto interface is described in [rpifwcrypto](https://github.com/raspberrypi/utils/tree/master/rpifwcrypto).

## The key ladder on the Pico 2 W

The RP2350 has no firmware key of its own to ask, so the firmware keeps its own root key in OTP:

```
root key: 256 bits from the hardware TRNG, in OTP page 60 (written once, by pico-otp-init.py)
  └─ KEK = HMAC-SHA256(root, "AWS_cert_keys:v1")    at every boot; then page 60 is locked until reset
       └─ AES-256-GCM wrap of the device key         in flash
            └─ the plain key                          in RAM only
```

- The wrapped key uses the same format as on the Raspberry Pi computers.
- The root is written only when you run `pico/tools/pico-otp-init.py --write` and type the Pico's thing name. The firmware never writes OTP by itself.
- After reading the root at boot, the firmware locks its OTP page until the next reset, so code that runs later cannot read it.
- **In production** the RP2350 needs secure boot (only signed firmware runs), debug and BOOTSEL turned off, and the root's page locked permanently against the boot ROM and Non-secure code. Without those, someone holding the Pico can load their own firmware and read the root. See Raspberry Pi's [RP2350 security documentation](https://www.raspberrypi.com/documentation/microcontrollers/silicon.html#rp2350) and the RP2350 datasheet's chapters on security and OTP.
- Where the key, certificate and Wi-Fi settings are in flash, and how a product should lay out flash instead (a partition table, permissions, power-safe writes): [pico/README.md](../pico/README.md#where-things-are-in-flash-and-what-a-product-should-do).
- Use stepping **A4** parts, which is what Raspberry Pi sells. On earlier steppings someone holding the chip can read OTP (RP2350 erratum E16).
- **Pico W (RP2040)** has no OTP for this and no TRNG: plain key only.

## Plain file keys

The key ladder needs a device key in OTP, and programming one is permanent. Where that is not wanted, the key can be kept plain:

- a board you will hand back or reuse, whose OTP you will not write;
- a Pico W (RP2040), which has no OTP for it;
- trying the flow out, on a board you will wipe afterwards.

| Device | Instead of | Use |
|---|---|---|
| Raspberry Pi computer, client or Greengrass | `sudo rpi-aws-keygen --store wrapped > device.csr` | `sudo rpi-aws-keygen > device.csr`, and skip `rpi-aws-otp-init` |
| Pico 2 W | building with the key ladder (the default) | build with `-DHELLO_KEY_LADDER=OFF`, and skip `pico-otp-init.py` |
| Pico W | | nothing: the Pico W has only the plain key |

Everything else is the same. The key is then `/etc/rpi-aws-examples/device.key` (readable by root and the service user) on a Raspberry Pi computer, or plain in the Pico's flash. Anyone with a copy of the card, a backup, or the Pico and `picotool` has it, so:

- do not use a plain key on a device you deploy;
- remove it when you have finished (`sudo rpi-aws-keygen --reset`, or `picotool erase -a`), and `aws-iot-unregister` the thing, which deactivates and deletes its certificate;
- to move a device to the key ladder later, start again with a new key: the plain key cannot be made safe after the fact, because copies of it may exist.

## Wi-Fi settings

A Wi-Fi password is a secret shared by every device on the network, so nothing here builds one into an image or a `.uf2`: each device gets it when it is set up, and keeps it like its key.

| Device | How it gets them | Where they are kept |
|---|---|---|
| Pico 2 W | `pico-enrol.py` asks for them and sends them over USB | Flash, wrapped under the key ladder's KEK (plain with `-DHELLO_KEY_LADDER=OFF`) |
| Pico W | The same | Flash, plain |
| Image (Zero 2 W, or any Pi on Wi-Fi) | `rpi-aws-wifi.txt` on the card's boot partition, imported at first boot and deleted | iwd's profile: wrapped with the device key by `rpi-aws-keygen --store wrapped`, plain in RAM only; plain in `/var/lib/iwd` with a plain file key |
| Raspberry Pi OS from Imager | Imager's settings | NetworkManager's connection file, plain (root only) |

- The boot-partition file is overwritten before it is deleted, but an SD card may keep old copies of data it has overwritten (wear levelling, and FAT itself): treat a card that held it as holding the password. The same goes for the plain iwd profile, deleted at the boot after the key ladder wraps it.
- Without secure boot (and a Zero 2 W has none), someone holding the device can get the password, wrapped or not. Wrapping protects copies: the card, an image of it, a flash dump.
- **On the network side**, which matters more in production: put devices on their own network or VLAN that reaches only AWS IoT and what they need; and where your access points support it, give each device its own password (per-device PSK) or its own certificate (WPA2/WPA3-Enterprise with EAP-TLS), so that one device's secret can be revoked alone and opens nothing else.
- Pico SDK examples worth knowing: [ble_wifi_provisioner](https://github.com/raspberrypi/pico-examples/tree/master/bluetooth/ble_wifi_provisioner) sets Wi-Fi over Bluetooth LE instead of USB (it keeps the settings plain in flash), and [hello_encrypted](https://github.com/raspberrypi/pico-examples/tree/master/encrypted/hello_encrypted) with its encrypted bootloader keeps secrets in an encrypted, signed image on an RP2350 with secure boot.

## Secure boot

Secure boot is what makes the key ladder protect a device that someone holds. It is permanent (a public key is written to OTP and cannot be changed), so treat it as a product decision:

- Raspberry Pi computers: [secure boot](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#secure-boot) and [rpi-sb-provisioner](https://github.com/raspberrypi/rpi-sb-provisioner), which also programs OTP key 1 and can encrypt the disk. Zero 2 W has no secure boot.
- RP2350: secure boot, signed images and anti-rollback, in the RP2350 datasheet and Raspberry Pi's documentation.

This repository does not walk through secure boot.

## Rotation and revocation

Certificates that AWS IoT signs from a CSR are valid until 2049, so nothing expires to force a change. Good practice anyway:

- **Rotate** keys and certificates on a schedule, after security updates and whenever you suspect a device: make a new key on the device, register its CSR, switch to the new certificate once it connects, then retire the old one. The shorter the device's protection (no secure boot, or a Zero 2 W), the more often.
- **Revoke** (deactivate) a certificate as soon as you stop using it, so that an old key restored from a backup cannot connect. `aws-iot-unregister` deactivates and deletes certificates; to retire one certificate of a thing, use `aws iot update-certificate --new-status REVOKED`.
- Turn on [AWS IoT Device Defender](https://docs.aws.amazon.com/iot-device-defender/latest/devguide/what-is-device-defender.html) audits, which flag certificates shared by several devices, overly broad policies and old certificates.

These examples do not rotate keys by themselves.

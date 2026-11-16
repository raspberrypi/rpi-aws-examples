# Pico 2 W and Pico W: quick start

`hello-mqtt` connects a Pico 2 W (RP2350) or a Pico W (RP2040) to AWS IoT Core: `hello` once, then a count every 10 seconds, and it prints what you send it. The Pico makes its own private key and only a certificate signing request (CSR) leaves it, over USB. A Pico 2 W keeps the key wrapped under a root key in the RP2350's one-time-programmable (OTP) memory (the *key ladder*), so a flash dump is useless on any other Pico. A Pico W (RP2040) has no OTP for that: its key is plain in flash.

You need a Pico 2 W or Pico W, a USB cable, a computer with the [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.1 or later (and its Arm toolchain), `picotool`, Python 3, and the AWS CLI signed in to your account.

## 0. Set up AWS (on your computer)

You need an AWS account. These steps are done once.

### The AWS CLI and your credentials

1. Install the [AWS CLI v2](https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html) and `openssl` on your computer (Linux, macOS, or WSL on Windows).
2. Sign in with an IAM Identity Center (SSO) user, or with an IAM user's access keys. Do not use the account's root user.

   ```sh
   aws configure sso && aws sso login      # IAM Identity Center (SSO) user
   aws configure                           # or: an IAM user's access keys
   ```

3. Choose a Region near you that has AWS IoT Core and stick to it (or add `--region` to every tool). The console must be in the same Region to show your devices.

   ```sh
   aws configure set region eu-west-2      # for example
   ```

4. Check that this prints your account and identity:

   ```sh
   aws sts get-caller-identity
   ```

5. Get this repository on your computer. The tools are in `rpi-aws-examples/tools/`.

   ```sh
   git clone https://github.com/raspberrypi/rpi-aws-examples.git
   ```

### Permissions

Your identity needs AWS IoT permissions to create and delete things, certificates and policies. The AWS managed policy `AWSIoTFullAccess` covers them all. For a narrower policy, the tools call: `iot:CreateThing`, `DescribeThing`, `DeleteThing`, `CreateCertificateFromCsr`, `DescribeCertificate`, `UpdateCertificate`, `DeleteCertificate`, `CreatePolicy`, `GetPolicy`, `DeletePolicy`, `ListPolicyVersions`, `DeletePolicyVersion`, `AttachPolicy`, `DetachPolicy`, `ListAttachedPolicies`, `ListTargetsForPolicy`, `AttachThingPrincipal`, `DetachThingPrincipal`, `ListThingPrincipals`, `DescribeEndpoint`; and `sts:GetCallerIdentity`. `tools/aws-iot-watch.py` also needs `iot:Connect`, `iot:Subscribe` and `iot:Receive`.

### What gets created, and what it costs

In your account, in your Region, all named `rpi-ex-…`: one **thing** per device (`rpi-ex-<board id>`), its **certificate**, and one shared **policy**, `rpi-ex-client`. Nothing else: no servers, no storage. AWS IoT Core charges per connection minute and per message; a few test devices sending a message every 10 seconds cost a few cents a month (see [AWS IoT Core pricing](https://aws.amazon.com/iot-core/pricing/); new accounts have a free tier). Everything is removed by `tools/aws-iot-unregister` (see the end of this page).

To see it all at any time:

```sh
aws iot list-things --query "things[?starts_with(thingName,'rpi-ex-')].thingName"
aws iot list-policies --query "policies[?starts_with(policyName,'rpi-ex-')].policyName"
```

In the console: **AWS IoT** → **All devices** → **Things**, and **Security** → **Certificates** and **Policies**.

## 1. Build

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S pico/hello-mqtt -B build/pico -DPICO_BOARD=pico2_w
cmake --build build/pico
```

For a Pico 2 W this builds with the key ladder (`HELLO_KEY_LADDER`, on by default for the RP2350). Use `-DPICO_BOARD=pico_w` for a Pico W. The firmware holds no Wi-Fi settings or other secrets, so one `.uf2` serves every Pico of its kind: the Wi-Fi network and password go to each Pico in step 4. The build downloads Amazon Root CA 1 and checks its hash.

## 2. Flash

Hold BOOTSEL while plugging the Pico in, then:

```sh
picotool load -x build/pico/hello_mqtt.uf2
```

Its USB serial port is `/dev/ttyACM0` on Linux, `/dev/cu.usbmodem…` on a Mac. A Pico W makes its key now, and prints `key: made now` and `not registered`: go to step 4. A Pico 2 W with no OTP root yet prints `key ladder: no OTP root on this Pico, so no key`: go to step 3.

If the Pico had other firmware that used a partition table or kept data at the top of the flash, erase it first (OTP is not touched):

```sh
picotool erase -a
```

With more than one Pico plugged in, pick one: a Pico 2 W by its serial number, `picotool load -x … --ser <board id>`. A Pico W in BOOTSEL does not match `--ser`; use `--bus <bus> --address <address>` from `lsusb`, or unplug the others. Its serial port then appears as `/dev/serial/by-id/usb-Raspberry_Pi_Pico_<board id>-if00`.

## 3. Write the OTP root (Pico 2 W)

```sh
pico/tools/pico-otp-init.py --port /dev/ttyACM0            # dry run: shows the state and what it would write
pico/tools/pico-otp-init.py --port /dev/ttyACM0 --write    # writes it, after you type the Pico's thing name
```

The root is 256 random bits from the RP2350's TRNG, written into one OTP page **once, for the life of the chip**. If a root is already there, the script says so and writes nothing. The Pico then restarts, makes its key, wraps it and prints `key: made now` and `not registered`. If you would rather not write OTP, build with `-DHELLO_KEY_LADDER=OFF` instead: the key is then plain in flash ([docs/key-storage.md](../docs/key-storage.md#plain-file-keys)).

## 4. Register

```sh
pico/tools/pico-enrol.py --port /dev/ttyACM0
```

Run it from the repository's top directory. If the Pico has no Wi-Fi settings yet, it first asks for the network (SSID) and its password (WPA2; not shown as you type) and sends them to the Pico, which keeps them in flash: wrapped by the key ladder on a Pico 2 W, plain on a Pico W. Then it reads the Pico's CSR, runs `tools/aws-iot-register` with it (the thing is `rpi-ex-<board id>`), and sends the certificate and endpoint back. The Pico checks that the certificate is for its own key, saves it and restarts. If the Pico is plugged into another computer than the one with your AWS credentials, run the steps apart: see `pico-enrol.py --help`. To change the Wi-Fi network later:

```sh
pico/tools/pico-enrol.py wifi --port /dev/ttyACM0
```

## 5. See the messages

The Pico's serial port shows Wi-Fi, the clock (SNTP), the connection, then each publish. In the console, **AWS IoT** → **Test** → **MQTT test client**, subscribe to `rpi/#`: `{"count": n}` arrives on `rpi/rpi-ex-<board id>/count` every 10 seconds. Publish anything to `rpi/rpi-ex-<board id>/cmd` and the Pico prints it. Or from your computer:

```sh
tools/aws-iot-watch.py 'rpi/#'
```

## How it works

| Piece | Where |
|---|---|
| The key: EC P-256, made at first boot (on a Pico 2 W, once the OTP root is there) | The top 4 KB sector of flash: wrapped on a Pico 2 W, plain DER on a Pico W. Reflashing the program keeps it; `picotool erase -a` or the `RESET KEY` command removes it |
| Its randomness | Pico 2 W: the RP2350's hardware TRNG, with its health tests on ([trng_rp2350.c](hello-mqtt/trng_rp2350.c)). Pico W: the SDK's `pico_rand`, which is **not** a cryptographic generator: fine for trying things out, not for products |
| Certificate and endpoint | The sector below the key |
| Wi-Fi network and password | The sector below that: wrapped on a Pico 2 W, plain on a Pico W |
| TLS and MQTT | lwIP's TLS layer over mbedTLS, and [coreMQTT](https://github.com/FreeRTOS/coreMQTT) (MQTT 3.1.1, QoS 1); the clock from SNTP first, as TLS needs the date |

### Where things are in flash, and what a product should do

**What this example does.** It keeps it simple: three 4 KB sectors at the top of flash, outside the program, one record each (a magic number, length and CRC, then the data), rewritten by erasing the sector and programming it. There is no partition table. A `.uf2` writes only the sectors it covers, so reflashing keeps them; `picotool erase -a` wipes them; and the firmware refuses to start if the program has grown into them. That works the same on an RP2040 and an RP2350, and it is enough for an example, but:

- **Nothing reserves those sectors.** Another program, a larger build, or a library can use them. The SDK's Bluetooth stack does: its flash bank (`PICO_FLASH_BANK_STORAGE_OFFSET`) is the top two sectors by default, where the key and certificate are.
- **A write is not safe against power loss.** Between the erase and the program there is no copy at all; lose power then and the record is gone (for the key: a new key, and registering again).
- **Anyone with the Pico can read flash**, over BOOTSEL with `picotool` or from any code on it. The key ladder makes the key and Wi-Fi settings useless off this Pico; the certificate and endpoint are not secret.
- The sectors' place depends on `PICO_FLASH_SIZE_BYTES` being right for the board.

**What a product should do instead.**

- **RP2350: a partition table.** Give the identity its own data partition (family `data`), separate from the firmware, so that an update never touches it, and find it at run time with the boot ROM's partition calls rather than at a fixed address. Use A/B firmware partitions for updates over the air. With secure boot, set the identity partition's permissions so that only Secure code can read it, not the bootloader (BOOTSEL and `picotool`) or Non-secure code. See pico-examples' [partition_info](https://github.com/raspberrypi/pico-examples/tree/master/flash/partition_info) (a table built into the binary with `pico_embed_pt_in_binary`), [runtime_flash_permissions](https://github.com/raspberrypi/pico-examples/tree/master/flash/runtime_flash_permissions) and [ota_update](https://github.com/raspberrypi/pico-examples/tree/master/pico_w/wifi/ota_update), and the RP2350 datasheet's chapter on the boot ROM.
- **RP2040: no partition tables.** Reserve the region in the linker script (shorten the flash the program may use) so that a build that grows into it fails, and keep every library's storage (Bluetooth's flash bank, a filesystem) out of it. Flash cannot be protected on an RP2040: a product that must keep secrets from someone holding it needs an RP2350 with secure boot, or a secure element.
- **Write safely.** Keep two copies with a sequence number, and write the new one before the old one is erased, so that a power cut leaves one good copy; or use a store built for it (littlefs, or BTstack's TLV store). Never erase the only copy of a key.
- **Plan for the device's life**: what a factory reset erases, and what a firmware update must keep.

On a Pico W, or a Pico 2 W built with `-DHELLO_KEY_LADDER=OFF`, the key is a plain copy in flash: anyone with the Pico and `picotool` can read it. [docs/key-storage.md](../docs/key-storage.md) says what each choice protects against, and why products also need secure boot.

## The key ladder (Pico 2 W)

On a Pico 2 W (the default build), the firmware keeps the key wrapped:

```
root key: 256 bits from the TRNG, in OTP page 60 (rows 0xF00-0xF0F, then a marker in row 0xF10)
  └─ KEK = HMAC-SHA256(root, "AWS_cert_keys:v1")    at every boot; the root is wiped from RAM, and the page
       │                                             is locked against reads until the next reset
       └─ AES-256-GCM wrap of the device key         in flash: useless on any other Pico
            └─ the plain key                          in RAM only
```

- If the root is already there, `pico-otp-init.py` says so and writes nothing. The firmware itself never writes OTP; until a root exists it makes no key at all (it does not fall back to a plain one).
- A Pico moved from a plain-key build makes a new, wrapped key, over the old plain one, and must be registered again (`aws-iot-unregister` the old certificate first, or keep the thing and let `aws-iot-register` add the new one).
- `OTP_ROOT_PAGE` (4-60, default 60) moves the root to another user page. The page holds nothing else, because the lock covers a whole page. Keep one page for all your devices.
- Customers' RP2350 parts are stepping A4. Earlier steppings work the same, but their OTP can be read by someone holding the chip (erratum E16).
- What this protects against, and what it needs in production (secure boot, debug and BOOTSEL closed, the page locked permanently): [docs/key-storage.md](../docs/key-storage.md).

## Commands over USB serial

`pico-enrol.py` uses these; you can type them too (one per line):

| Command | Answer |
|---|---|
| `ID` | The thing name, `rpi-ex-<board id>` |
| `CSR` | A CSR for the Pico's key |
| `ENDPOINT <host>`, `CERT` (then the PEM lines), `SAVE` | Store the endpoint and certificate, after checking the certificate against the key; restart |
| `STATUS` | Registered or not |
| `WIFI-SSID <name>`, `WIFI-PASSWORD <password>`, `WIFI-SAVE` | Store the Wi-Fi settings (WPA2); restart if it was connected |
| `WIFI-STATUS` | The saved network's name, or `none`; never the password |
| `RESET`, `RESET KEY` | Forget the certificate and endpoint (and the key and Wi-Fi settings); restart |
| `OTP-STATUS`, `OTP-INIT <thing>` | Key ladder builds only: the root's state; write the root (what `pico-otp-init.py` sends) |

## Remove it

```sh
tools/aws-iot-unregister rpi-ex-<board id>
picotool erase -a          # with the Pico in BOOTSEL
rm -rf rpi-ex-<board id>   # optional: the certificate and settings from registering
```

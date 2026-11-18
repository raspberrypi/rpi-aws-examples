# Raspberry Pi and AWS IoT: examples

> Looking for the earlier Raspberry Pi 5 example, which passes the AWS Device Advisor tests? It is still here: [Pi5AWSExample.py](Pi5AWSExample.py), with its own [README](README-Pi5AWSExample.md).

These examples connect Raspberry Pi devices to [AWS IoT Core](https://aws.amazon.com/iot-core/), the AWS service that devices send messages to and receive them from. Each device publishes a message every 10 seconds, which you can watch in the AWS IoT console, and acts on messages you send to it. They cover the Raspberry Pi computers (Pi 4, Pi 5, the Compute Modules and the Zero 2 W), with or without [AWS IoT Greengrass](https://aws.amazon.com/greengrass/), and the Pico 2 W and Pico W. A Raspberry Pi computer with a camera can also stream live video to [Amazon Kinesis Video Streams](https://aws.amazon.com/kinesis/video-streams/), using the same device identity.

## What the examples show

To connect, a device proves who it is with a **private key** and a **certificate** that AWS IoT trusts. Anyone with a copy of the private key can pretend to be the device, so how the key is made and kept matters.

A common way to start is to have AWS make the key and certificate, download them, and copy them onto the device. The key has then passed through your computer, and it sits on the device as an ordinary file: anyone who copies the SD card has it too.

These examples do it the other way round:

1. **The device makes its own private key.** The key never leaves the device.
2. The device makes a **certificate signing request (CSR)**. It holds only the matching public key, which is safe to share, and you copy it to your computer.
3. **AWS IoT signs a certificate** from the CSR, and you copy the certificate back to the device.
4. The device **connects** with its key and certificate, and starts publishing.

On boards that support it, the key is also **protected on the device**: it is stored encrypted under a key that only that board can derive, from a secret in its one-time-programmable memory (OTP). A copy of the SD card or flash is then useless on any other board. See [How the key is kept](#how-the-key-is-kept).

## Prerequisites

- An **AWS account**, and permission to create AWS IoT things, certificates and policies (and, for Greengrass, an IAM role). Each README lists the exact permissions it needs.
- A **computer with bash** and the [AWS CLI](https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html), signed in to your account. The scripts in [tools/](tools/) run there, with your own credentials.
- One of the **boards** below, and a way to put software on it: [Raspberry Pi Imager](https://www.raspberrypi.com/software/) and an SD card for a Raspberry Pi computer, or a USB cable for a Pico.

Nothing here runs a service or needs infrastructure of its own: the scripts create the AWS IoT objects, and each has a matching teardown that removes them again.

## Pick your board and mode

| Board | Plain client (MQTT) | Greengrass Nucleus Lite | Greengrass Classic |
|---|---|---|---|
| Raspberry Pi 4, Pi 5, CM4, CM5 | [sbc/](sbc/README.md) | [sbc/greengrass/](sbc/greengrass/README.md) | [sbc/greengrass/](sbc/greengrass/README.md) |
| Raspberry Pi Zero 2 W | [sbc/](sbc/README.md) | [sbc/greengrass/](sbc/greengrass/README.md) | [sbc/greengrass/](sbc/greengrass/README.md) (512 MB: tight; Lite is the better fit) |
| Pico 2 W, Pico W | [pico/](pico/README.md) | | |

**Video.** On a Raspberry Pi computer with a camera, [sbc/kvs/](sbc/kvs/README.md) streams live video to Kinesis Video Streams, for you to watch in the AWS console. It builds on the plain client's registration, and the Pi gets short-lived AWS credentials with its certificate, so no AWS keys go on it.

Raspberry Pi computers run **Raspberry Pi OS Trixie** (64-bit). Install on a running Pi (the quick start), or build an image with everything in it with [rpi-image-gen](sbc/image/README.md).

**What each mode gives you.** A *plain client* is your own program with the AWS IoT Device SDK: small, and you own the connection. A *Greengrass core* runs the Greengrass nucleus, which holds the connection and deploys components to the device from the cloud. *Nucleus Lite* is the small C version for boards with little memory; *Classic* is the full Java nucleus, with the widest set of AWS components.

## How the key is kept

The instructions here keep the key wrapped by the **key ladder**. A plain key is there for when you need one: [docs/key-storage.md](docs/key-storage.md#plain-file-keys).

| | Key ladder (what the READMEs do) | Plain key (if you need it) |
|---|---|---|
| Raspberry Pi computers | Wrapped under a key the firmware derives from the board's OTP device key; plain only in RAM | A plain file, readable by root and the service user |
| Pico 2 W | Wrapped under a root key in the RP2350's OTP | Plain in flash |
| Pico W | (no OTP for it) | Plain in flash |
| A copied SD card or flash dump | Useless on any other board | Gives away the key |

Scripts that write OTP are named for it (`rpi-aws-otp-init`, `pico-otp-init.py`), do a dry run by default, and ask you to confirm by typing the device's name, because OTP writes are permanent. See [docs/key-storage.md](docs/key-storage.md) for what each level does and does not protect against, and why products also need secure boot.

## Layout

```
tools/              on your computer: aws-iot-register / -unregister, gg-core-setup / -teardown, gg-deploy-hello,
                    kvs-setup / -teardown, kvs-hls-url, aws-iot-watch.py, and the policy templates
sbc/                Raspberry Pi computers: install.sh, rpi-aws-keygen, the key ladder, the hello client
sbc/greengrass/     rpi-aws-gg-setup and the rpi-ex.Hello component (one component, both nuclei)
sbc/kvs/            rpi-aws-kvs-setup (builds kvssink) and the rpi-aws-kvs camera streamer, for Kinesis Video Streams
sbc/image/          rpi-image-gen layers and configs per board and mode
pico/               hello-mqtt firmware (C, Pico SDK, coreMQTT, mbedTLS), pico-enrol.py, pico-otp-init.py
docs/               concepts, key storage, where to keep the data, troubleshooting, further examples
Pi5AWSExample.py    the earlier Pi 5 example, which passes the AWS Device Advisor tests (README-Pi5AWSExample.md)
```

## Tested

| Path | Board | How |
|---|---|---|
| Client, Nucleus Lite and Classic, each with a wrapped key and a plain file key | Raspberry Pi 4 | Raspberry Pi OS Lite (Trixie) from Imager, then [sbc/](sbc/README.md) and [sbc/greengrass/](sbc/greengrass/README.md); and the `pi4-client`, `pi4-greengrass-lite` and `pi4-greengrass-classic` images |
| Client, Nucleus Lite and Classic, each with a wrapped key and a plain file key | Raspberry Pi 5 | Raspberry Pi OS Lite (Trixie) from Imager, then [sbc/](sbc/README.md) and [sbc/greengrass/](sbc/greengrass/README.md); and the `pi5-client`, `pi5-greengrass-lite` and `pi5-greengrass-classic` images |
| Client, Nucleus Lite and Classic, each with a wrapped key and a plain file key | Compute Module 4 on the CM4 IO board | Raspberry Pi OS Lite (Trixie) from Imager, then [sbc/](sbc/README.md) and [sbc/greengrass/](sbc/greengrass/README.md); and the `cm4-client`, `cm4-greengrass-lite` and `cm4-greengrass-classic` images |
| Client, Nucleus Lite and Classic, each with a wrapped key and a plain file key | Compute Module 5 on the CM5 IO board | Raspberry Pi OS Lite (Trixie) from Imager, booted from USB or an NVMe SSD, then [sbc/](sbc/README.md) and [sbc/greengrass/](sbc/greengrass/README.md); and the `cm5-client`, `cm5-greengrass-lite` and `cm5-greengrass-classic` images, on an NVMe SSD |
| Client, Nucleus Lite and Classic, each with a wrapped key and a plain file key | Raspberry Pi Zero 2 W | Raspberry Pi OS Lite (Trixie) from Imager, then [sbc/](sbc/README.md) and [sbc/greengrass/](sbc/greengrass/README.md); and the `zero2w-client`, `zero2w-greengrass-lite` and `zero2w-greengrass-classic` images. With Classic, 130–200 MB of the 512 MB stay free |
| Video to Kinesis Video Streams, plain file key, software encoder (x264) | Raspberry Pi 5 with Camera Module 3 | Raspberry Pi OS (Trixie), then [sbc/](sbc/README.md) with `install.sh --no-client` and [sbc/kvs/](sbc/kvs/README.md), in `eu-west-2`. The hardware-encoder path (Pi 4, CM4, Zero 2 W) is untested |
| Key ladder (with a root already in OTP), and a plain key | Pico 2 W (RP2350 A2) | [pico/](pico/README.md) |
| Plain key | Pico WH (RP2040) | [pico/](pico/README.md) |

Other boards and the image configs not listed are untested; each image config says at its top whether it has been tried.

## Licence

BSD-3-Clause: see [LICENCE.txt](LICENCE.txt).

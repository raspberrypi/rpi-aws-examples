# Raspberry Pi OS images with rpi-image-gen

Instead of installing on a running Pi (the [quick start](../README.md)), build a Raspberry Pi OS image with everything already in it, using [rpi-image-gen](https://github.com/raspberrypi/rpi-image-gen) and the layers here. Write it to an SD card or eMMC, boot, and go straight to making the key.

**Images hold nothing per device and no secrets.** Each device makes its own key after its first boot, and gets its own certificate, exactly as in the quick start. Wi-Fi settings go on each card, not into the image. One image serves every board of its kind.

## Layers

| Layer | Adds |
|---|---|
| `rpi-aws-examples-base` | `rpi-aws-keygen`, the key ladder helper and its unwrap unit, `rpi-aws-otp-init`, `rpi-aws-gg-setup`; the service user `rpi-ex` and `/etc/rpi-aws-examples`; a first-boot unit that grows the root filesystem to fill the card; mDNS, so that `<host name>.local` works; `rpi-aws-wifi-import`, which takes Wi-Fi settings from the boot partition |
| `rpi-aws-examples-client` | The `rpi-aws-hello` client and the AWS IoT Device SDK for Python, its unit enabled (it starts once the device has a certificate) |
| `rpi-aws-examples-greengrass-classic` | A Java runtime and the Greengrass nucleus installer, pinned and signature-checked on the build host. Not set up as a core: `rpi-aws-gg-setup` does that on the device |
| `rpi-aws-examples-greengrass-lite` | Greengrass Nucleus Lite from its release package, pinned and hash-checked on the build host; not started |

The base layer runs the same `sbc/install.sh` as the quick start, so a Pi set up either way ends up the same.

## Configs

One per board and mode in [config/](config/): `pi4-client`, `pi5-greengrass-classic`, `zero2w-greengrass-lite` and so on. Each config says at the top whether it has been tested on a real board.

Every config logs in as `pi` with **your SSH key only**, and lets `pi` use `sudo` without a password (there is no password). That suits a desk setup; for a product, leave `user1sudo` out.

## Build

On an arm64 Linux machine (a Raspberry Pi 4 or 5 with Raspberry Pi OS or Debian Trixie, several GB free), with rpi-image-gen installed as its README says, and a JDK for `jarsigner` when you build a Greengrass Classic image:

```sh
export PATH=$PATH:/usr/sbin
rpi-image-gen/rpi-image-gen build -S rpi-aws-examples/sbc/image \
    -c rpi-aws-examples/sbc/image/config/pi4-client.yaml -- \
    IGconf_awsex_src=$PWD/rpi-aws-examples \
    IGconf_ssh_pubkey_user1=$HOME/.ssh/id_ed25519.pub \
    IGconf_device_hostname=my-pi
```

The image is in `work/image-rpi-aws-examples-pi4-client/rpi-aws-examples-pi4-client.img`.

The image holds no Wi-Fi settings: each card gets its own when you write it (below). Do not use rpi-image-gen's `IGconf_iwd_profile`, which would build your Wi-Fi password into the image.

## Set up AWS (on your computer)

You need an AWS account; this is done once.

1. Install the [AWS CLI v2](https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html) and `openssl`.
2. Sign in with an IAM Identity Center (SSO) user or an IAM user's access keys; not the root user.

   ```sh
   aws configure sso && aws sso login      # IAM Identity Center (SSO) user
   aws configure                           # or: an IAM user's access keys
   ```

3. Pick a Region with AWS IoT Core and keep to it. The console must be in the same Region.

   ```sh
   aws configure set region eu-west-2      # for example
   ```

4. Check that this prints your account and identity:

   ```sh
   aws sts get-caller-identity
   ```

5. Clone this repository on your computer too. The tools are in `rpi-aws-examples/tools/`.

   ```sh
   git clone https://github.com/raspberrypi/rpi-aws-examples.git
   ```

**Permissions.** For a client image: AWS IoT things, certificates and policies (the managed policy `AWSIoTFullAccess`). For a Greengrass image, also: IAM role creation and `iam:PassRole` on `rpi-ex-gg-tes`, Greengrass components, deployments and core devices (`AWSGreengrassFullAccess`), and `logs:DeleteLogGroup`. The full lists are in [sbc/README.md](../README.md#permissions) and [sbc/greengrass/README.md](../greengrass/README.md#permissions).

**What gets created and what it costs.** All named `rpi-ex-…`: per device a thing, its certificate and a policy; for Greengrass also a token-exchange role and role alias, a thing group with its deployments, the `rpi-ex.Hello` component and its CloudWatch log group. No servers or storage. A few test devices cost a few cents a month in AWS IoT messages and CloudWatch Logs ([AWS IoT Core pricing](https://aws.amazon.com/iot-core/pricing/)). The commands at the end of this page remove it all.

## Write, boot and enrol

1. Write the `.img` to the card with Raspberry Pi Imager (**Choose OS** → **Use custom**), with **no** OS customisation (the image already has its user, SSH key and host name), or with `dd`.
   **Wi-Fi** (a Zero 2 W, or any Pi without Ethernet): open the card's boot partition on your computer (it appears as a drive called `bootfs` or similar) and add a file `rpi-aws-wifi.txt`:

   ```
   ssid=MyNetwork
   password=my Wi-Fi password
   ```

   At first boot the Pi imports it for iwd, overwrites it and deletes it. With the key ladder (step 3) the Wi-Fi profile is wrapped too, like the device key: a copy of the card then gives away neither. To change networks later, add the file again and reboot.
   The image boots from an SD card, eMMC or NVMe, **not from USB**: rpi-image-gen finds the root filesystem through the boot device's "slot" links, which it makes only for SD/eMMC and NVMe, and a USB boot stops in the initramfs with `/dev/disk/by-slot/system does not exist`. On a CM4 or CM5 with eMMC, whose IO board SD slot does not work, write the image to the eMMC (rpiboot) or to an NVMe drive.
2. Boot the Pi and log in, with the host name you built with. The first boot grows the filesystem to fill the card.

   ```sh
   ssh pi@my-pi.local
   ```

3. Make the key and its CSR, on the Pi. Nothing needs installing:

   ```sh
   sudo rpi-aws-otp-init                               # checks OTP key 1; writes nothing
   sudo rpi-aws-keygen --store wrapped > device.csr
   ```

   The key is wrapped under a key that only this board's firmware can derive (the key ladder), and so is the Wi-Fi profile, if the Pi has one; its plain copy is deleted at the next boot. If `rpi-aws-otp-init` says `blank`, `sudo rpi-aws-otp-init --write` programs OTP key 1 after you type the board's serial: **permanent**. For a plain file key instead, leave out `--store wrapped` ([docs/key-storage.md](../../docs/key-storage.md#plain-file-keys)).
4. Register it, on your computer:

   ```sh
   scp pi@my-pi.local:device.csr .
   rpi-aws-examples/tools/aws-iot-register --csr device.csr
   rpi-aws-examples/tools/gg-core-setup rpi-ex-<id> --classic      # if using a Greengrass image (or --lite)
   scp rpi-ex-*/device.crt rpi-ex-*/config.json pi@my-pi.local:
   ```

5. Install the certificate and start, on the Pi:

   ```sh
   sudo rpi-aws-keygen --install ~
   sudo systemctl enable --now rpi-aws-hello # if using the client image
   sudo rpi-aws-gg-setup                     # if using the Greengrass image: the nucleus is already on the image
   ```

6. Deploy the hello component, on your computer (Greengrass images only). The first deployment takes a few minutes.

   ```sh
   rpi-aws-examples/tools/gg-deploy-hello --classic      # or --lite
   ```

## See it in AWS

- **MQTT:** in the console, **AWS IoT** → **Test** → **MQTT test client**, subscribe to `rpi/#`. A `{"count": n}` arrives on `rpi/rpi-ex-<id>/count` every 10 seconds. Or from your computer: `rpi-aws-examples/tools/aws-iot-watch.py 'rpi/#'`.
- **Greengrass:** **AWS IoT** → **Greengrass devices** → **Core devices** shows the core **Healthy**, and `rpi-ex.Hello` running under its **Components**.
- **Logs (Greengrass):** **CloudWatch** → **Logs** → **Log groups** → `/aws/greengrass/UserComponent/<Region>/rpi-ex.Hello` (Classic) or `/aws/greengrass/rpi-ex-lite` (Lite), stream ending in `rpi-ex-<id>`. Or `aws logs tail <group> --follow`. The group appears with the first batch of lines, a minute or two after the deployment.

## Remove everything

```sh
# on the Pi
sudo systemctl disable --now rpi-aws-hello      # if using the client image
sudo rpi-aws-gg-setup --remove                  # if using the Greengrass image: keeps the nucleus software
sudo rpi-aws-keygen --reset
# on your computer
rpi-aws-examples/tools/gg-core-teardown rpi-ex-<id>      # if using a Greengrass image
rpi-aws-examples/tools/aws-iot-unregister rpi-ex-<id>
rm -rf rpi-ex-<id>                                        # optional: the certificate and settings from registering
```

## Secure boot and factory provisioning

For products, the image is signed and written by [rpi-sb-provisioner](https://github.com/raspberrypi/rpi-sb-provisioner), which can also program OTP key 1 for the key ladder. These examples do not cover it yet: see [docs/further-examples.md](../../docs/further-examples.md).

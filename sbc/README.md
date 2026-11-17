# Raspberry Pi computers: client quick start

A Raspberry Pi 4, Pi 5, CM4, CM5 or Zero 2 W on **Raspberry Pi OS Trixie** (64-bit), sending MQTT messages to AWS IoT Core. The private key is made on the Pi and never leaves it; only a certificate signing request (CSR) goes to your computer. The Pi keeps the key wrapped under a key that only its own firmware can derive, so a copy of the SD card is useless on any other board.

You need:

- the Pi, set up with Raspberry Pi Imager (Raspberry Pi OS Lite is enough), on a network that reaches the internet, with SSH on;
- a computer with bash, `openssl` and the [AWS CLI](https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html), signed in to your AWS account (`aws sts get-caller-identity` works);
- a copy of this repository on both.

In the commands below, `pi@raspberrypi` is your Pi's user and address: Imager's host name with `.local` (for example `pi@rpi-ex-pi4.local`) usually works, or use its IP address. On images made with Imager, `sudo` asks for the password you set there. The tools use the same Region as the AWS CLI: `AWS_REGION` if it is set, otherwise your profile's (`aws configure get region`). Add `--region` to override it.

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

In your account, in your Region, all named `rpi-ex-…`: one **thing** per device (`rpi-ex-<id>`), its **certificate**, and one shared **policy**, `rpi-ex-client`. Nothing else: no servers, no storage. AWS IoT Core charges per connection minute and per message; a few test devices sending a message every 10 seconds cost a few cents a month (see [AWS IoT Core pricing](https://aws.amazon.com/iot-core/pricing/); new accounts have a free tier). Everything is removed by `tools/aws-iot-unregister` (see the end of this page).

To see it all at any time:

```sh
aws iot list-things --query "things[?starts_with(thingName,'rpi-ex-')].thingName"
aws iot list-policies --query "policies[?starts_with(policyName,'rpi-ex-')].policyName"
```

In the console: **AWS IoT** → **All devices** → **Things**, and **Security** → **Certificates** and **Policies**.

## 1. Install (on the Pi)

```sh
sudo apt install -y git
git clone https://github.com/raspberrypi/rpi-aws-examples.git
sudo rpi-aws-examples/sbc/install.sh
```

This adds a service user, `rpi-ex`, the directory `/etc/rpi-aws-examples` (owned by root; `rpi-ex` reads it through its group, and cannot change it), the key tools and the hello client with the [AWS IoT Device SDK for Python](https://github.com/aws/aws-iot-device-sdk-python-v2) in its own Python environment.

## 2. Make the key and its CSR (on the Pi)

The key is wrapped under a key that the firmware derives from **OTP key 1**, a device-unique key in the board's one-time-programmable (OTP) memory: the *key ladder* ([docs/key-storage.md](../docs/key-storage.md)). Check that OTP key 1 is there, without writing anything:

```sh
sudo rpi-aws-otp-init
```

- `programmed`: go on. A Pi 5 or CM5 used with Raspberry Pi Connect already has it; the key ladder only asks the firmware for an HMAC from it, which reveals nothing of the key.
- `blank`: `sudo rpi-aws-otp-init --write` programs it, after you type the board's serial. This is **permanent**: the key stays in the chip for its life. If you would rather not, use a [plain file key](#a-plain-file-key-instead) instead.

Then make the key:

```sh
sudo rpi-aws-keygen --store wrapped > device.csr
```

This makes an EC P-256 key, keeps it wrapped in `/var/lib/rpi-aws-examples/` (root only), and prints a CSR with `CN=rpi-ex-<id>`, where `<id>` is the board's machine ID from the firmware (`/proc/device-tree/chosen/rpi-machine-id`), or its 64-bit serial (`chosen/rpi-serial64`) on firmware too old to have one. That name becomes the thing name in AWS IoT. At every boot `rpi-aws-keyunwrap.service` puts a plain copy in `/run/rpi-aws-examples/` (memory only, never on the card) for the client.

## 3. Register the device (on your computer)

```sh
scp pi@raspberrypi:device.csr .
rpi-aws-examples/tools/aws-iot-register --csr device.csr
```

This creates the thing, a certificate that AWS IoT signs from the CSR, and the policy `rpi-ex-client`, and attaches them. It writes `rpi-ex-<id>/device.crt` and `rpi-ex-<id>/config.json` (thing name, endpoint and Region).

## 4. Install the certificate and start (on the Pi)

```sh
# on your computer
scp rpi-ex-*/device.crt rpi-ex-*/config.json pi@raspberrypi:
# on the Pi
sudo rpi-aws-keygen --install ~
sudo systemctl enable --now rpi-aws-hello
journalctl -u rpi-aws-hello -f
```

`--install` checks that the certificate is for this Pi's key before copying it in. The log shows the connection, then one `hello` and a `count` every 10 seconds.

## 5. See the messages in AWS

**In the console:** open **AWS IoT** → **Test** → **MQTT test client**, in the same Region. Under **Subscribe to a topic**, enter `rpi/#` and choose **Subscribe**. A message arrives on `rpi/rpi-ex-<id>/count` every 10 seconds. The `hello` was sent when the client started: restart it (`sudo systemctl restart rpi-aws-hello`) to see one.

To send the Pi a message, use **Publish to a topic** with the topic `rpi/rpi-ex-<id>/cmd` and any payload. It appears in the Pi's log.

**From your computer:** `tools/aws-iot-watch.py 'rpi/#'` prints the same messages, using your AWS credentials (see [tools/README.md](../tools/README.md)).

## Topics

| Topic | Direction | Payload |
|---|---|---|
| `rpi/<thingName>/hello` | Pi → AWS, once at start | `{"hello": "<thingName>"}` |
| `rpi/<thingName>/count` | Pi → AWS, every 10 s | `{"count": n}` |
| `rpi/<thingName>/cmd` | AWS → Pi | anything; logged |

The policy lets each device use only the topics under its own thing name.

## Where things are

| Path | What | Owner, mode |
|---|---|---|
| `/var/lib/rpi-aws-examples/device.key.wrapped` | The private key, wrapped | root, `0600` |
| `/run/rpi-aws-examples/device.key` | Its plain copy, in memory, made at each boot | root, group `rpi-ex`, `0640` |
| `/etc/rpi-aws-examples/device.crt`, `config.json` | Certificate and settings from `aws-iot-register` | root, group `rpi-ex`, `0640` |
| `/etc/rpi-aws-examples/keystore` | `file` or `wrapped` | |
| `/usr/local/bin/rpi-aws-hello` | The client; `rpi-aws-hello.service` runs it as `rpi-ex` | |

## A plain file key instead

If you cannot or would rather not program OTP key 1 (a board you will hand back, say), make the key in step 2 with `sudo rpi-aws-keygen > device.csr`. It is then a plain file, `/etc/rpi-aws-examples/device.key` (root, group `rpi-ex`, `0640`), and anyone who copies the SD card has it. See [docs/key-storage.md](../docs/key-storage.md#plain-file-keys).

To change the key of a Pi that is already registered, start again: `sudo rpi-aws-keygen --reset` on the Pi, `aws-iot-unregister rpi-ex-<id>` on your computer, then step 2.

## Remove everything

```sh
# on the Pi
sudo systemctl disable --now rpi-aws-hello
sudo rpi-aws-keygen --reset
# on your computer
rpi-aws-examples/tools/aws-iot-unregister rpi-ex-<id>
rm -rf rpi-ex-<id>                                        # optional: the certificate and settings from registering
```

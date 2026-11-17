# Raspberry Pi computers: Greengrass core

Turns a Pi into an [AWS IoT Greengrass](https://docs.aws.amazon.com/greengrass/v2/developerguide/what-is-iot-greengrass.html) core device, and deploys one component, `rpi-ex.Hello`, which publishes a count to MQTT every 10 seconds, logs any message sent to it on MQTT, and writes a log line each time that ends up in CloudWatch Logs.

Two nuclei, one component:

| | Greengrass **Classic** (the Java nucleus) | Greengrass **Nucleus Lite** |
|---|---|---|
| For | Pi 4, Pi 5, CM4, CM5 (1 GB or more) | Zero 2 W and other small boards; also runs on the others |
| Runs as | `greengrass.service`, Java 17 | `greengrass-lite.target`, a set of small C services |
| Logs to CloudWatch with | `aws.greengrass.LogManager` (the component's log file) | `aws.greengrass.SystemLogForwarder` (the component's journal) |
| Log group | `/aws/greengrass/UserComponent/<Region>/rpi-ex.Hello` | `/aws/greengrass/rpi-ex-lite` |

Both use the device key that `rpi-aws-keygen` made, wrapped by the key ladder (see [../README.md](../README.md#2-make-the-key-and-its-csr-on-the-pi)), and the same certificate flow as the client quick start.

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

Besides AWS IoT (the managed policy `AWSIoTFullAccess` covers that part), a Greengrass core needs:

- **IAM**, to create the token-exchange role: `iam:CreateRole`, `GetRole`, `PutRolePolicy`, `ListRolePolicies`, `DeleteRolePolicy`, `DeleteRole`, and `iam:PassRole` on `rpi-ex-gg-tes` (AWS IoT takes the role through the role alias);
- **AWS IoT** role aliases and thing groups: `iot:CreateRoleAlias`, `DescribeRoleAlias`, `DeleteRoleAlias`, `CreateThingGroup`, `DescribeThingGroup`, `DeleteThingGroup`, `AddThingToThingGroup`, `RemoveThingFromThingGroup`, `ListThingsInThingGroup`, and the thing, certificate and policy calls of `aws-iot-register`;
- **Greengrass**: `greengrass:CreateComponentVersion`, `DescribeComponent`, `ListComponentVersions`, `DeleteComponent`, `CreateDeployment`, `ListDeployments`, `CancelDeployment`, `DeleteDeployment`, `ListEffectiveDeployments`, `DeleteCoreDevice` (the managed policy `AWSGreengrassFullAccess` covers them);
- **CloudWatch Logs**, to read and remove the logs: `logs:FilterLogEvents`, `DescribeLogGroups`, `DeleteLogGroup`.

### What gets created, and what it costs

In your account, in your Region, all named `rpi-ex-…`: the core's **thing**, **certificate** and its policy `rpi-ex-gg-<id>` (and the shared `rpi-ex-client`); the **token-exchange role** `rpi-ex-gg-tes` and **role alias** `rpi-ex-gg-tes-alias`; the **thing group** `rpi-ex-gg-classic` or `rpi-ex-gg-lite` and its **deployments**; the **component** `rpi-ex.Hello`; and its **CloudWatch log group**. Greengrass itself has no charge for a few devices in this kind of use (check [AWS IoT Greengrass pricing](https://aws.amazon.com/greengrass/pricing/)); AWS IoT Core messages and CloudWatch Logs ingestion and storage cost a few cents a month for one test device. `tools/gg-core-teardown` and `tools/aws-iot-unregister` remove everything (see the end of this page).

In the console: **AWS IoT** → **Greengrass devices** → **Core devices**, **Components** and **Deployments**; the thing under **All devices** → **Things**; the logs in **CloudWatch** → **Logs** → **Log groups**.

## 1. Install and make the key (on the Pi)

```sh
sudo apt install -y git
git clone https://github.com/raspberrypi/rpi-aws-examples.git
sudo rpi-aws-examples/sbc/install.sh --no-client
sudo rpi-aws-otp-init                               # checks OTP key 1; writes nothing
sudo rpi-aws-keygen --store wrapped > device.csr
```

If `rpi-aws-otp-init` says `blank`, see [step 2 of the client quick start](../README.md#2-make-the-key-and-its-csr-on-the-pi): `--write` programs OTP key 1, permanently. For a plain file key instead, leave out `--store wrapped` ([docs/key-storage.md](../../docs/key-storage.md#plain-file-keys)).

## 2. Register the core (on your computer)

```sh
scp pi@raspberrypi:device.csr .
rpi-aws-examples/tools/aws-iot-register --csr device.csr
rpi-aws-examples/tools/gg-core-setup rpi-ex-<id> --classic      # or --lite
```

`gg-core-setup` creates the token-exchange role `rpi-ex-gg-tes` and its role alias, the core's policy `rpi-ex-gg-<id>`, and the thing group `rpi-ex-gg-classic` (or `-lite`), and adds the Greengrass settings to `rpi-ex-<id>/config.json`. The role lets components write to CloudWatch Logs under `/aws/greengrass/`, each core to its own log streams only, and nothing else.

## 3. Start the nucleus (on the Pi)

```sh
# on your computer
scp rpi-ex-*/device.crt rpi-ex-*/config.json pi@raspberrypi:
# on the Pi
sudo rpi-aws-keygen --install ~
sudo rpi-aws-gg-setup
```

`rpi-aws-gg-setup` installs the nucleus named in `config.json`: the Java runtime and the nucleus (pinned, about 1 minute on a Pi 4), or the Nucleus Lite package (pinned and hash-checked). It configures the nucleus with this Pi's certificate and key and returns once it has connected to AWS IoT. On an image built with this repository's layers, the software is already there.

The core now appears in the console: **AWS IoT** → **Greengrass devices** → **Core devices**, status **Healthy**.

## 4. Deploy the hello component (on your computer)

```sh
rpi-aws-examples/tools/gg-deploy-hello --classic      # or --lite
```

This publishes `rpi-ex.Hello` (its Python code is in [hello/](hello/), embedded in the recipe so that no S3 bucket is needed) and deploys it, with the log component, to the thing group. The first deployment takes a few minutes: the core downloads the log component, and `rpi-ex.Hello` makes its own Python environment with the AWS IoT Device SDK (up to 10 minutes on a Zero 2 W).

Follow it:

```sh
aws greengrassv2 list-effective-deployments --core-device-thing-name rpi-ex-<id>
```

or in the console: **Core devices** → your core → **Deployments**, then the **Components** tab, where `rpi-ex.Hello` shows **Running**.

## 5. See the count and the log

**The count on MQTT.** **AWS IoT** → **Test** → **MQTT test client** → **Subscribe to a topic**: `rpi/#`. A message `{"count": n}` arrives on `rpi/rpi-ex-<id>/count` every 10 seconds. From your computer: `tools/aws-iot-watch.py 'rpi/#'`.

**A message to the Pi.** **Publish to a topic** with the topic `rpi/rpi-ex-<id>/cmd` and any payload; the component logs it as `message on rpi/rpi-ex-<id>/cmd: …`. From your computer: `aws iot-data publish --topic rpi/rpi-ex-<id>/cmd --cli-binary-format raw-in-base64-out --payload '{"hi": 1}'`.

**The log in CloudWatch.** Log lines arrive in batches, about once a minute. The log group is created with the first batch, so for a minute or two after the deployment `aws logs tail` says it does not exist.

- Console: **CloudWatch** → **Logs** → **Log groups** → the group in the table above → the stream ending in `rpi-ex-<id>`. Each line reads `hello from rpi-ex-<id>: published count n to rpi/rpi-ex-<id>/count`.
- CLI: `aws logs tail /aws/greengrass/UserComponent/<Region>/rpi-ex.Hello --follow` (Classic) or `aws logs tail /aws/greengrass/rpi-ex-lite --follow` (Lite).

**On the Pi.** Classic: `sudo tail -f /greengrass/v2/logs/rpi-ex.Hello.log`. Lite: `journalctl -fu ggl.rpi-ex.Hello`.

## Notes

- **Component users.** Classic runs components as `ggc_user`; Lite as `gg_component`. Neither can read the device key. Lite's own services run as `ggcore`, which joins the group `rpi-ex` to read it.
- **Local authorisation.** The recipe lets `rpi-ex.Hello` publish and subscribe under `rpi/*` over IPC; the core's IoT policy then limits the core to `rpi/<its thing name>/` in AWS.
- **Classic waits for the network.** `rpi-aws-gg-setup` adds a drop-in, `/etc/systemd/system/greengrass.service.d/rpi-aws-examples.conf`, that starts the nucleus after `network-online.target`. Without it a fast-booting Pi (a Pi 5) starts the nucleus before DNS works; its first fetch of AWS credentials fails, it tries again only 5 minutes later, and logs reach CloudWatch late.
- **Just after boot** the nucleus refuses a subscription until it has its connection; the component tries again every 10 seconds, so its log may start with one `subscribe … failed`.
- **Redeploying.** `gg-deploy-hello` resets the component's configuration to the recipe's defaults each time. A nucleus keeps a component's configuration across versions, so without the reset a changed default (such as its access control) would not reach a core that already had it.
- **One recipe, both nuclei.** The recipe's platform says `runtime: "*"`. Without it, a manifest applies only to the Java nucleus, and a Lite core fails the deployment with "doesn't claim platform … compatibility". Lite also reads recipe keys case-sensitively: the lifecycle steps are `install` and `run`, in lower case, as in the recipe reference (the Java nucleus accepts `Install` too).

## Remove everything

```sh
# on the Pi
sudo rpi-aws-gg-setup --remove
sudo rpi-aws-keygen --reset
# on your computer
rpi-aws-examples/tools/gg-core-teardown rpi-ex-<id>
rpi-aws-examples/tools/aws-iot-unregister rpi-ex-<id>
rm -rf rpi-ex-<id>                                        # optional: the certificate and settings from registering
```

`gg-core-teardown` removes the core device record, its policy and, once no core is left, the thing groups and their deployments, the role and role alias, the `rpi-ex.Hello` component and its log groups. `rpi-aws-gg-setup --remove` stops the nucleus and deletes the core's configuration and deployed components (`/greengrass/v2` for Classic; `/var/lib/greengrass` and `/etc/greengrass/config.yaml` for Lite), but keeps the nucleus software, so the Pi can be set up again as a new core. To reuse the card for something else, also remove `/opt/greengrass` (Classic) or the `aws-greengrass-lite` package (Lite).

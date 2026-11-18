# Laptop tools

Bash scripts you run on your own computer, with the [AWS CLI](https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html) and your own AWS credentials. They need `aws` and `openssl`; `aws-iot-watch.py` also needs Python.

| Script | Does | Undo with |
|---|---|---|
| `aws-iot-register --csr FILE` | Creates the thing, a certificate signed from the device's CSR, and the `rpi-ex-client` policy, and attaches them. Writes `device.crt` and `config.json` for the device and prints the endpoint | `aws-iot-unregister THING` |
| `aws-iot-watch.py 'rpi/#'` | Prints the MQTT messages on a topic filter, using your AWS credentials (no device certificate). Needs [uv](https://docs.astral.sh/uv/), or Python with `awsiotsdk` | |
| `gg-core-setup THING` | Greengrass core: the token-exchange role, role alias, core policy and thing group | `gg-core-teardown THING` |
| `kvs-setup THING` | Kinesis Video Streams: the thing's video stream, the shared role `rpi-ex-kvs` (each thing may write only its own stream), its role alias and the `rpi-ex-kvs` policy. Writes `kvs.json` for the device | `kvs-teardown THING` |
| `kvs-hls-url THING` | Prints a URL that plays the thing's stream live (in Chrome or Safari, VLC or `ffplay`), using your AWS credentials | |

Everything these scripts create in AWS starts with `rpi-ex-`, so you can always see what is there:

```sh
aws iot list-things --query "things[?starts_with(thingName,'rpi-ex-')].thingName"
aws iot list-policies --query "policies[?starts_with(policyName,'rpi-ex-')].policyName"
aws kinesisvideo list-streams --query "StreamInfoList[?starts_with(StreamName,'rpi-ex-')].StreamName"
```

Every script takes `--region`; without it, the AWS CLI's configured Region is used.

## Set up AWS

1. Install the [AWS CLI v2](https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html) and `openssl` on your computer (Linux, macOS, or WSL on Windows).
2. Sign in with an IAM Identity Center (SSO) user, `aws configure sso` then `aws sso login`, or with an IAM user's access keys, `aws configure`. Do not use the account's root user.
3. Choose a Region near you that has AWS IoT Core and stick to it: `aws configure set region eu-west-2` (for example), or add `--region` to every tool. The console must be in the same Region to show your devices.
4. Check: `aws sts get-caller-identity` prints your account and identity.
5. Get this repository on your computer: `git clone https://github.com/raspberrypi/rpi-aws-examples.git`. The tools are in `rpi-aws-examples/tools/`.

Your identity needs AWS IoT permissions to create and delete things, certificates and policies. The AWS managed policy `AWSIoTFullAccess` covers them all. For a narrower policy, the tools call: `iot:CreateThing`, `DescribeThing`, `DeleteThing`, `CreateCertificateFromCsr`, `DescribeCertificate`, `UpdateCertificate`, `DeleteCertificate`, `CreatePolicy`, `GetPolicy`, `DeletePolicy`, `ListPolicyVersions`, `DeletePolicyVersion`, `AttachPolicy`, `DetachPolicy`, `ListAttachedPolicies`, `ListTargetsForPolicy`, `AttachThingPrincipal`, `DetachThingPrincipal`, `ListThingPrincipals`, `DescribeEndpoint`; and `sts:GetCallerIdentity`. `aws-iot-watch.py` also needs `iot:Connect`, `iot:Subscribe` and `iot:Receive`. Greengrass (`gg-*`) needs more: IAM role creation and Greengrass permissions; see [../sbc/greengrass/README.md](../sbc/greengrass/README.md#permissions). So does Kinesis Video Streams (`kvs-*`); see [../sbc/kvs/README.md](../sbc/kvs/README.md#permissions).

## Policies

The templates in [`policies/`](policies/) are filled in with your Region and account when a script creates them.

- `kvs-device.json` (`rpi-ex-kvs`) lets a certificate ask the AWS IoT credentials provider for the role alias `rpi-ex-kvs-alias`, and nothing else. `kvs-role.json`, the role's own policy, allows describing and writing to `stream/${credentials-iot:ThingName}` only, so one role serves every device. `kvs-trust.json` lets the credentials provider assume the role.
- `client.json` (`rpi-ex-client`, shared by all your client devices): connect only with the client ID equal to the thing name, and only when the certificate is attached to that thing; publish, subscribe and receive only under `rpi/<thingName>/`. It uses [thing policy variables](https://docs.aws.amazon.com/iot/latest/developerguide/thing-policy-variables.html), so one policy serves every device and no device can use another's topics.

None of these policies uses `iot:*` or `Resource: "*"` for MQTT.

## Safe to rerun

`aws-iot-register` reuses an active certificate whose public key matches the CSR, so running it twice does not make a second certificate. `aws-iot-unregister` deactivates and deletes the thing's certificates, deletes the thing, and deletes an `rpi-ex-` policy once nothing else is attached to it.

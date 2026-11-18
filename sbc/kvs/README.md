# Raspberry Pi computers: video to Amazon Kinesis Video Streams

A Raspberry Pi 4, Pi 5, CM4, CM5 or Zero 2 W with a camera streams live video to [Amazon Kinesis Video Streams](https://aws.amazon.com/kinesis/video-streams/) (KVS), and you watch it in the AWS console. It uses the [Kinesis Video Streams Producer SDK for C++](https://github.com/awslabs/amazon-kinesis-video-streams-producer-sdk-cpp)'s GStreamer plugin, `kvssink`.

The Pi uses the identity it already has from the [client quick start](../README.md): its own private key and the certificate AWS IoT signed for it. No AWS access keys go on the Pi. Instead, `kvssink` shows its certificate to the **AWS IoT credentials provider** and gets short-lived AWS credentials for a role that can write only to this Pi's own stream:

```
Pi (key + certificate) ──TLS──▶ AWS IoT credentials provider ──▶ credentials for rpi-ex-kvs (about an hour)
Pi ──H.264 video, signed with those credentials──▶ the stream named after the Pi's thing ──▶ the KVS console
```

You need:

- a Pi set up and registered with the [client quick start](../README.md), steps 0 to 4, but without starting `rpi-aws-hello`. For video alone, install with `sudo rpi-aws-examples/sbc/install.sh --no-client`;
- a camera on the camera connector (a Raspberry Pi Camera Module, or another libcamera camera). Nothing else may use it while the Pi streams: libcamera allows one user at a time;
- the same computer, AWS CLI and Region as in the quick start. Kinesis Video Streams is in most Regions, including `eu-west-2`.

In the commands below, `rpi-ex-<id>` is your Pi's thing name, as printed by `aws-iot-register`.

## 1. Create the stream (on your computer)

```sh
rpi-aws-examples/tools/kvs-setup rpi-ex-<id>
```

This creates:

- **the stream**, named after the thing. It keeps video for 24 hours; change that with `--retention-hours N`, which must be at least 1, because the console plays stored video;
- **the role** `rpi-ex-kvs`, which may only describe the stream named after the calling thing, and write video to it;
- **the role alias** `rpi-ex-kvs-alias`, which the Pi asks the credentials provider for;
- **the policy** `rpi-ex-kvs`, attached to the Pi's certificate, which allows only that request.

The role, alias and policy are shared by all your Pis. Each Pi still reaches only its own stream, because the role's policy names the stream `${credentials-iot:ThingName}`.

It writes `rpi-ex-<id>/kvs.json`: the stream, the Region, your account's credentials-provider endpoint and the role alias.

## 2. Build and install (on the Pi)

Bring the Pi up to date first. The camera packages must match each other, and installing one new package on an old system can update half of the camera stack and break it:

```sh
sudo apt update && sudo apt full-upgrade -y && sudo reboot
```

Then:

```sh
# on your computer
scp rpi-ex-*/kvs.json pi@raspberrypi:
# on the Pi
sudo rpi-aws-examples/sbc/kvs/rpi-aws-kvs-setup ~/kvs.json
```

This installs GStreamer, the camera plugin and the build tools. It then builds `kvssink` from a pinned release of the Producer SDK in `/opt/rpi-aws-examples/kvs`. That takes under a minute on a Pi 5, and much longer on a Zero 2 W, which builds one file at a time to fit in its memory. It also installs the streamer, `rpi-aws-kvs`, and its service. It checks that `kvs.json` is for this Pi before copying it to `/etc/rpi-aws-examples/`.

If it stops because a package would be upgraded, run the update above and try again.

## 3. Start streaming (on the Pi)

```sh
sudo systemctl enable --now rpi-aws-kvs
journalctl -u rpi-aws-kvs -f
```

The log starts with a line like `streaming 1280x720 at 30 fps, 2000 kbit/s (x264) to stream rpi-ex-<id> in eu-west-2`. It then shows `kvssink` fetching credentials and connecting. After that it stays quiet while video flows.

## 4. Watch it

**In the console:**

1. Open **Kinesis Video Streams** in the same Region.
2. Choose **Video streams**, then your stream, `rpi-ex-<id>`.
3. Expand **Media playback**. The live picture appears after a few seconds. If you see `StartTimestamp must be before the current time` instead, rewind a little: set **Time** a few seconds earlier, then play.

To go back in time, use the playback controls, up to the stream's retention.

**From your computer:** `tools/kvs-hls-url rpi-ex-<id>` prints a live HLS URL, using your AWS credentials. Open it in a browser, such as Chrome or Safari, or in VLC (**Media** → **Open Network Stream**) or `ffplay`. It works for an hour (`--expires`, up to 12 hours). Anyone who has it can watch until then.

## Settings

`/etc/rpi-aws-examples/kvs.conf` holds the settings. After changing it, run `sudo systemctl restart rpi-aws-kvs`.

| Setting | Default | |
|---|---|---|
| `WIDTH`, `HEIGHT`, `FRAMERATE` | 1280, 720, 30 | The camera mode |
| `BITRATE_KBPS` | 2000 | H.264 bitrate |
| `ENCODER` | `auto` | `v4l2`, the hardware H.264 encoder, on a Pi 4, CM4 or Zero 2 W. `x264`, in software, on a Pi 5 or CM5, which have no hardware encoder. At 1280x720 and 30 fps, x264 uses about a third of one Pi 5 core |

Every board sends a key frame each second, and `kvssink` starts a new fragment at each one.

## What gets created, and what it costs

In your Region, `kvs-setup` creates:

- **the stream**, `rpi-ex-<id>`, one per Pi;
- **the role** `rpi-ex-kvs` and **the role alias** `rpi-ex-kvs-alias`, shared by all Pis;
- **the policy** `rpi-ex-kvs`, attached to each Pi's certificate.

Kinesis Video Streams charges for the data:

- **ingested:** about 21 GB a day at the default 2000 kbit/s, while the Pi streams;
- **stored:** for the retention period;
- **played back:** whatever you watch.

See [Kinesis Video Streams pricing](https://aws.amazon.com/kinesis/video-streams/pricing/). Stop the service when you are not watching (`sudo systemctl stop rpi-aws-kvs`), and nothing more is charged once the retention period has passed.

To see it all:

```sh
aws kinesisvideo list-streams --query "StreamInfoList[?starts_with(StreamName,'rpi-ex-')].StreamName"
aws iot list-role-aliases --query "roleAliases[?starts_with(@,'rpi-ex-')]"
```

### Permissions

Your identity needs, in addition to the quick start's AWS IoT permissions:

- **Kinesis Video Streams:** `kinesisvideo:CreateStream`, `DescribeStream`, `DeleteStream`, `GetDataEndpoint`, and `GetHLSStreamingSessionURL` (for `kvs-hls-url`).
- **AWS IoT:** `iot:CreateRoleAlias`, `DescribeRoleAlias`, `DeleteRoleAlias`.
- **IAM:** `iam:CreateRole`, `GetRole`, `PutRolePolicy`, `ListRolePolicies`, `DeleteRolePolicy`, `DeleteRole` and `PassRole`, for the role `rpi-ex-kvs`.

The console's Media playback also needs `kinesisvideo:GetHLSStreamingSessionURL`, or the DASH equivalent.

## Where things are

| Path | What |
|---|---|
| `/etc/rpi-aws-examples/kvs.json` | Stream, Region, credentials endpoint and role alias, from `kvs-setup` (root, group `rpi-ex`, `0640`) |
| `/etc/rpi-aws-examples/kvs.conf` | Settings |
| `/opt/rpi-aws-examples/kvs/` | The Producer SDK, its build (`sdk/build/libgstkvssink.so`), `build.log`, and `kvssink`'s log settings |
| `/usr/local/bin/rpi-aws-kvs` | The streamer. `rpi-aws-kvs.service` runs it as `rpi-ex`, which `rpi-aws-kvs-setup` adds to the `video` group for the camera |

The key and certificate are where the quick start put them. With the wrapped key store, `rpi-aws-kvs` uses the plain copy in `/run`.

## When it does not work

- **`not-negotiated` from `libcamerasrc`:** the camera cannot give the mode in `kvs.conf`. Try 1280x720 at 30.
- **`Failed to acquire camera`, or `Device or resource busy`:** something else has the camera, such as `rpicam-hello` or another pipeline. Stop it.
- **HTTP 403 when fetching credentials:** usually a problem with the certificate or the policy. Check that `kvs-setup` ran for this thing, and that the Pi's clock is right (`timedatectl`).
- **HTTP 403 on `DescribeStream` or `PutMedia`:** the stream is not named after the thing, or it is in another Region.
- **The console says there is no media:** the Pi is not streaming yet, or it streams to another Region. Check the log in step 3.

These are harmless: a `WARN` that the "Retention period" from `DescribeStream` doesn't match (an SDK quirk), and libcamera's `Unsupported V4L2 pixel format` warnings.

## Remove everything

```sh
# on the Pi
sudo systemctl disable --now rpi-aws-kvs
# on your computer: the stream (with its video), and the shared role, alias and policy once no Pi uses them
rpi-aws-examples/tools/kvs-teardown rpi-ex-<id>
```

Then, to remove the device itself, follow the quick start's [Remove everything](../README.md#remove-everything). `/opt/rpi-aws-examples/kvs` and the packages stay on the Pi. Remove the folder with `sudo rm -rf /opt/rpi-aws-examples/kvs`.

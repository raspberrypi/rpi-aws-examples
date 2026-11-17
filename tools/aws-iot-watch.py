#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["awsiotsdk==1.31.0"]
# ///
"""Print the MQTT messages on a topic filter, from your computer, with your own AWS credentials.

    tools/aws-iot-watch.py 'rpi/#' [--region eu-west-2] [--seconds 60] [--count N]

Connects to AWS IoT Core over a WebSocket signed with the credentials the AWS CLI is using (no device
certificate), as the client ID rpi-ex-watch-<random>. Your credentials need iot:Connect, iot:Subscribe and
iot:Receive. Runs with uv (https://docs.astral.sh/uv/), or with any Python that has awsiotsdk installed.
"""

import argparse
import json
import os
import secrets
import subprocess
import sys
import threading
import time

from awscrt import auth, mqtt5
from awsiot import mqtt5_client_builder


def cli(*args: str) -> str:
    return subprocess.run(["aws", *args], capture_output=True, text=True, check=True).stdout.strip()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("topic", help="topic filter, for example 'rpi/#'")
    p.add_argument("--region")
    p.add_argument("--seconds", type=float, default=0, help="stop after this long (default: until Ctrl-C)")
    p.add_argument("--count", type=int, default=0, help="stop after this many messages")
    args = p.parse_args()

    # Same order as the CLI: --region, then AWS_REGION and AWS_DEFAULT_REGION, then the profile's region.
    region = (args.region or os.environ.get("AWS_REGION") or os.environ.get("AWS_DEFAULT_REGION")
              or cli("configure", "get", "region"))
    endpoint = cli("iot", "describe-endpoint", "--endpoint-type", "iot:Data-ATS", "--region", region,
                   "--query", "endpointAddress", "--output", "text")
    creds = json.loads(cli("configure", "export-credentials", "--format", "process"))
    provider = auth.AwsCredentialsProvider.new_static(
        creds["AccessKeyId"], creds["SecretAccessKey"], creds.get("SessionToken"))

    done, connected, received = threading.Event(), threading.Event(), [0]

    def on_message(data: mqtt5.PublishReceivedData) -> None:
        packet = data.publish_packet
        stamp = time.strftime("%H:%M:%S")
        print(f"{stamp} {packet.topic} {bytes(packet.payload or b'').decode(errors='replace')}", flush=True)
        received[0] += 1
        if args.count and received[0] >= args.count:
            done.set()

    client = mqtt5_client_builder.websockets_with_default_aws_signing(
        endpoint=endpoint, region=region, credentials_provider=provider,
        client_id=f"rpi-ex-watch-{secrets.token_hex(4)}", on_publish_received=on_message,
        on_lifecycle_connection_success=lambda _: connected.set())
    client.start()
    if not connected.wait(30):
        print("error: could not connect (check your credentials and Region)", file=sys.stderr)
        client.stop()
        return 1
    client.subscribe(mqtt5.SubscribePacket(subscriptions=[
        mqtt5.Subscription(topic_filter=args.topic, qos=mqtt5.QoS.AT_MOST_ONCE)])).result(30)
    print(f"watching {args.topic} on {endpoint}", file=sys.stderr, flush=True)
    try:
        done.wait(args.seconds or None)
    except KeyboardInterrupt:
        pass
    client.stop()
    return 0 if received[0] or not args.count else 1


if __name__ == "__main__":
    sys.exit(main())

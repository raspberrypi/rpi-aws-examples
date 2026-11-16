"""Greengrass component rpi-ex.Hello: publishes {"count": n} to rpi/<thingName>/count every N seconds, over the
nucleus's connection (IPC), and writes one log line per count; logs each message that arrives on rpi/<thingName>/cmd. The same code runs on Greengrass Classic (the
Java nucleus) and on Nucleus Lite. Its log lines reach CloudWatch through the nucleus's log component:
aws.greengrass.LogManager on Classic, aws.greengrass.SystemLogForwarder on Lite.
"""

import json
import os
import sys
import time

from awsiot.greengrasscoreipc.clientv2 import GreengrassCoreIPCClientV2
from awsiot.greengrasscoreipc.model import QOS, IoTCoreMessage


def main() -> int:
    interval = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    thing = os.environ["AWS_IOT_THING_NAME"]  # set by the nucleus
    topic = f"rpi/{thing}/count"
    ipc = GreengrassCoreIPCClientV2()

    def on_cmd(event: IoTCoreMessage) -> None:
        msg = event.message
        print(f"hello from {thing}: message on {msg.topic_name}: {msg.payload.decode(errors='replace')}", flush=True)

    cmd = f"rpi/{thing}/cmd"
    subscribed = False
    n = 0
    while True:
        if not subscribed:
            # Refused until the nucleus has its connection (just after boot, for example): try again each time.
            try:
                ipc.subscribe_to_iot_core(topic_name=cmd, qos=QOS.AT_LEAST_ONCE, on_stream_event=on_cmd)
                subscribed = True
                print(f"hello from {thing}: subscribed to {cmd}", flush=True)
            except Exception as exc:  # noqa: BLE001
                print(f"hello from {thing}: subscribe to {cmd} failed: {exc!r}", flush=True)
        n += 1
        try:
            ipc.publish_to_iot_core(topic_name=topic, qos=QOS.AT_LEAST_ONCE, payload=json.dumps({"count": n}))
            print(f"hello from {thing}: published count {n} to {topic}", flush=True)
        except Exception as exc:  # noqa: BLE001 - keep counting; the nucleus reconnects
            print(f"hello from {thing}: publish {n} failed: {exc!r}", flush=True)
        time.sleep(interval)


if __name__ == "__main__":
    sys.exit(main())

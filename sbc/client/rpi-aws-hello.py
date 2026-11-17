#!/opt/rpi-aws-examples/venv/bin/python3
"""Hello, AWS IoT: publishes "hello" once, then a count every 10 s, and logs messages on the command topic.

Topics, for the thing name in config.json:
    rpi/<thingName>/hello   published once, at start
    rpi/<thingName>/count   {"count": n} every --interval seconds
    rpi/<thingName>/cmd     subscribed; each message is logged

Reads /etc/rpi-aws-examples (or --dir): config.json and device.crt from aws-iot-register, and the private
key that rpi-aws-keygen made. With the wrapped key store the key is the plain copy the key helper puts in
/run/rpi-aws-examples at boot. The server's CA comes from the system trust store (Amazon Root CA 1 is in
ca-certificates).
"""

import argparse
import json
import logging
import signal
import sys
import threading
from concurrent.futures import Future
from pathlib import Path

from awscrt import mqtt5
from awsiot import mqtt5_client_builder

DEFAULT_DIR = Path("/etc/rpi-aws-examples")
RUN_KEY = Path("/run/rpi-aws-examples/device.key")
TIMEOUT_S = 30

log = logging.getLogger("rpi-aws-hello")


def key_path(directory: Path) -> Path:
    """The file store keeps the key beside the certificate; the wrapped store's plain copy is in /run."""
    marker = directory / "keystore"
    store = marker.read_text().strip() if marker.exists() else "file"
    if store == "wrapped":
        if not RUN_KEY.exists():
            sys.exit(f"{RUN_KEY} is missing: is rpi-aws-keyunwrap.service enabled?")
        return RUN_KEY
    return directory / "device.key"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--dir", type=Path, default=DEFAULT_DIR)
    p.add_argument("--interval", type=float, default=10.0, help="seconds between counts (default 10)")
    p.add_argument("--count", type=int, default=0, help="stop after this many counts (default: run until stopped)")
    args = p.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")

    config = json.loads((args.dir / "config.json").read_text())
    thing, endpoint = config["thingName"], config["endpoint"]
    prefix = f"rpi/{thing}"
    stop = threading.Event()
    connected = threading.Event()

    def on_message(data: mqtt5.PublishReceivedData) -> None:
        packet = data.publish_packet
        log.info("message on %s: %s", packet.topic, bytes(packet.payload or b"").decode(errors="replace"))

    def on_success(data: mqtt5.LifecycleConnectSuccessData) -> None:
        log.info("connected to %s as %s", endpoint, thing)
        connected.set()

    def on_failure(data: mqtt5.LifecycleConnectFailureData) -> None:
        log.warning("connection failed: %s", data.exception)

    def on_stopped(data: mqtt5.LifecycleStoppedData) -> None:
        stop.set()

    # The SDK reconnects by itself, with backoff, and keeps the subscription across reconnects.
    client = mqtt5_client_builder.mtls_from_path(
        endpoint=endpoint, cert_filepath=str(args.dir / "device.crt"), pri_key_filepath=str(key_path(args.dir)),
        client_id=thing, on_publish_received=on_message, on_lifecycle_connection_success=on_success,
        on_lifecycle_connection_failure=on_failure, on_lifecycle_stopped=on_stopped)
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())

    client.start()
    if not connected.wait(TIMEOUT_S):
        log.error("no connection within %d s; see docs/troubleshooting.md", TIMEOUT_S)
        client.stop()
        return 1
    subscribe = client.subscribe(mqtt5.SubscribePacket(
        subscriptions=[mqtt5.Subscription(topic_filter=f"{prefix}/cmd", qos=mqtt5.QoS.AT_LEAST_ONCE)]))
    subscribe.result(TIMEOUT_S)
    log.info("subscribed to %s/cmd", prefix)

    def publish(topic: str, body: dict) -> Future:
        payload = json.dumps(body).encode()
        log.info("publish %s %s", topic, payload.decode())
        return client.publish(mqtt5.PublishPacket(topic=topic, payload=payload, qos=mqtt5.QoS.AT_LEAST_ONCE))

    publish(f"{prefix}/hello", {"hello": thing}).result(TIMEOUT_S)
    n = 0
    while not stop.is_set():
        n += 1
        try:
            publish(f"{prefix}/count", {"count": n}).result(TIMEOUT_S)
        except Exception as exc:  # noqa: BLE001 - offline: the SDK reconnects; keep counting
            log.warning("publish failed: %s", exc)
        if args.count and n >= args.count:
            break
        stop.wait(args.interval)

    client.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())

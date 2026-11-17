#!/usr/bin/python3
"""The key ladder's root helper: the only code that asks the firmware for the key-encryption key.

    OTP device key (key 1; the firmware never releases it with lock_device_private_key=1)
      └─ KEK = HMAC-SHA256(OTP key 1, LABEL)           computed by the firmware: rpi-fw-crypto hmac
           └─ AES-256-GCM wrap of the AWS device key    /var/lib/rpi-aws-examples/device.key.wrapped (root, 0600)
                └─ plain copy for TLS, made at boot     /run/rpi-aws-examples/device.key (tmpfs, root:rpi-ex 0640)
           └─ AES-256-GCM wrap of each iwd Wi-Fi profile /var/lib/rpi-aws-examples/iwd/<name>.psk (root, 0600)
                └─ plain copy for iwd, made at boot     /run/rpi-aws-examples/iwd/<name>.psk, which iwd sees
                                                        as /var/lib/iwd (a bind mount in its unit only)

Operations (fixed files, no paths):
    status          is OTP key 1 programmed, and is there a wrapped key
    csr --cn NAME   make and wrap a key if there is none, then print a CSR for it (CN=NAME)
    pubkey          print the wrapped key's public key
    unwrap [--boot] write the plain copies to /run (rpi-aws-keyunwrap.service runs this at boot, with --boot)
    wifi-wrap       wrap the iwd Wi-Fi profiles in /var/lib/iwd too (images use iwd; Raspberry Pi OS from
                    Imager uses NetworkManager, and has none)
    reset           delete the wrapped key and its plain copy; put the Wi-Fi profiles back in plain
                    in /var/lib/iwd (their copies in /run go at the next boot)

The plain Wi-Fi profiles are deleted at the next boot, before iwd starts (unwrap --boot): iwd forgets a
network whose file is deleted while it runs, which would drop the connection.

The wrapped file opens only on the board that made it: a copied card, a backup or the file alone is useless
elsewhere. Root on the running board can still read the /run copy or ask the firmware: see
docs/key-storage.md for what this does and does not protect against. It never writes OTP.
"""

import argparse
import grp
import os
import re
import secrets
import subprocess
import sys
import tempfile
from pathlib import Path

from cryptography import x509
from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.x509.oid import NameOID

# The HMAC label: one constant, so that the same OTP key can serve other uses under other labels.
# Changing it makes existing wrapped keys unreadable.
LABEL = b"AWS_cert_keys:v1"
KEY_ID = 1
KEYS_DIR = Path("/var/lib/rpi-aws-examples")
WRAPPED = KEYS_DIR / "device.key.wrapped"
RUN_DIR = Path("/run/rpi-aws-examples")
RUN_KEY = RUN_DIR / "device.key"
IWD_DIR = Path("/var/lib/iwd")
IWD_WRAPPED = KEYS_DIR / "iwd"
IWD_RUN = RUN_DIR / "iwd"
IWD_DROPIN = Path("/etc/systemd/system/iwd.service.d/rpi-aws-examples.conf")
IWD_DROPIN_TEXT = f"""# rpi-aws-examples: iwd reads its Wi-Fi profiles from the key ladder's plain copies, in RAM.
[Unit]
Wants=rpi-aws-keyunwrap.service
After=rpi-aws-keyunwrap.service

[Service]
BindPaths={IWD_RUN}:{IWD_DIR}
"""
SERVICE_USER = "rpi-ex"  # reads the plain copy through its group, as a Nucleus Lite core does; never owns it
KEY_NOT_SET = 6  # firmware error: the OTP key slot is blank

# Wrapped file: MAGIC | format | len(label) | label | 12-byte nonce | AES-256-GCM(PEM key). The header is the
# additional authenticated data, so a changed header (or another label) is refused.
MAGIC, FORMAT = b"RPKW", 1
NAME_RE = re.compile(r"^[A-Za-z0-9_:-]{1,128}$")


class Error(Exception):
    pass


def firmware_hmac(data: bytes) -> bytes:
    """HMAC-SHA256 keyed with OTP key 1, by the firmware. The KEK comes back on a pipe, never via a file."""
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "label"
        src.write_bytes(data)  # the label: not a secret
        try:
            proc = subprocess.run(["rpi-fw-crypto", "hmac", "--key-id", str(KEY_ID), "--in", str(src)],
                                  capture_output=True, timeout=10, check=False)
        except FileNotFoundError as exc:
            raise Error("rpi-fw-crypto is not installed (sudo apt install rpifwcrypto)") from exc
    if proc.returncode != 0:
        text = proc.stderr.decode(errors="replace")
        m = re.search(r"Last crypto error: (\d+)", text)
        if m and int(m.group(1)) == KEY_NOT_SET:
            raise Error(f"OTP key {KEY_ID} is not programmed on this board, so it cannot use the wrapped key "
                        "store. See rpi-aws-otp-init (the write is permanent).")
        raise Error(f"firmware HMAC failed: {text.strip() or proc.returncode}")
    if len(proc.stdout) != 32:
        raise Error(f"firmware HMAC returned {len(proc.stdout)} bytes, not 32")
    return proc.stdout


def otp_key_programmed() -> bool:
    try:
        firmware_hmac(LABEL)
    except Error as exc:
        if "not programmed" in str(exc):
            return False
        raise
    return True


def wrap(kek: bytes, plaintext: bytes) -> bytes:
    header = MAGIC + bytes([FORMAT, len(LABEL)]) + LABEL
    nonce = secrets.token_bytes(12)
    return header + nonce + AESGCM(kek).encrypt(nonce, plaintext, header)


def unwrap(kek: bytes, blob: bytes, what: Path = WRAPPED) -> bytes:
    if len(blob) < 6 or blob[:4] != MAGIC or blob[4] != FORMAT:
        raise Error(f"{what} is not a wrapped file this helper understands")
    n = blob[5]
    header, nonce, body = blob[:6 + n], blob[6 + n:18 + n], blob[18 + n:]
    if header[6:] != LABEL:
        raise Error(f"{what} was wrapped under another label")
    try:
        return AESGCM(kek).decrypt(nonce, body, header)
    except InvalidTag as exc:
        raise Error(f"{what} does not open on this board (copied from another board, or damaged)") from exc


def atomic_write(path: Path, data: bytes, mode: int, owner: tuple[int, int] | None = None) -> None:
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.")
    try:
        os.fchmod(fd, mode)
        if owner:
            os.fchown(fd, *owner)
        os.write(fd, data)
        os.fsync(fd)
        os.close(fd)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise
    dfd = os.open(path.parent, os.O_DIRECTORY)
    try:
        os.fsync(dfd)
    finally:
        os.close(dfd)


def load_key() -> ec.EllipticCurvePrivateKey:
    pem = unwrap(firmware_hmac(LABEL), WRAPPED.read_bytes())
    return serialization.load_pem_private_key(pem, password=None)


def service_owner() -> tuple[int, int]:
    """root, and the service user's group. The service user must own nothing that root writes into: it could
    plant a link there for root to write, chmod or chown through."""
    try:
        return 0, grp.getgrnam(SERVICE_USER).gr_gid
    except KeyError as exc:
        raise Error(f"the service user {SERVICE_USER} does not exist: run sbc/install.sh first") from exc


def run_dir(path: Path, mode: int, owner: tuple[int, int]) -> None:
    """Make path a real directory with this owner and mode. A link or anything else in its place is removed
    (an older version let the service user own /run/rpi-aws-examples, where it could have planted one)."""
    path.mkdir(mode=mode, exist_ok=True)
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fchown(fd, *owner)
        os.fchmod(fd, mode)
    finally:
        os.close(fd)


def op_csr(cn: str) -> None:
    if not NAME_RE.match(cn):
        raise Error("--cn must be 1-128 letters, digits, '-', '_' or ':'")
    if WRAPPED.exists():
        key = load_key()
    else:
        kek = firmware_hmac(LABEL)  # fails here, before a key is made, if OTP key 1 is blank
        key = ec.generate_private_key(ec.SECP256R1())
        pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                serialization.NoEncryption())
        KEYS_DIR.mkdir(mode=0o700, parents=True, exist_ok=True)
        atomic_write(WRAPPED, wrap(kek, pem), 0o600)
    csr = (x509.CertificateSigningRequestBuilder()
           .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)]))
           .sign(key, hashes.SHA256()))
    sys.stdout.write(csr.public_bytes(serialization.Encoding.PEM).decode())


def wifi_profiles(kek: bytes) -> dict[str, bytes]:
    """The wrapped Wi-Fi profiles, unwrapped: {file name: contents}."""
    if not IWD_WRAPPED.is_dir():
        return {}
    return {f.name: unwrap(kek, f.read_bytes(), f) for f in sorted(IWD_WRAPPED.glob("*.psk"))}


def write_wifi_run(profiles: dict[str, bytes]) -> None:
    run_dir(RUN_DIR, 0o750, service_owner())
    if IWD_RUN.is_symlink():
        IWD_RUN.unlink()
    run_dir(IWD_RUN, 0o700, (0, 0))
    for name, data in profiles.items():
        atomic_write(IWD_RUN / name, data, 0o600)


def op_wifi_wrap() -> None:
    plain = sorted(f for f in IWD_DIR.glob("*.psk") if f.is_file()) if IWD_DIR.is_dir() else []
    if not plain:
        return
    kek = firmware_hmac(LABEL)
    IWD_WRAPPED.mkdir(mode=0o700, parents=True, exist_ok=True)
    profiles = wifi_profiles(kek)
    for f in plain:
        profiles[f.name] = f.read_bytes()
        atomic_write(IWD_WRAPPED / f.name, wrap(kek, profiles[f.name]), 0o600)
    write_wifi_run(profiles)
    IWD_DROPIN.parent.mkdir(parents=True, exist_ok=True)
    atomic_write(IWD_DROPIN, IWD_DROPIN_TEXT.encode(), 0o644)
    subprocess.run(["systemctl", "daemon-reload"], check=False)
    print(f"Wi-Fi: wrapped {', '.join(f.name for f in plain)}; the plain copy in {IWD_DIR} is deleted at the "
          "next boot", file=sys.stderr)


def op_unwrap(boot: bool) -> None:
    if not WRAPPED.exists():
        print("no wrapped key", file=sys.stderr)
        return
    key = load_key()
    profiles = wifi_profiles(firmware_hmac(LABEL))
    if profiles:
        write_wifi_run(profiles)
        if boot:  # iwd has not started yet (rpi-aws-keyunwrap.service is ordered before it)
            for name in profiles:
                (IWD_DIR / name).unlink(missing_ok=True)
        print(f"unwrapped Wi-Fi to {IWD_RUN}", file=sys.stderr)
    owner = service_owner()
    run_dir(RUN_DIR, 0o750, owner)
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                            serialization.NoEncryption())
    atomic_write(RUN_KEY, pem, 0o640, owner)
    print(f"unwrapped to {RUN_KEY}", file=sys.stderr)


def op_reset() -> None:
    # The Wi-Fi profiles go back to plain first, or the board would lose its network at the next boot.
    profiles = wifi_profiles(firmware_hmac(LABEL)) if IWD_WRAPPED.is_dir() and any(IWD_WRAPPED.iterdir()) else {}
    for name, data in profiles.items():
        IWD_DIR.mkdir(mode=0o700, exist_ok=True)
        atomic_write(IWD_DIR / name, data, 0o600)
    if IWD_DROPIN.exists():
        IWD_DROPIN.unlink()
        subprocess.run(["systemctl", "daemon-reload"], check=False)
    # The plain copies in /run stay until the next boot: a running iwd may be reading them.
    for path in [WRAPPED, RUN_KEY, *IWD_WRAPPED.glob("*")]:
        path.unlink(missing_ok=True)
    if IWD_WRAPPED.is_dir():
        IWD_WRAPPED.rmdir()


def main() -> int:
    p = argparse.ArgumentParser(prog="rpi-aws-keyhelper", description="Key ladder operations (root).")
    sub = p.add_subparsers(dest="op", required=True)
    sub.add_parser("csr").add_argument("--cn", required=True)
    sub.add_parser("unwrap").add_argument("--boot", action="store_true")
    for op in ("status", "pubkey", "wifi-wrap", "reset"):
        sub.add_parser(op)
    args = p.parse_args()
    if os.geteuid() != 0:
        print("error: run as root", file=sys.stderr)
        return 1
    try:
        if args.op == "status":
            print(f"otp-key-{KEY_ID}: {'programmed' if otp_key_programmed() else 'blank'}")
            print(f"wrapped-key: {'present' if WRAPPED.exists() else 'none'}")
        elif args.op == "csr":
            op_csr(args.cn)
        elif args.op == "pubkey":
            if not WRAPPED.exists():
                raise Error("no wrapped key")
            sys.stdout.write(load_key().public_key().public_bytes(
                serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo).decode())
        elif args.op == "unwrap":
            op_unwrap(args.boot)
        elif args.op == "wifi-wrap":
            op_wifi_wrap()
        elif args.op == "reset":
            op_reset()
    except (Error, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

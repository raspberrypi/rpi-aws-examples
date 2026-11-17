#!/usr/bin/env python3
"""Write the key ladder's root key into a Pico 2 W's OTP, if it has none. PERMANENT.

    pico/tools/pico-otp-init.py --port /dev/ttyACM0            dry run: what is there, what would be written
    pico/tools/pico-otp-init.py --port /dev/ttyACM0 --write    write it, after you type the Pico's thing name

The Pico must run hello-mqtt built with -DHELLO_KEY_LADDER=ON. If its root is already there (programmed),
this writes nothing, ever. Otherwise --write asks the firmware to write 32 random bytes from the RP2350's
TRNG into one OTP page (page 60 unless the firmware was built with another OTP_ROOT_PAGE), then a marker row.
OTP cannot be erased or rewritten: the root stays for the life of the chip, and that page holds nothing else.
"""

import argparse
import importlib.util
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location("pico_enrol", Path(__file__).with_name("pico-enrol.py"))
pico_enrol = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pico_enrol)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port", required=True, help="the Pico's USB serial port, for example /dev/ttyACM0")
    p.add_argument("--write", action="store_true", help="write the root (asks for confirmation)")
    args = p.parse_args()
    pico = pico_enrol.Pico(args.port)
    thing = [x for x in pico.command("ID") if x.startswith("rpi-ex-")][-1]
    status = [x for x in pico.command("OTP-STATUS") if x.startswith("otp-root:")]
    if not status:
        sys.exit("error: this firmware has no key ladder (build hello-mqtt with -DHELLO_KEY_LADDER=ON)")
    # otp-root: <state> page <n> rows <first>-<marker> stepping A<n>
    fields = status[-1].split()
    state, page, rows, stepping = fields[1], fields[3], fields[5], fields[7]
    print(f"Pico:      {thing} (RP2350 {stepping})")
    print(f"OTP root:  {state} (page {page}, rows {rows})")

    if state == "programmed":
        print("Nothing to do: the root is there, and this tool never writes it again.")
        return 0
    if state != "blank":
        print("The rows hold an interrupted write. This Pico cannot use the key ladder on that page; "
              "nothing will be written.")
        return 1

    print(f"""
--write would make the firmware write, into OTP page {page}:
  rows {rows.split('-')[0]}+: a 256-bit root key from the TRNG (ECC-protected, 16 rows)
  row  {rows.split('-')[1]}: the marker 0x4c31, written after the root
This is PERMANENT: OTP cannot be erased or changed. The page is used for nothing else.""")
    if stepping != "A4":
        print(f"This chip is {stepping}: fine for testing, but before A4 the OTP is not protected against "
              "someone who has the chip (erratum E16). Products use A4.")
    if not args.write:
        print("Dry run: nothing written. Run with --write to write it.")
        return 0

    answer = input(f"Type the Pico's thing name ({thing}) to write the root, or anything else to stop: ")
    if answer.strip() != thing:
        print("Not confirmed: nothing written.")
        return 1
    pico.command(f"OTP-INIT {thing}", timeout=30)
    print("Written. The Pico restarts, makes its key, wraps it under the root and locks the page until reset. "
          "Register it with pico-enrol.py.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

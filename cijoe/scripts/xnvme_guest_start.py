#!/usr/bin/env python3
"""
Start a qemu guest and wait for its login prompt
================================================

The same as cijoe's own qemu.guest_start, except that the time to wait for the
guest is an argument rather than a fixed 180 seconds. A first boot of the
FreeBSD image generates its host keys and lands within seconds of that limit
on an idle host, and past it when another guest is booting alongside.

Retargetable: False
-------------------
"""
import errno
import logging as log
from argparse import ArgumentParser

from cijoe.qemu.wrapper import Guest


def add_args(parser: ArgumentParser):
    parser.add_argument("--guest_name", type=str, help="Name of the qemu guest.")
    parser.add_argument(
        "--timeout",
        type=int,
        default=180,
        help="Seconds to wait for the guest to reach its login prompt.",
    )


def main(args, cijoe):
    """Start a qemu guest and wait for it"""

    if not args.guest_name:
        log.error("missing argument: guest_name")
        return errno.EINVAL

    guest = Guest(cijoe, cijoe.config, args.guest_name)

    err = guest.start()
    if err:
        log.error(f"guest.start() : err({err})")
        return err

    if not guest.is_up(timeout=args.timeout):
        log.error("guest.is_up() : False")
        return errno.EAGAIN

    return 0

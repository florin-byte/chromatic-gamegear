#!/usr/bin/env python3
"""Log the Chromatic's ESP32 console until Ctrl+C.

Usage: python3 log_serial.py [--reset] [--no-reset] [PORT]

By default, keep DTR and RTS low while the serial port is open. The microSD
card can disappear while those control lines are asserted. Use --reset to
restore the old behavior, which asserts the lines when opening the port and
restarts the ESP32. --no-reset is accepted as a compatibility no-op.
"""
import argparse
import datetime
import os
import sys

import serial
from serial.tools import list_ports


def open_serial(port, reset):
    if reset:
        return serial.Serial(port, 115200, timeout=0.2)

    connection = serial.Serial()
    connection.port = port
    connection.baudrate = 115200
    connection.timeout = 0.2
    connection.dtr = False
    connection.rts = False
    connection.open()
    # Some drivers assert DTR while opening, so deassert both lines again.
    connection.dtr = False
    connection.rts = False
    return connection


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?", metavar="PORT", help="serial port (default: detect Chromatic USB 374e:0101)")
    parser.add_argument("--reset", action="store_true", help="assert DTR and RTS on open, restoring the old ESP32-reset behavior")
    parser.add_argument("--no-reset", action="store_true", help="compatibility no-op; DTR and RTS are low by default")
    args = parser.parse_args(argv)

    port = args.port if args.port else next((p.device for p in list_ports.comports() if p.vid == 0x374E and p.pid == 0x0101), None)
    if not port:
        sys.exit('no Chromatic port (USB 374e:0101); give it by hand')
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'serial-%s.log' % datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    print('logging', port, 'to', out, '(Ctrl+C stops)')
    connection = open_serial(port, args.reset)
    with connection as s, open(out, 'a', encoding='utf-8', errors='replace') as f:
        status = 'log_serial: DTR=%d RTS=%d (reset mode: %s)' % (
            bool(s.dtr), bool(s.rts), 'yes' if args.reset else 'no')
        f.write(status + '\n')
        f.flush()
        buf = b''
        try:
            while True:
                buf += s.read(512)
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    t = '%s %s' % (datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3], line.decode('utf-8', 'replace').rstrip('\r'))
                    print(t)
                    f.write(t + '\n')
                    f.flush()
        except KeyboardInterrupt:
            print('stopped:', out)


if __name__ == '__main__':
    main()

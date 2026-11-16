#!/usr/bin/env python3
"""Hardware-free checks for log_serial's DTR/RTS behavior."""
import io
import unittest
from unittest import mock

import log_serial


class FakeSerial:
    instances = []

    def __init__(self, *args, **kwargs):
        self.port = args[0] if args else None
        self.baudrate = args[1] if len(args) > 1 else None
        self.timeout = kwargs.get('timeout')
        self.dtr = True
        self.rts = True
        self.before_open = None
        self.after_open = None
        self.opened = bool(args)
        self.instances.append(self)

    def open(self):
        self.before_open = (self.dtr, self.rts)
        # Model drivers that assert both lines during open().
        self.dtr = True
        self.rts = True
        self.opened = True
        self.after_open = (self.dtr, self.rts)

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        return False

    def read(self, _size):
        raise KeyboardInterrupt


class LogBuffer(io.StringIO):
    def close(self):
        pass


class LogSerialTests(unittest.TestCase):
    def run_logger(self, args):
        FakeSerial.instances = []
        output = LogBuffer()
        with mock.patch.object(log_serial.serial, 'Serial', FakeSerial), \
                mock.patch.object(log_serial, 'open', return_value=output, create=True), \
                mock.patch('builtins.print'):
            log_serial.main(args)
        return FakeSerial.instances[-1], output.getvalue()

    def test_default_lowers_lines_before_and_after_open(self):
        connection, output = self.run_logger(['TESTPORT'])
        self.assertEqual((False, False), connection.before_open)
        self.assertEqual((True, True), connection.after_open)
        self.assertEqual((False, False), (connection.dtr, connection.rts))
        self.assertIn('log_serial: DTR=0 RTS=0 (reset mode: no)', output)

    def test_reset_keeps_lines_asserted(self):
        connection, output = self.run_logger(['--reset', 'TESTPORT'])
        self.assertTrue(connection.opened)
        self.assertEqual((True, True), (connection.dtr, connection.rts))
        self.assertIn('log_serial: DTR=1 RTS=1 (reset mode: yes)', output)

    def test_no_reset_is_accepted_as_no_op(self):
        connection, _output = self.run_logger(['--no-reset', 'TESTPORT'])
        self.assertEqual((False, False), (connection.dtr, connection.rts))


if __name__ == '__main__':
    unittest.main()

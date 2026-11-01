"""Exercise bounded serial teardown with fakes only; never open a COM port."""

import threading
import time
import unittest

from start import close_serial_transport


class FakeSerial:
    def __init__(self):
        self.calls = []
        self.worker_daemons = []
        self.closed = threading.Event()
        self.is_open = True

    def cancel_write(self):
        self.calls.append('cancel_write')
        self.worker_daemons.append(threading.current_thread().daemon)

    def cancel_read(self):
        self.calls.append('cancel_read')
        self.worker_daemons.append(threading.current_thread().daemon)

    def close(self):
        self.calls.append('close')
        self.worker_daemons.append(threading.current_thread().daemon)
        self.is_open = False
        self.closed.set()


class Receiver:
    def __init__(self, calls, alive=False):
        self.calls = calls
        self.alive = alive

    def join(self, timeout):
        self.calls.append('join')
        self.timeout = timeout

    def is_alive(self):
        return self.alive


class SerialCleanupTests(unittest.TestCase):
    def assert_contract(self, result):
        for name in ('serial_close_confirmed', 'serial_receiver_stopped',
                     'serial_cleanup_timeout'):
            self.assertIs(type(result[name]), bool, name)
        self.assertIsInstance(result['serial_cleanup_stage'], str)
        self.assertIsInstance(result['serial_cleanup_errors'], list)
        self.assertTrue(all(isinstance(value, str)
                            for value in result['serial_cleanup_errors']))

    def test_normal_close_cancels_before_join_and_releases_once(self):
        port = FakeSerial()
        receiver = Receiver(port.calls)
        stages = []
        result = close_serial_transport(port, receiver, timeout=.2,
                                        progress=lambda value: stages.append(dict(value)))
        self.assert_contract(result)
        self.assertEqual(port.calls, ['cancel_write', 'cancel_read', 'join', 'close'])
        self.assertTrue(result['serial_close_confirmed'])
        self.assertTrue(result['serial_receiver_stopped'])
        self.assertFalse(result['serial_cleanup_timeout'])
        self.assertEqual(result['serial_cleanup_errors'], [])
        self.assertTrue(port.closed.is_set())
        self.assertTrue(all(port.worker_daemons))
        self.assertTrue(stages)
        for value in stages:
            self.assert_contract(value)
        self.assertFalse(stages[0]['serial_close_confirmed'])
        self.assertTrue(stages[-1]['serial_close_confirmed'])
        emitted = [value['serial_cleanup_stage'] for value in stages]
        cursor = 0
        for stage in ('cancel_write', 'cancel_read', 'waiting-receiver',
                      'closing-port', 'closed'):
            cursor = emitted.index(stage, cursor) + 1
        self.assertEqual(result['serial_cleanup_stage'], 'closed')

    def test_cancel_releases_real_blocked_writer_before_close(self):
        entered = threading.Event()
        released = threading.Event()
        finished = threading.Event()
        port = FakeSerial()
        original_cancel = port.cancel_write
        original_close = port.close

        def write():
            entered.set()
            released.wait(3)
            finished.set()

        def cancel_write():
            original_cancel()
            released.set()

        def close():
            self.assertTrue(finished.is_set(), 'close raced with the active writer')
            original_close()

        port.cancel_write = cancel_write
        port.close = close
        receiver = threading.Thread(target=write, daemon=True)
        try:
            receiver.start()
            self.assertTrue(entered.wait(1))
            result = close_serial_transport(port, receiver, timeout=.2)
            self.assert_contract(result)
            self.assertTrue(result['serial_close_confirmed'])
            self.assertTrue(result['serial_receiver_stopped'])
            self.assertFalse(receiver.is_alive())
            self.assertEqual(port.calls, ['cancel_write', 'cancel_read', 'close'])
        finally:
            released.set()
            receiver.join(1)

    def test_live_receiver_is_reported_and_never_closed_underneath(self):
        entered = threading.Event()
        release = threading.Event()
        port = FakeSerial()

        def remain_busy():
            entered.set()
            release.wait(3)

        receiver = threading.Thread(target=remain_busy, daemon=True)
        try:
            receiver.start()
            self.assertTrue(entered.wait(1))
            started = time.monotonic()
            result = close_serial_transport(port, receiver, timeout=.1)
            self.assertLess(time.monotonic() - started, 1.0)
            self.assert_contract(result)
            self.assertFalse(result['serial_close_confirmed'])
            self.assertFalse(result['serial_receiver_stopped'])
            self.assertTrue(result['serial_cleanup_timeout'])
            self.assertIn(result['serial_cleanup_stage'],
                          ('receiver-still-running', 'waiting-receiver'))
            self.assertNotIn('close', port.calls)
            self.assertTrue(receiver.is_alive())
        finally:
            release.set()
            receiver.join(1)

    def test_blocked_close_returns_unconfirmed_without_retry(self):
        entered = threading.Event()
        release = threading.Event()
        finished = threading.Event()
        port = FakeSerial()
        stages = []

        def close():
            port.calls.append('close')
            self.assertTrue(threading.current_thread().daemon)
            entered.set()
            release.wait(3)
            port.is_open = False
            finished.set()

        port.close = close
        try:
            started = time.monotonic()
            result = close_serial_transport(port, timeout=.1,
                                            progress=lambda value: stages.append(dict(value)))
            self.assertLess(time.monotonic() - started, 1.0)
            self.assertTrue(entered.is_set())
            self.assert_contract(result)
            self.assertFalse(result['serial_close_confirmed'])
            self.assertTrue(result['serial_receiver_stopped'])
            self.assertTrue(result['serial_cleanup_timeout'])
            self.assertEqual(result['serial_cleanup_stage'], 'closing-port')
            self.assertEqual(port.calls.count('close'), 1)
            self.assertEqual(stages[-1]['serial_cleanup_stage'], 'closing-port')
            self.assertTrue(stages[-1]['serial_cleanup_timeout'])
            self.assertFalse(stages[-1]['serial_close_confirmed'])
            snapshot = {**result, 'serial_cleanup_errors': list(result['serial_cleanup_errors'])}
        finally:
            release.set()
            self.assertTrue(finished.wait(1))
        # A late driver return must not rewrite the already-delivered receipt.
        self.assertEqual(result, snapshot)

    def test_cancel_error_does_not_skip_other_cancellation_or_safe_close(self):
        for failing_method in ('cancel_write', 'cancel_read'):
            with self.subTest(failing_method=failing_method):
                port = FakeSerial()

                def fail(name=failing_method):
                    port.calls.append(name)
                    raise OSError('synthetic ' + name + ' failure')

                setattr(port, failing_method, fail)
                result = close_serial_transport(port, Receiver(port.calls), timeout=.2)
                self.assert_contract(result)
                self.assertEqual(port.calls, ['cancel_write', 'cancel_read', 'join', 'close'])
                self.assertTrue(result['serial_close_confirmed'])
                self.assertTrue(result['serial_receiver_stopped'])
                self.assertFalse(result['serial_cleanup_timeout'])
                self.assertTrue(any('synthetic ' + failing_method + ' failure' in value
                                    for value in result['serial_cleanup_errors']))

    def test_close_error_is_not_retried_or_reported_as_release(self):
        port = FakeSerial()

        def fail():
            port.calls.append('close')
            raise OSError('synthetic close failure')

        port.close = fail
        result = close_serial_transport(port, timeout=.2)
        self.assert_contract(result)
        self.assertFalse(result['serial_close_confirmed'])
        self.assertTrue(result['serial_receiver_stopped'])
        self.assertFalse(result['serial_cleanup_timeout'])
        self.assertEqual(port.calls.count('close'), 1)
        self.assertTrue(any('synthetic close failure' in value
                            for value in result['serial_cleanup_errors']))

    def test_close_return_without_releasing_port_is_not_success(self):
        port = FakeSerial()
        port.close = lambda: port.calls.append('close')
        result = close_serial_transport(port, timeout=.2)
        self.assert_contract(result)
        self.assertFalse(result['serial_close_confirmed'])
        self.assertTrue(result['serial_receiver_stopped'])
        self.assertFalse(result['serial_cleanup_timeout'])
        self.assertEqual(result['serial_cleanup_stage'], 'close-unconfirmed')
        self.assertTrue(port.is_open)

    def test_receiver_join_error_does_not_close_underneath(self):
        port = FakeSerial()

        class BrokenReceiver(Receiver):
            def join(self, timeout):
                self.calls.append('join')
                raise RuntimeError('synthetic join failure')

        result = close_serial_transport(port, BrokenReceiver(port.calls, alive=True),
                                        timeout=.2)
        self.assert_contract(result)
        self.assertFalse(result['serial_close_confirmed'])
        self.assertFalse(result['serial_receiver_stopped'])
        self.assertEqual(result['serial_cleanup_stage'], 'failed')
        self.assertEqual(port.calls, ['cancel_write', 'cancel_read', 'join'])
        self.assertTrue(any('synthetic join failure' in value
                            for value in result['serial_cleanup_errors']))

    def test_stalled_cancel_is_bounded_and_does_not_start_unsafe_close(self):
        entered = threading.Event()
        release = threading.Event()
        finished = threading.Event()
        port = FakeSerial()

        def cancel_write():
            port.calls.append('cancel_write')
            entered.set()
            release.wait(3)
            finished.set()

        port.cancel_write = cancel_write
        try:
            started = time.monotonic()
            result = close_serial_transport(port, timeout=.1)
            self.assertLess(time.monotonic() - started, 1.0)
            self.assertTrue(entered.is_set())
            self.assert_contract(result)
            self.assertFalse(result['serial_close_confirmed'])
            self.assertTrue(result['serial_cleanup_timeout'])
            self.assertEqual(result['serial_cleanup_stage'], 'cancel_write')
            self.assertNotIn('close', port.calls)
        finally:
            release.set()
            self.assertTrue(finished.wait(1))


if __name__ == '__main__':
    unittest.main()

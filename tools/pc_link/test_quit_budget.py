"""Virtual-time exit-budget regressions; no real serial port or process control."""

import contextlib
import inspect
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import launch
import start


class Clock:
    def __init__(self):
        self.value = 0.0

    def __call__(self):
        return self.value

    def advance(self, seconds):
        self.value += seconds


class TimedPort:
    def __init__(self, clock, frames=(), step=.25):
        self.clock = clock
        self.frames = list(frames)
        self.step = step
        self.reads = 0

    @property
    def in_waiting(self):
        return 0

    def read(self, count):
        self.reads += 1
        self.clock.advance(self.step)
        if self.frames and self.frames[0][0] <= self.clock():
            return self.frames.pop(0)[1]
        return b''


class BoardQuitBudgetTests(unittest.TestCase):
    def drain(self, frames, clock=None):
        clock = clock or Clock()
        port = TimedPort(clock, frames)
        forwarded = []
        calls = []
        captured = []
        tail = bytearray()
        log = io.BytesIO()
        errors = []
        metrics = {'last_uart_data': None, 'uart_to_gateway_bytes': 0}

        class Link:
            def sendall(self, data):
                forwarded.append((clock(), bytes(data)))

        def send(value, timeout):
            calls.append((value, timeout))
            return {'rc': 0}

        def capture():
            result = start.read_board_exit(port, tail, clock=clock)
            if result['extra']:
                log.write(result['extra'])
                tail.extend(result['extra'])
            captured.append(result)

        closer = start.drain_board_quit(port, Link(), tail, log, send, capture,
                                        errors, metrics=metrics, clock=clock)
        closer.join(1)
        self.assertFalse(closer.is_alive())
        self.assertEqual(calls, [({'action': 'quit'}, 8)])
        self.assertEqual(errors, [])
        self.assertEqual(metrics['uart_to_gateway_bytes'],
                         sum(len(value) for _, value in forwarded))
        return clock, forwarded, captured, log.getvalue(), port

    def test_default_constants_and_parameters_cover_board_and_host_cleanup(self):
        self.assertEqual(start.BOARD_QUIT_WAIT_SECONDS, 90)
        self.assertEqual(inspect.signature(start.drain_board_quit).parameters['timeout'].default,
                         start.BOARD_QUIT_WAIT_SECONDS)
        self.assertEqual(launch.STOP_SESSION_WAIT_SECONDS, 240)
        self.assertEqual(inspect.signature(launch.stop_session).parameters['wait_seconds'].default,
                         launch.STOP_SESSION_WAIT_SECONDS)
        self.assertGreaterEqual(launch.STOP_SESSION_WAIT_SECONDS,
                                start.BOARD_QUIT_WAIT_SECONDS + 12 + 25 + 25 + 25 + 35)

    def test_fifty_second_split_exit_is_confirmed_while_ppp_keeps_forwarding(self):
        first = b'~synthetic-ppp-before-stop~'
        late = b'~synthetic-ppp-during-cleanup~'
        clock, forwarded, captured, logged, _ = self.drain([
            (1, first), (48, late), (50, b'PC_LINK finished rc='),
            (50.25, b'0; routes and DNS restored;'), (50.5, b' console restored.\n')])
        self.assertEqual([value for _, value in forwarded], [first, late])
        self.assertGreaterEqual(forwarded[-1][0], 48)
        self.assertEqual(len(captured), 1)
        result = captured[0]
        self.assertTrue(result['seen']); self.assertEqual(result['rc'], 0)
        self.assertTrue(result['line'].endswith('console restored.'))
        start.validate_board_exit(result['seen'], result['rc'], True)
        self.assertEqual(logged, first + late +
                         b'PC_LINK finished rc=0; routes and DNS restored; console restored.\n')
        self.assertGreaterEqual(clock(), 50.5)
        self.assertLess(clock(), start.BOARD_QUIT_WAIT_SECONDS)

    def test_late_negative_exit_code_remains_a_real_failure(self):
        clock, forwarded, captured, _, _ = self.drain([
            (49, b'~synthetic-ppp~'), (55, b'PC_LINK finished rc='),
            (55.25, b'-110; cleanup timed out;'), (55.5, b' console restored.\n')])
        self.assertEqual([value for _, value in forwarded], [b'~synthetic-ppp~'])
        self.assertEqual(len(captured), 1)
        result = captured[0]
        self.assertTrue(result['seen']); self.assertEqual(result['rc'], -110)
        with self.assertRaisesRegex(RuntimeError, '-110'):
            start.validate_board_exit(result['seen'], result['rc'], True)
        self.assertGreaterEqual(clock(), 55.5)

    def test_ninety_seconds_without_marker_never_invents_confirmation(self):
        clock, forwarded, captured, logged, _ = self.drain([
            (20, b'~first~'), (85, b'~last-before-deadline~')])
        self.assertEqual(clock(), 90)
        self.assertEqual(captured, [])
        self.assertEqual([value for _, value in forwarded], [b'~first~', b'~last-before-deadline~'])
        self.assertNotIn(b'PC_LINK finished', logged)
        with self.assertRaises(RuntimeError):
            start.validate_board_exit(False, None, True)

    def test_unterminated_result_has_a_separate_bounded_line_drain(self):
        clock = Clock()
        port = TimedPort(clock, [(3, b'; too-late newline\n')])
        result = start.read_board_exit(port, b'PC_LINK finished rc=0', clock=clock)
        self.assertEqual(clock(), 2)
        self.assertEqual(port.reads, 8)
        self.assertTrue(result['seen'])
        self.assertIsNone(result['rc']); self.assertIsNone(result['line'])
        self.assertEqual(result['extra'], b'')
        with self.assertRaises(RuntimeError):
            start.validate_board_exit(result['seen'], result['rc'], True)


class LauncherStopBudgetTests(unittest.TestCase):
    def run_stop(self, phase, delay):
        clock = Clock()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            session = root / 'synthetic-session'
            session.mkdir()
            path = session / 'status.json'
            path.write_text('{"phase":"stopping-board"}', encoding='utf-8')
            reads = []

            def read_status(target):
                self.assertEqual(target, path)
                reads.append(clock())
                return {'phase': phase if clock() >= delay else 'stopping-board'}

            output = io.StringIO()
            with patch.object(launch, 'active_sessions', return_value=[path]), \
                 patch.object(launch, 'read_status', side_effect=read_status), \
                 patch.object(launch.time, 'monotonic', clock), \
                 patch.object(launch.time, 'sleep', clock.advance), \
                 contextlib.redirect_stdout(output):
                try:
                    result = launch.stop_session(root)
                except RuntimeError as error:
                    result = error
            self.assertTrue((session / 'stop.request').exists())
            return clock(), result, output.getvalue(), reads

    def test_default_wait_accepts_stopped_after_sixty_seconds(self):
        elapsed, result, output, reads = self.run_stop('stopped', 75)
        self.assertEqual(result, 0)
        self.assertGreaterEqual(elapsed, 75)
        self.assertLess(elapsed, 75.3)
        self.assertGreater(len(reads), 300)
        self.assertIn('会话已正常结束', output)

    def test_default_wait_preserves_failed_result_after_sixty_seconds(self):
        elapsed, result, _, _ = self.run_stop('failed', 80)
        self.assertIsInstance(result, RuntimeError)
        self.assertIn('清理或连接错误', str(result))
        self.assertNotIn('尚未收到结束确认', str(result))
        self.assertGreaterEqual(elapsed, 80)
        self.assertLess(elapsed, 80.3)

    def test_default_wait_still_expires_without_claiming_completion(self):
        elapsed, result, output, _ = self.run_stop('stopped', 300)
        self.assertIsInstance(result, RuntimeError)
        self.assertIn('尚未收到结束确认', str(result))
        self.assertGreaterEqual(elapsed, 240)
        self.assertLessEqual(elapsed, 240.21)
        self.assertNotIn('会话已正常结束', output)

    def test_immediately_stopped_session_does_not_wait_out_the_budget(self):
        elapsed, result, _, reads = self.run_stop('stopped', 0)
        self.assertEqual(result, 0)
        self.assertEqual(elapsed, 0)
        self.assertEqual(reads, [0])


if __name__ == '__main__':
    unittest.main()

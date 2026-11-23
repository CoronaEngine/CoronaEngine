"""The local CDP endpoint may pause while the engine prepares render resources."""
import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vision_frontend_probe import Frontend


class FrontendDiscoveryTests(unittest.TestCase):
    def test_discovery_allows_the_same_startup_stall_as_websocket_connection(self):
        target = dict(type='page', id='engine', url='file:///CabbageEditor/Frontend/index.html',
                      webSocketDebuggerUrl='ws://127.0.0.1:9222/devtools/page/engine')
        def slow_endpoint(url, timeout):
            # Model a five-second resource-preparation stall without sleeping.
            if timeout < 5:
                raise TimeoutError('engine resource preparation exceeded discovery budget')
            return io.StringIO(json.dumps([target]))
        with patch('vision_frontend_probe.urlopen', side_effect=slow_endpoint), \
                patch('vision_frontend_probe.websocket.create_connection') as connect, \
                patch.object(Frontend, 'call'):
            try:
                Frontend(target_id='engine')
            except TimeoutError as error:
                self.fail(str(error))
        self.assertEqual(connect.call_args.kwargs['timeout'], 20)

    def test_discovery_still_rejects_a_non_engine_document(self):
        target = dict(type='page', id='other', url='file:///other/index.html')
        with patch('vision_frontend_probe.urlopen', return_value=io.StringIO(json.dumps([target]))), \
                patch('vision_frontend_probe.websocket.create_connection') as connect:
            with self.assertRaisesRegex(RuntimeError, 'does not belong'):
                Frontend(target_id='other')
            connect.assert_not_called()

    def test_discovery_timeout_is_reported_without_relaunch_or_retry(self):
        with patch('vision_frontend_probe.urlopen', side_effect=TimeoutError('still stalled')) as request:
            with self.assertRaisesRegex(TimeoutError, 'still stalled'):
                Frontend(target_id='engine')
            self.assertEqual(request.call_count, 1)


if __name__ == '__main__':
    unittest.main()

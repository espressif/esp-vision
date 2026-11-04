# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pathlib
import socket
import sys
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from preview_tcp import FrameParser, encode_frame
from preview_tcp import PreviewObserver


class PreviewTcpHostTest(unittest.TestCase):
    def test_observer_subscribes_without_acquiring_and_accepts_fragmented_preview(self):
        client, server = socket.socketpair()
        errors = []

        def serve():
            try:
                parser = FrameParser()
                requests = []
                while not requests:
                    requests = parser.feed(server.recv(1024))
                request = requests[0]
                self.assertEqual(request.metadata['method'], 'preview.subscribe')
                self.assertNotIn('sessionId', request.metadata)
                self.assertNotIn('vmEpoch', request.metadata)
                self.assertEqual(request.json(), {})
                response = encode_frame(dict(request.metadata, type='rsp'), b'{"ok":true}')
                preview = encode_frame({'channel': 'preview.frame', 'type': 'data',
                                        'contentType': 'image/jpeg', 'seq': 7}, b'jpeg-data')
                for byte in response + preview:
                    server.sendall(bytes([byte]))
            except BaseException as exc:
                errors.append(exc)
            finally:
                server.close()

        worker = threading.Thread(target=serve)
        worker.start()
        observer = None
        try:
            with mock.patch('preview_tcp.socket.create_connection', return_value=client):
                observer = PreviewObserver('selected-device')
            self.assertTrue(observer.subscribe()['ok'])
            frame = None
            while frame is None:
                frame = observer.receive()
            self.assertEqual(frame.metadata['seq'], 7)
            self.assertEqual(frame.payload, b'jpeg-data')
            self.assertEqual(observer.parser.bad_frames, 0)
        finally:
            client.close()
            worker.join(timeout=3)
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])

    def test_rejected_subscription_is_reported(self):
        observer = object.__new__(PreviewObserver)
        observer.request = mock.Mock(return_value={'ok': False, 'error': {'code': 'OBSERVER_LIMIT'}})
        with self.assertRaisesRegex(RuntimeError, 'OBSERVER_LIMIT'):
            observer.subscribe()


if __name__ == '__main__':
    unittest.main()

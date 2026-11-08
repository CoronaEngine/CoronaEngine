"""LANChat retirement must not remove native collaboration contracts."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LanChatRemovalTests(unittest.TestCase):
    def test_production_sources_do_not_include_chat_protocol_or_storage(self):
        for folder in ('include/corona/systems/network', 'src/systems/network',
                       'src/systems/ui/cef', 'src/systems/ui/editor_api'):
            for path in (ROOT / folder).rglob('*'):
                if path.suffix not in ('.h', '.cpp', '.txt'):
                    continue
                with self.subTest(path=str(path.relative_to(ROOT))):
                    self.assertIsNone(re.search(r'lan_?chat|CHAT_(?:JOIN|MESSAGE|AGENT|HISTORY|LEAVE|MEMBER)',
                                                path.read_text(encoding='utf-8-sig'), re.I))

    def test_native_network_contract_and_host_disconnect_remain(self):
        header = (ROOT / 'include/corona/systems/network/network_system.h').read_text(encoding='utf-8')
        for method in ('start_session', 'stop_session', 'start_discovery', 'connect_to_peer',
                       'broadcast_actor_create', 'broadcast_actor_transform_update',
                       'register_actor_identity', 'claim_actor_ownership', 'lock_object'):
            self.assertIn(method + '(', header)
        source = (ROOT / 'src/systems/network/network_system.cpp').read_text(encoding='utf-8')
        self.assertIn('is_connected_host_peer(info)', source)
        self.assertIn('stop_session_after_peer_disconnect = true', source)
        protocol = (ROOT / 'include/corona/systems/network/protocol.h').read_text(encoding='utf-8')
        self.assertRegex(protocol, r'EDITOR_SYNC\s*=\s*0x[0-9A-Fa-f]+')


if __name__ == '__main__':
    unittest.main()

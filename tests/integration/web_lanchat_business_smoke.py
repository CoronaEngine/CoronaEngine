"""Real LANChat store/native business checks inside two runner-owned editors.

This phase covers discovery, messages, history, roles, explicit leave/rejoin,
and native transport shutdown followed by explicit room restart/rejoin.
Optional object checks exercise named scene bindings over the real native network.
Packet loss and automatic reconnect are not simulated.
"""
import json
import socket
import uuid

from web_environment_smoke import wait


STORE = 'window.__lanBusiness'


def run(launches, *, object_sync=False):
    host, guest = launches
    room = 'acceptance-' + uuid.uuid4().hex
    texts = [room + '-before-join', room + '-guest', room + '-host']
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]

    def action(launch, name, *args):
        return launch['main'].evaluate(f'{STORE}.{name}(' + ','.join(json.dumps(arg) for arg in args) + ')')

    def state(launch):
        return launch['main'].evaluate(f'({{role:{STORE}.state.role,inRoom:{STORE}.state.inRoom,'
            f'connection:{STORE}.state.connection,error:{STORE}.state.error,room:{STORE}.state.room,'
            f'peerId:{STORE}.state.peerId,members:{STORE}.state.members,'
            f'messages:{STORE}.state.messages.map(m=>({{text:m.text,id:m.message_id,seq:m.seq}}))}})')

    def received(launch, expected):
        result = wait(lambda: state(launch),
                      lambda s: all(sum(m['text'] == text for m in s['messages']) == 1 for text in expected),
                      'real LANChat messages reach ' + ('host' if launch is host else 'guest'), seconds=40)
        selected = [m for m in result['messages'] if m['text'] in expected]
        assert len({m['id'] for m in selected}) == len(expected), selected
        assert [m['seq'] for m in selected] == sorted(m['seq'] for m in selected), selected
        return result

    initialized = []
    try:
        for launch in launches:
            # Replication resolves scene assets relative to this root. Use the
            # actual runner-owned scene project for object tests; chat-only
            # tests keep separate history roots for the two processes.
            root = (launch['project_root'] if object_sync else launch['root'] / 'lanchat-project').resolve()
            root.mkdir(exist_ok=object_sync)
            launch['lanchat_root'] = root
            launch['main'].evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
              const store=Object.values(m).find(v=>typeof v?.openRoom==='function'&&typeof v?.finishWorldSession==='function'&&v.state);
              if(!store)throw Error('production LANChat store export missing');window.__lanBusiness=store;return true;})()""")
            initialized.append(launch)
            assert not launch['main'].evaluate('corona.network.getSessionInfo()')['active'], 'unexpected preexisting session in owned editor'
            launch['main'].evaluate('corona.network.setProjectRoot(' + json.dumps(str(root)) + ')')
        seeded_actor = None
        if object_sync:
            source = write_replication_source(host['lanchat_root'])
            scope = host['main'].evaluate('corona.scene.readActive()')['archive_session']
            seeded_actor = host['main'].evaluate('corona.scene.createActor("scene.ini",' + json.dumps(str(source)) + ','
                + json.dumps({'archive_session': scope, 'actor_type': 'model', 'actor_name': 'before-room-' + uuid.uuid4().hex}) + ')')
        opened = action(host, 'openRoom', {'room': room, 'port': port, 'nickname': 'Acceptance Host', 'mode': 'multi'})
        assert opened.get('ok'), opened
        host_state = state(host)
        assert host_state['role'] == 'host' and host_state['inRoom'] and host_state['connection'] == 'connected', host_state
        guest['main'].evaluate('corona.network.clearDiscoveredPeers()')
        guest['main'].evaluate('corona.network.searchLan()')
        discovered = wait(lambda: guest['main'].evaluate('corona.network.getDiscoveredPeers()'),
                          lambda result: any(p.get('role') == 'host' and p.get('port') == port and p.get('name') == 'Acceptance Host'
                                             for p in result.get('peers', [])),
                          'real LAN discovery identifies the test-owned host', seconds=40)
        sent = action(host, 'sendMessage', texts[0])
        assert sent.get('ok'), sent
        received(host, texts[:1])

        def join():
            joined = action(guest, 'joinRoom', {'ip': '127.0.0.1', 'port': port, 'room': room, 'nickname': 'Acceptance Guest'})
            assert joined.get('ok'), joined
            return wait(lambda: state(guest), lambda s: s['role'] == 'guest' and s['inRoom'] and s['connection'] == 'connected',
                        'actual LANChat guest history synchronization', seconds=40)

        joined = join()
        received(guest, texts[:1])
        wait(lambda: state(host), lambda s: len(s['members']) == 2, 'native member update reaches host store')
        for launch, text in [(guest, texts[1]), (host, texts[2])]:
            sent = action(launch, 'sendMessage', text)
            assert sent.get('ok'), sent
        host_messages = received(host, texts)
        guest_messages = received(guest, texts)
        assert [m for m in host_messages['messages'] if m['text'] in texts] == [m for m in guest_messages['messages'] if m['text'] in texts]
        sessions = [launch['main'].evaluate('corona.network.getSessionInfo()') for launch in launches]
        assert all(info['active'] for info in sessions), sessions
        assert [info['role'] for info in sessions] == ['host', 'client'], sessions
        seed_index = None
        if seeded_actor:
            guest_scope = guest['main'].evaluate('corona.scene.readActive()')['archive_session']
            seed_index = wait(lambda: guest['main'].evaluate('corona.scene.readActorIndex("scene.ini",'
                + json.dumps({'archive_session': guest_scope}) + ')'),
                lambda result: any(actor['actor_guid'] == seeded_actor['actor_guid'] for actor in result['actors']),
                'Actor created before opening the room reaches the joining guest', seconds=15)
        replication = verify_object_sync(host, guest) if object_sync else None
        if replication:
            replication['initialSeedGuid'] = seeded_actor['actor_guid']
            replication['initialSeedIndex'] = seed_index

        action(guest, 'leaveRoom')
        left = state(guest)
        assert left['role'] == 'none' and not left['inRoom'], left
        wait(lambda: state(host), lambda s: len(s['members']) == 1, 'guest departure reaches host store')
        offline_actor = None
        if object_sync:
            offline_actor = host['main'].evaluate('corona.scene.createActor("scene.ini",'
                + json.dumps(str(host['lanchat_root'] / 'replication-triangle.ply')) + ','
                + json.dumps({'archive_session': replication['hostArchive'], 'actor_type': 'model',
                              'actor_name': 'offline-' + uuid.uuid4().hex}) + ')')
        rejoined = join()
        received(guest, texts)
        if offline_actor:
            replication['rejoinIndex'] = wait(lambda: guest['main'].evaluate('corona.scene.readActorIndex("scene.ini",'
                + json.dumps({'archive_session': replication['guestArchive']}) + ')'),
                lambda result: any(actor['actor_guid'] == offline_actor['actor_guid'] for actor in result['actors']),
                'late rejoin restores an Actor created while the guest was offline', seconds=15)
            replication['offlineCreatedGuid'] = offline_actor['actor_guid']
            assert not any(actor['actor_guid'] == replication['deletedGuid']
                           for actor in replication['rejoinIndex']['actors']), replication
        summaries = wait(lambda: action(host, 'refreshHistoryRooms'),
                         lambda result: result.get('ok') and any(r.get('display_room_id') == room and r.get('message_count', 0) >= 3
                                                               for r in result.get('rooms', [])),
                         'native persisted history available through named V8', seconds=40)
        summary = next(r for r in summaries['rooms'] if r.get('display_room_id') == room)
        history = action(host, 'loadHistoryRoom', summary['room_id'])
        assert history.get('ok'), history
        assert all(sum(m.get('text') == text for m in history.get('history', [])) == 1 for text in texts), history
        action(host, 'closeRoom')
        closed = wait(lambda: state(guest), lambda s: not s['inRoom'] and s['role'] == 'none' and s['error'] == 'ROOM_CLOSED',
                      'host room closure reaches guest native event subscription', seconds=40)
        wait(lambda: guest['main'].evaluate('corona.network.getSessionInfo()'),
             lambda value: not value['active'], 'closed guest native session retired', seconds=40)

        # Exercise the real PeerDisconnected route, bypassing LANChat's explicit
        # closeRoom/leaveRoom packet. Stop only this runner-owned native host.
        reopened = action(host, 'openRoom', {'room': room, 'port': port, 'nickname': 'Acceptance Host', 'mode': 'multi'})
        assert reopened.get('ok'), reopened
        join()
        guest_stdout = guest['root'] / 'stdout.log'
        log_offset = guest_stdout.stat().st_size
        host['main'].evaluate('corona.network.stopSession()')
        disconnected = []
        inactive_sessions = []
        for launch in launches:
            disconnected.append(wait(lambda: state(launch),
                lambda s: s['role'] == 'none' and not s['inRoom'] and s['connection'] == 'idle'
                          and s['error'] == 'ROOM_CLOSED' and not s['room'] and not s['peerId']
                          and not s['members'] and not s['messages'],
                'native transport disconnect clears the actual LANChat store', seconds=40))
            inactive_sessions.append(wait(lambda: launch['main'].evaluate('corona.network.getSessionInfo()'),
                lambda value: not value['active'], 'disconnected native session retired', seconds=40))
        disconnect_log = wait(lambda: guest_stdout.read_bytes()[log_offset:].decode('utf-8', errors='replace'),
            lambda value: 'NetworkSystem: Peer disconnected' in value,
            'guest executes the real native PeerDisconnected callback', seconds=40)
        transport_actor = None
        if object_sync:
            transport_actor = host['main'].evaluate('corona.scene.createActor("scene.ini",'
                + json.dumps(str(host['lanchat_root'] / 'replication-triangle.ply')) + ','
                + json.dumps({'archive_session': replication['hostArchive'], 'actor_type': 'model',
                              'actor_name': 'transport-offline-' + uuid.uuid4().hex}) + ')')
        restarted = action(host, 'openRoom', {'room': room, 'port': port, 'nickname': 'Acceptance Host', 'mode': 'multi'})
        assert restarted.get('ok'), restarted
        guest['main'].evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))')
        awaiting_join = state(guest)
        assert awaiting_join == disconnected[1], awaiting_join
        assert not guest['main'].evaluate('corona.network.getSessionInfo()')['active']
        manual_rejoin = join()
        recovery_text = room + '-manual-rejoin'
        assert action(guest, 'sendMessage', recovery_text).get('ok')
        received(host, [recovery_text])
        received(guest, [recovery_text])
        recovered_index = None
        if transport_actor:
            recovered_index = wait(lambda: guest['main'].evaluate('corona.scene.readActorIndex("scene.ini",'
                + json.dumps({'archive_session': replication['guestArchive']}) + ')'),
                lambda value: any(actor['actor_guid'] == transport_actor['actor_guid'] for actor in value['actors']),
                'manual rejoin receives an Actor created during native transport downtime', seconds=15)
        transport_disconnect = dict(closedStates=disconnected, inactiveSessions=inactive_sessions,
            peerDisconnectedLog=[line for line in disconnect_log.splitlines() if 'NetworkSystem: Peer disconnected' in line],
            guestWhileHostRestarted=awaiting_join, explicitRejoinRole=manual_rejoin['role'], recoveryMessage=recovery_text,
            offlineCreatedGuid=transport_actor['actor_guid'] if transport_actor else None, recoveredIndex=recovered_index,
            scope='native ENet host stop, real peer disconnect, explicit restart/rejoin; no packet-loss or automatic-retry simulation')
        action(host, 'closeRoom')
        wait(lambda: state(guest), lambda s: not s['inRoom'] and s['role'] == 'none' and s['error'] == 'ROOM_CLOSED',
             'restarted room closure reaches guest', seconds=40)
        persisted = list((host['lanchat_root'] / 'Saved/LANChat/history').glob('*'))
        assert persisted, 'history must persist only beneath the test-owned network project root'
        return dict(room=room, messages=texts, hostRole=host_state['role'], guestRole=joined['role'],
                    discoveredHost=[p for p in discovered['peers'] if p.get('port') == port and p.get('role') == 'host'],
                    synchronizedMessages=host_messages['messages'], nativeSessions=sessions, explicitLeave=left,
                    explicitRejoinRole=rejoined['role'], persistedHistory=summary, roomClosed=closed,
                    testOwnedHistoryRoot=str(host['lanchat_root']),
                    objectReplication=replication, transportDisconnect=transport_disconnect,
                    scope='discovery/chat/history/roles/explicit leave-rejoin'
                          + ('/object seed/create/delete, bidirectional transforms/properties/rename and offline-create rejoin'
                             if object_sync else '; object replication not covered')
                          + '/native transport stop and explicit restart-rejoin; packet loss and automatic reconnect not covered')
    finally:
        for launch in reversed(initialized):
            # Every process and session here was created by the enclosing runner.
            try:
                action(launch, 'finishWorldSession')
            finally:
                launch['main'].evaluate('corona.network.stopSession()')


def write_replication_source(root):
    source = root / 'replication-triangle.ply'
    source.write_text('ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\n'
                      'property float y\nproperty float z\nelement face 1\n'
                      'property list uchar int vertex_indices\nend_header\n'
                      '0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n', encoding='ascii')
    return source


def verify_object_sync(host, guest):
    source = write_replication_source(host['lanchat_root'])
    name = 'replication-' + uuid.uuid4().hex
    scopes = [launch['main'].evaluate('corona.scene.readActive()')['archive_session'] for launch in (host, guest)]
    created = host['main'].evaluate('corona.scene.createActor("scene.ini",' + json.dumps(str(source)) + ','
        + json.dumps({'archive_session': scopes[0], 'actor_type': 'model', 'actor_name': name}) + ')')
    assert created['name'] == name and created['archive_session'] == scopes[0], created
    print('Native object created on host: ' + created['actor_guid'], flush=True)
    index = wait(lambda: guest['main'].evaluate('corona.scene.readActorIndex("scene.ini",'
                 + json.dumps({'archive_session': scopes[1]}) + ')'),
                 lambda result: any(actor['actor_guid'] == created['actor_guid'] for actor in result['actors']),
                 'host-created Actor reaches guest with the same stable GUID', seconds=15)
    removed = host['main'].evaluate('corona.scene.createActor("scene.ini",' + json.dumps(str(source)) + ','
        + json.dumps({'archive_session': scopes[0], 'actor_type': 'model', 'actor_name': name + '-delete'}) + ')')
    wait(lambda: guest['main'].evaluate('corona.scene.readActorIndex("scene.ini",'
         + json.dumps({'archive_session': scopes[1]}) + ')'),
         lambda result: any(actor['actor_guid'] == removed['actor_guid'] for actor in result['actors']),
         'deletion target first materializes on the guest', seconds=15)
    host['main'].evaluate('corona.scene.deleteActor("scene.ini",' + json.dumps(removed['actor_guid']) + ','
        + json.dumps({'archive_session': scopes[0]}) + ')')
    deleted_index = wait(lambda: guest['main'].evaluate('corona.scene.readActorIndex("scene.ini",'
                         + json.dumps({'archive_session': scopes[1]}) + ')'),
                         lambda result: not any(actor['actor_guid'] == removed['actor_guid'] for actor in result['actors']),
                         'host-deleted Actor disappears from the guest', seconds=15)
    transforms = []
    for writer, reader, writer_scope, reader_scope, position in [
        (host, guest, scopes[0], scopes[1], [2, 3, 4]),
        (guest, host, scopes[1], scopes[0], [-1, 2, 0]),
    ]:
        changed = writer['main'].evaluate('corona.scene.setActorTransform("scene.ini",'
            + json.dumps(created['actor_guid']) + ','
            + json.dumps({'archive_session': writer_scope, 'position': position}) + ')')
        assert changed['status'] == 'success' and changed['actor']['geometry']['position'] == position, changed
        observed = wait(lambda: reader['main'].evaluate('corona.scene.readTransforms('
                        + json.dumps({'archive_session': reader_scope, 'actors': [created['actor_guid']]}) + ')'),
                        lambda result: any(actor['actor_guid'] == created['actor_guid'] and actor['position'] == position
                                           for actor in result['actors']),
                        'native Actor transform reaches the other participant', seconds=15)
        transforms.append(observed)
    state = verify_object_state_sync(host, guest, created['actor_guid'], scopes, name)
    return dict(createdGuid=created['actor_guid'], hostArchive=scopes[0], guestArchive=scopes[1],
                guestIndex=index, deletedGuid=removed['actor_guid'], deletedIndex=deleted_index,
                transforms=transforms, state=state)


def verify_object_state_sync(host, guest, guid, scopes, original_name):
    evidence = {'properties': [], 'renames': []}
    failures = []
    for writer, reader, writer_scope, reader_scope, visible, suffix in [
        (host, guest, scopes[0], scopes[1], False, '-host-renamed'),
        (guest, host, scopes[1], scopes[0], True, '-guest-renamed'),
    ]:
        changed = writer['main'].evaluate('corona.scene.setActorProperties("scene.ini",' + json.dumps(guid) + ','
            + json.dumps({'archive_session': writer_scope, 'visible': visible}) + ')')
        assert changed['actor']['visible'] is visible, changed
        try:
            evidence['properties'].append(wait(lambda: reader['main'].evaluate('corona.scene.readActorProperties("scene.ini",'
                + json.dumps(guid) + ',' + json.dumps({'archive_session': reader_scope}) + ')'),
                lambda result: result['visible'] is visible, 'Actor visibility replicates to the other participant', seconds=15))
        except AssertionError as error:
            failures.append(str(error))
        renamed = writer['main'].evaluate('corona.scene.renameActor("scene.ini",' + json.dumps(guid) + ','
            + json.dumps(original_name + suffix) + ',' + json.dumps({'archive_session': writer_scope}) + ')')
        assert renamed['new_name'] == original_name + suffix, renamed
        try:
            evidence['renames'].append(wait(lambda: reader['main'].evaluate('corona.scene.readActorIndex("scene.ini",'
                + json.dumps({'archive_session': reader_scope}) + ')'),
                lambda result: any(actor['actor_guid'] == guid and actor['name'] == original_name + suffix
                                   for actor in result['actors']), 'Actor rename replicates to the other participant', seconds=15))
        except AssertionError as error:
            failures.append(str(error))
    assert not failures, '\n'.join(failures)
    return evidence

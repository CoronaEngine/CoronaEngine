"""Real native discovery, object replication and explicit reconnect without chat."""
import json
import socket
import uuid
from web_environment_smoke import wait


def run(launches):
    host, guest = launches
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]

    def call(launch, method, *args):
        return launch['main'].evaluate('corona.network.' + method + '('
            + ','.join(json.dumps(arg) for arg in args) + ')')

    def connect():
        assert call(guest, 'startSession', 'Acceptance Guest', 0, 0, 'client')['ok']
        assert call(guest, 'connectToPeer', '127.0.0.1', port, 'Acceptance Host')['ok']
        for launch in launches:
            wait(lambda: call(launch, 'getSessionInfo'),
                 lambda info: info['active'] and info['peer_count'] == 1,
                 'native peer handshake completes without a chat room', seconds=40)

    def create(name):
        scope = host['main'].evaluate('corona.scene.readActive()')['archive_session']
        return host['main'].evaluate('corona.scene.createActor("scene.ini",' + json.dumps(str(source)) + ','
            + json.dumps({'archive_session': scope, 'actor_type': 'model', 'actor_name': name}) + ')')

    def replicated(actor):
        scope = guest['main'].evaluate('corona.scene.readActive()')['archive_session']
        return wait(lambda: guest['main'].evaluate('corona.scene.readScene({scene:"scene.ini",actors:"index",...' + json.dumps({'archive_session': scope}) + '})'),
            lambda result: any(item['actor_guid'] == actor['actor_guid'] for item in result['actors']),
            'snapshot restores Actors created before connection', seconds=40)

    try:
        for launch in launches:
            assert launch['main'].evaluate('typeof corona.lanChat') == 'undefined'
            assert not call(launch, 'getSessionInfo')['active']
            assert call(launch, 'setProjectRoot', str(launch['project_root']))['ok']
        source = write_replication_source(host['project_root'])
        seeded = create('before-session-' + uuid.uuid4().hex)
        assert call(host, 'startSession', 'Acceptance Host', 0, port, 'host')['ok']
        call(guest, 'clearDiscoveredPeers')
        call(guest, 'searchLan')
        discovered = wait(lambda: call(guest, 'getDiscoveredPeers'),
            lambda info: any(peer.get('role') == 'host' and peer.get('port') == port
                             for peer in info.get('peers', [])),
            'LAN discovery finds the native host', seconds=40)
        connect()
        replicated(seeded)
        replication = verify_object_sync(host, guest)
        call(guest, 'stopSession')
        wait(lambda: call(host, 'getSessionInfo'), lambda info: info['peer_count'] == 0,
             'explicit guest disconnect reaches host', seconds=40)
        offline = create('offline-' + uuid.uuid4().hex)
        connect()
        replicated(offline)
        # Real transport loss follows the existing stop-session behavior.
        call(host, 'stopSession')
        disconnected = wait(lambda: call(guest, 'getSessionInfo'),
            lambda info: not info['active'] and info['role'] == 'none',
            'host transport shutdown stops client session', seconds=40)
        assert call(host, 'startSession', 'Acceptance Host', 0, port, 'host')['ok']
        connect()
        restored = replicated(offline)
        after_reconnect = verify_object_sync(host, guest)
        return dict(discovery=discovered, initialSeedGuid=seeded['actor_guid'],
                    objectReplication=replication, offlineGuid=offline['actor_guid'],
                    transportDisconnect=disconnected, explicitReconnect=restored,
                    afterReconnect=after_reconnect)
    finally:
        for launch in reversed(launches):
            call(launch, 'stopSession')


def write_replication_source(root):
    source = root / 'replication-triangle.ply'
    source.write_text('ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\n'
                      'property float y\nproperty float z\nelement face 1\n'
                      'property list uchar int vertex_indices\nend_header\n'
                      '0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n', encoding='ascii')
    return source


def verify_object_sync(host, guest):
    source = write_replication_source(host['project_root'])
    name = 'replication-' + uuid.uuid4().hex
    scopes = [launch['main'].evaluate('corona.scene.readActive()')['archive_session'] for launch in (host, guest)]
    created = host['main'].evaluate('corona.scene.createActor("scene.ini",' + json.dumps(str(source)) + ','
        + json.dumps({'archive_session': scopes[0], 'actor_type': 'model', 'actor_name': name}) + ')')
    assert created['name'] == name and created['archive_session'] == scopes[0], created
    print('Native object created on host: ' + created['actor_guid'], flush=True)
    index = wait(lambda: guest['main'].evaluate('corona.scene.readScene({scene:"scene.ini",actors:"index",...' + json.dumps({'archive_session': scopes[1]}) + '})'),
                 lambda result: any(actor['actor_guid'] == created['actor_guid'] for actor in result['actors']),
                 'host-created Actor reaches guest with the same stable GUID', seconds=15)
    removed = host['main'].evaluate('corona.scene.createActor("scene.ini",' + json.dumps(str(source)) + ','
        + json.dumps({'archive_session': scopes[0], 'actor_type': 'model', 'actor_name': name + '-delete'}) + ')')
    wait(lambda: guest['main'].evaluate('corona.scene.readScene({scene:"scene.ini",actors:"index",...' + json.dumps({'archive_session': scopes[1]}) + '})'),
         lambda result: any(actor['actor_guid'] == removed['actor_guid'] for actor in result['actors']),
         'deletion target first materializes on the guest', seconds=15)
    host['main'].evaluate('corona.scene.deleteActor("scene.ini",' + json.dumps(removed['actor_guid']) + ','
        + json.dumps({'archive_session': scopes[0]}) + ')')
    deleted_index = wait(lambda: guest['main'].evaluate('corona.scene.readScene({scene:"scene.ini",actors:"index",...' + json.dumps({'archive_session': scopes[1]}) + '})'),
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
        changed = writer['main'].evaluate('corona.scene.updateActor("scene.ini",' + json.dumps(guid) + ','
            + json.dumps({'archive_session': writer_scope, 'visible': visible}) + ')')
        assert changed['actor']['visible'] is visible, changed
        try:
            evidence['properties'].append(wait(lambda: reader['main'].evaluate('corona.scene.readActorProperties("scene.ini",'
                + json.dumps(guid) + ',' + json.dumps({'archive_session': reader_scope}) + ')'),
                lambda result: result['visible'] is visible, 'Actor visibility replicates to the other participant', seconds=15))
        except AssertionError as error:
            failures.append(str(error))
        renamed = writer['main'].evaluate('corona.scene.updateActor("scene.ini",' + json.dumps(guid) + ','
            + json.dumps({'archive_session': writer_scope, 'name': original_name + suffix}) + ')')
        assert renamed['actor']['name'] == original_name + suffix, renamed
        try:
            evidence['renames'].append(wait(lambda: reader['main'].evaluate('corona.scene.readScene({scene:"scene.ini",actors:"index",...' + json.dumps({'archive_session': reader_scope}) + '})'),
                lambda result: any(actor['actor_guid'] == guid and actor['name'] == original_name + suffix
                                   for actor in result['actors']), 'Actor rename replicates to the other participant', seconds=15))
        except AssertionError as error:
            failures.append(str(error))
    assert not failures, '\n'.join(failures)
    return evidence

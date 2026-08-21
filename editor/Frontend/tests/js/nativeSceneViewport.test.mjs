import assert from 'node:assert/strict';
import test from 'node:test';

import {
  cameraCandidatesFromSnapshot,
  cameraListFromPayload,
  computeCameraViewportRenderSize,
  resolveActiveCameraBinding,
  resolveInitialSceneId,
  resolveStoryCameraBinding,
  shouldCreateStoryCamera,
} from '../../src/utils/nativeSceneViewport.js';

test('resolves the active scene and clamps an invalid active index', () => {
  assert.equal(
    resolveInitialSceneId({
      data: {
        active_index: 1,
        scenes: [
          { name: 'First', path: 'Scenes/first.scene' },
          { name: 'Second', path: 'Scenes/second.scene' },
        ],
      },
    }),
    'Scenes/second.scene'
  );

  assert.equal(
    resolveInitialSceneId({ active_index: 20, scenes: [{ path: 'scene.ini' }] }),
    'scene.ini'
  );
});

test('supports the legacy single-scene init payload', () => {
  assert.equal(resolveInitialSceneId({ data: { name: 'LegacyScene' } }), 'LegacyScene');
});

test('resolves the named active camera and complete native binding', () => {
  const binding = resolveActiveCameraBinding(
    {
      data: {
        scene: 'scene.ini',
        active_camera_name: 'GameplayCamera',
        cameras: [
          { name: 'EditorCamera', handle: 11, camera_id: 'editor' },
          {
            name: 'GameplayCamera',
            handle: 22,
            camera_id: 'gameplay',
            position: [1, 2, 3],
            forward: [0, 0, -1],
            world_up: [0, 1, 0],
            fov: 60,
            story_move_speed: 18,
          },
        ],
      },
    },
    'fallback.scene'
  );

  assert.deepEqual(binding, {
    sceneId: 'scene.ini',
    cameraId: 'gameplay',
    cameraName: 'GameplayCamera',
    cameraHandle: 22,
    position: [1, 2, 3],
    forward: [0, 0, -1],
    worldUp: [0, 1, 0],
    fov: 60,
    moveSpeed: 18,
  });
});

test('falls back to snapshot.camera and rejects snapshots without a valid handle', () => {
  assert.equal(
    resolveActiveCameraBinding({ scene: { camera: { name: 'Main', camera_handle: '31' } } })
      ?.cameraHandle,
    31
  );
  assert.equal(resolveActiveCameraBinding({ data: { cameras: [{ name: 'Main' }] } }), null);
});

test('uses listed cameras when the scene snapshot has no usable camera', () => {
  const binding = resolveStoryCameraBinding(
    { data: { scene: 'Scenes/story.scene', cameras: [] } },
    {
      data: {
        cameras: [
          {
            id: 'listed-camera',
            name: 'ListedCamera',
            handle: 77,
            position: [4, 5, 6],
          },
        ],
      },
    },
    'fallback.scene'
  );

  assert.equal(binding?.sceneId, 'Scenes/story.scene');
  assert.equal(binding?.cameraId, 'listed-camera');
  assert.equal(binding?.cameraHandle, 77);
  assert.deepEqual(binding?.position, [4, 5, 6]);
});

test('prefers an existing StoryCamera over an unrelated listed fallback', () => {
  const binding = resolveStoryCameraBinding(
    { data: { scene: 'scene.ini', cameras: [] } },
    {
      cameras: [
        { id: 'first', name: 'OtherCamera', handle: 10 },
        { id: 'story', name: 'StoryCamera', handle: 20 },
      ],
    }
  );

  assert.equal(binding?.cameraId, 'story');
  assert.equal(binding?.cameraHandle, 20);
});

test('camera payload helpers preserve evidence of cameras with invalid handles', () => {
  const payload = { data: { camera: { name: 'Main' }, cameras: [{ name: 'Main' }] } };
  assert.equal(cameraCandidatesFromSnapshot(payload).length, 2);
  assert.equal(cameraListFromPayload({ data: { cameras: [{ name: 'Main' }] } }).length, 1);
});

test('only creates StoryCamera after repeated empty checks with no observed camera', () => {
  assert.equal(shouldCreateStoryCamera({ successfulEmptyChecks: 1 }), false);
  assert.equal(shouldCreateStoryCamera({ successfulEmptyChecks: 2 }), true);
  assert.equal(
    shouldCreateStoryCamera({ successfulEmptyChecks: 3, hasObservedCamera: true }),
    false
  );
  assert.equal(shouldCreateStoryCamera({ successfulEmptyChecks: 3, createAttempted: true }), false);
});

test('viewport render size uses physical pixels and caps the render budget', () => {
  assert.deepEqual(computeCameraViewportRenderSize(640, 360, 2), {
    width: 1280,
    height: 720,
  });

  const capped = computeCameraViewportRenderSize(3840, 2160, 2);
  assert.ok(capped.width * capped.height <= 1920 * 1080);
  assert.ok(Math.abs(capped.width / capped.height - 16 / 9) < 0.01);
});

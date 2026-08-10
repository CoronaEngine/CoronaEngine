import assert from 'node:assert/strict';
import test from 'node:test';

import { createDockOverlayStyles } from '../../src/components/dock/dockOverlayLayout.js';

test('viewport always fills the workspace regardless of open docks', () => {
  const closed = createDockOverlayStyles({});
  const open = createDockOverlayStyles({
    leftVisible: true,
    rightVisible: true,
    bottomVisible: true,
    leftWidth: 360,
    rightWidth: 400,
    bottomHeight: 320,
  });

  assert.deepEqual(closed.viewport, { inset: '0' });
  assert.deepEqual(open.viewport, closed.viewport);
});

test('bottom dock occupies the space between side overlays', () => {
  const styles = createDockOverlayStyles({
    leftVisible: true,
    rightVisible: true,
    bottomVisible: true,
    leftWidth: 360,
    rightWidth: 400,
    bottomHeight: 320,
    separatorSize: 4,
  });

  assert.deepEqual(styles.bottom, {
    left: '364px',
    right: '404px',
    bottom: '0',
    height: '320px',
  });
  assert.deepEqual(styles.bottomSeparator, {
    left: '364px',
    right: '404px',
    bottom: '320px',
  });
});

test('hidden side docks do not reserve overlay space', () => {
  const styles = createDockOverlayStyles({
    bottomVisible: true,
    bottomHeight: 240,
    leftWidth: 360,
    rightWidth: 400,
  });

  assert.equal(styles.bottom.left, '0px');
  assert.equal(styles.bottom.right, '0px');
});

export const pickedNodeIdentity = (payload = {}) => {
  const nodeIndex = Number(payload?.node_index ?? payload?.nodeIndex);
  const actorName =
    payload?.actor_name || payload?.actorName || payload?.name || payload?.actor?.name;
  if (!actorName || !Number.isInteger(nodeIndex) || nodeIndex < 0) return null;
  return { actorName, nodeIndex };
};

export const nodeTree = (nodes = []) => {
  const byIndex = new Map(
    nodes.map((node) => [Number(node.node_index), { ...node, children: [] }])
  );
  const roots = [];
  for (const node of byIndex.values()) {
    const parent = byIndex.get(Number(node.parent_index));
    if (parent) parent.children.push(node);
    else roots.push(node);
  }
  return roots;
};

// The importer wraps a model in a synthetic root (usually named "root"), and glTF
// assets commonly add another single-child container ("Scene").  Those entries carry
// no editable content of their own, so the dock would show a one-row menu above the
// model's real nodes.  Skipping them puts the nodes directly under the actor.
export const MODEL_NODE_DISPLAY_MAX_SKIP = 3;

// Returns the node whose children are shown as the top level, or -1 when the roots
// themselves are content.  `depth` counts the containers skipped so far, which bounds
// a chain of single-child containers.
export const resolveModelNodeDisplayParent = (page = {}, depth = 0) => {
  const items = Array.isArray(page?.items) ? page.items : [];
  if (depth >= MODEL_NODE_DISPLAY_MAX_SKIP) return -1;
  if (items.length !== 1) return -1;
  if (Number(items[0]?.child_count || 0) <= 0) return -1;
  const index = Number(items[0]?.node_index);
  return Number.isInteger(index) && index >= 0 ? index : -1;
};

export const flattenLoadedNodes = (pages = {}, expanded = new Set(), rootKey = '-1') => {
  const rows = [];
  const visit = (parentKey, depth) => {
    const page = pages[parentKey] || {};
    for (const node of page.items || []) {
      rows.push({ node, depth });
      if (expanded.has(Number(node.node_index))) visit(String(node.node_index), depth + 1);
    }
    if (page.has_more) rows.push({ kind: 'more', parentKey, depth });
  };
  visit(rootKey, 0);
  return rows;
};

export const virtualNodeWindow = (
  rows,
  scrollTop,
  viewportHeight,
  rowHeight = 28,
  overscan = 6
) => {
  const visibleStart = Math.floor(Math.max(0, scrollTop) / rowHeight);
  const visibleCount = Math.ceil(Math.max(rowHeight, viewportHeight) / rowHeight);
  const start = Math.max(0, visibleStart - overscan);
  const end = Math.min(rows.length, visibleStart + visibleCount + overscan);
  return {
    start,
    end,
    topSpacer: start * rowHeight,
    bottomSpacer: Math.max(0, (rows.length - end) * rowHeight),
  };
};

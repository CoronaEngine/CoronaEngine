/**
 * The story page's keyboard routing, extracted so the page-level event flow is testable.
 *
 * `StoryWorld.vue::onKeyDown` used to inline this chain; every suite still stayed green because
 * the tests only ever called `camera.keyDown(event)` directly. They never walked the page's own
 * handler, so a guard that swallowed a key *before* the camera saw it could pass every suite and
 * still break the real page. This module is the single source of that decision: `onKeyDown` asks
 * it which branch a key belongs to and only then runs that branch's side effect.
 *
 * The decision is a pure function of the event and the page state, so the test can drive the real
 * routing and the real camera controllers together — the exact flow a keydown takes in the page.
 */
export const STORY_KEY_ROUTES = Object.freeze([
  'ignore', 'adjust', 'dialogue', 'hotbar-slot', 'place-item', 'escape',
  'tab', 'inventory', 'cycle-view', 'interact', 'space-busy', 'skill', 'camera',
]);

const isSpace = event => event.code === 'Space' || event.key === ' ' || event.key === 'Spacebar';
const isEscape = event => event.code === 'Escape' || event.key === 'Escape';
const isTab = event => event.code === 'Tab' || event.key === 'Tab';
const isKey = (event, code, letter) => event.code === code || event.key?.toLowerCase() === letter;

/**
 * Which branch of the page handler a keydown belongs to.
 *
 * Order matters exactly as much as it did in the original handler: panels own the keyboard first
 * (adjust → dialogue → hotbar/B), then Escape/Tab, then the inventory, then V/F, and only then do
 * the movement keys reach the camera.
 *
 * 主世界与小世界的分界就在这里：
 *   - Space 在主世界是跳跃（`playerController.jumpKey`），动作/保存进行中必须被吞掉，否则玩家
 *     会在存档还没确认时起跳；在小世界里 Space 是相机升降，不碰玩家存档，所以**不经过这个
 *     闸门**，直接交给相机。
 *   - E/R 技能只属于主世界；小世界把 E 留给相机，这样自由/俯视视角的 Q/E 升降才按得动。
 */
export function routeStoryKeydown(event, state) {
  const {
    current, adjustMode, dialogueOpen, inSubworld, inventoryOpen,
    actionBusy, busy, needsSave,
  } = state;

  if (!current) return 'ignore';
  if (adjustMode) return 'adjust';
  if (dialogueOpen) return 'dialogue';
  // 物品栏：调整模式与对话（上面两个分支）已经吃掉数字键，因此这里只在
  // 小世界、面板关闭时接管数字键与 B。主世界里这套键完全不生效。
  if (inSubworld && !inventoryOpen && !event.ctrlKey && !event.altKey
    && !event.metaKey && !event.isComposing) {
    if (/^[1-9]$/.test(event.key || '')) return 'hotbar-slot';
    if (event.code === 'KeyB' || event.key?.toLowerCase() === 'b') return 'place-item';
  }
  if (isEscape(event)) return 'escape';
  if (event.ctrlKey || event.altKey || event.metaKey || event.isComposing) return 'ignore';
  if (isTab(event)) return 'tab';
  if (inventoryOpen) return 'inventory';
  if (isKey(event, 'KeyV', 'v')) return 'cycle-view';
  if (isKey(event, 'KeyF', 'f')) return 'interact';
  // 空格只在「主世界 + 动作/保存进行中」被吞（跳跃会改写玩家存档）；小世界的空格是相机升降，
  // 不碰存档，因此不受这个闸门影响。
  if (!inSubworld && isSpace(event) && (actionBusy || busy || needsSave)) return 'space-busy';
  if (!inSubworld && (event.code === 'KeyE' || event.code === 'KeyR'
    || ['e', 'r'].includes(event.key?.toLowerCase()))) return 'skill';
  return 'camera';
}

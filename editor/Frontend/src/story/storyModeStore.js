/**
 * 剧情模式状态：集中保存覆盖层、玩家状态、世界状态和调试状态。
 */
import { reactive } from 'vue';

export const storyModeStore = reactive({
  running: false,
  worldType: 'main',
  inventoryOpen: false,
  mapOpen: false,
  pointerLocked: false,
  mouseActive: false,
  debugVisible: false,
  player: { x: 0, y: 1.7, z: 4 },
  bossHealth: 100,
  interactionTarget: '',
  items: [
    {
      id: 'material-wood',
      name: '木材',
      category: 'material',
      quantity: 12,
      description: '基础建造材料。',
    },
    {
      id: 'world-fragment-demo',
      name: '世界碎片',
      category: 'ugc',
      quantity: 1,
      description: '承载受控游戏逻辑，可用于制作 Demo。',
    },
    {
      id: 'world-orb-demo',
      name: '世界小球',
      category: 'ugc',
      quantity: 1,
      description: '进入空白 UGC 世界的入口。',
    },
  ],
});

export function closeOverlays() {
  storyModeStore.inventoryOpen = false;
  storyModeStore.mapOpen = false;
  storyModeStore.mouseActive = false;
}

export function toggleInventory() {
  storyModeStore.inventoryOpen = !storyModeStore.inventoryOpen;
  if (storyModeStore.inventoryOpen) storyModeStore.mapOpen = false;
}

export function toggleMap() {
  storyModeStore.mapOpen = !storyModeStore.mapOpen;
  if (storyModeStore.mapOpen) storyModeStore.inventoryOpen = false;
}


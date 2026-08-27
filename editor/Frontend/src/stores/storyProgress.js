import { defineStore } from 'pinia';
import { STORY_QUEST_DEFINITIONS } from '@/config/storyNpc.js';
import { normalizeStoryProgress, storyProgressStorageKey } from '@/utils/storyNpc.js';

function storage() { try { return window.localStorage; } catch { return null; } }

export const useStoryProgressStore = defineStore('storyProgress', {
  state: () => ({ projectKey: '', data: normalizeStoryProgress(), loaded: false, notice: null }),
  getters: {
    activeQuest: (state) => STORY_QUEST_DEFINITIONS.find((quest) => quest.id === state.data.activeQuestId) || null,
    unlockedWorldBalls: (state) => state.data.unlockedWorldBalls,
    completedQuestIds: (state) => state.data.completedQuestIds,
  },
  actions: {
    notify(message, kind = 'info') { this.notice = message ? { message: String(message), kind, id: Date.now() } : null; },
    load(projectKey, store = storage()) {
      this.projectKey = String(projectKey || '').trim();
      let source = null;
      try { const raw = store?.getItem(storyProgressStorageKey(this.projectKey)); if (raw) source = JSON.parse(raw); } catch { this.notify('剧情进度存档损坏，已使用安全默认值。', 'warning'); }
      this.data = normalizeStoryProgress(source); this.loaded = true; return this.data;
    },
    save(store = storage()) { if (!this.projectKey || !store) return false; this.data.updatedAt = Date.now(); try { store.setItem(storyProgressStorageKey(this.projectKey), JSON.stringify(this.data)); return true; } catch { this.notify('剧情进度暂时无法保存。', 'warning'); return false; } },
    acceptQuest(questId) { const quest = STORY_QUEST_DEFINITIONS.find((item) => item.id === questId); if (!quest || this.data.completedQuestIds.includes(quest.id)) return false; this.data.activeQuestId = quest.id; this.save(); return true; },
    completeQuest(questId) { if (!this.data.completedQuestIds.includes(questId)) this.data.completedQuestIds.push(questId); if (this.data.activeQuestId === questId) this.data.activeQuestId = null; this.save(); return true; },
    unlockWorldBall(id = 'demo-1') { const value = String(id); if (!this.data.unlockedWorldBalls.includes(value)) this.data.unlockedWorldBalls.push(value); this.save(); return value; },
    setMerchantForDay(day, available) { this.data.merchantByDay[String(Math.max(1, Math.trunc(Number(day) || 1)))] = Boolean(available); this.save(); },
    merchantForDay(day) { return Boolean(this.data.merchantByDay[String(Math.max(1, Math.trunc(Number(day) || 1)))]); },
    updateStats(stats = {}) { this.data.questStats = { ...this.data.questStats, ...stats }; this.save(); },
    questProgress(questId) {
      const quest = STORY_QUEST_DEFINITIONS.find((item) => item.id === questId);
      if (!quest) return { current: 0, target: 0, complete: false };
      const stats = this.data.questStats || {};
      const current = Math.max(0, Math.trunc(Number(
        quest.type === 'minions' ? stats.minionKills
          : quest.type === 'boss' ? stats.bossKills
            : quest.type === 'fragments' ? stats.fragmentCount
              : stats.exploredAreas
      ) || 0));
      return { current, target: quest.target, complete: current >= quest.target };
    },
    claimQuest(questId) {
      const quest = STORY_QUEST_DEFINITIONS.find((item) => item.id === questId);
      if (!quest || !this.data.activeQuestId || this.data.activeQuestId !== quest.id) return { success: false, reason: 'not-active' };
      if (!this.questProgress(quest.id).complete) return { success: false, reason: 'incomplete' };
      this.completeQuest(quest.id);
      return { success: true, reward: { ...quest.reward } };
    },
    merchantPurchaseKey(day, stock) { return `${Math.max(1, Math.trunc(Number(day) || 1))}:${String(stock?.itemId || '')}`; },
    hasMerchantPurchase(day, stock) { return Boolean(this.data.merchantPurchases?.[this.merchantPurchaseKey(day, stock)]); },
    markMerchantPurchase(day, stock) { this.data.merchantPurchases[this.merchantPurchaseKey(day, stock)] = true; this.save(); },
  },
});

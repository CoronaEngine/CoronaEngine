import { defineStore } from 'pinia';
import { STORY_QUEST_DEFINITIONS } from '@/config/storyNpc.js';
import { normalizeStoryProgress, storyProgressStorageKey } from '@/utils/storyNpc.js';

function storage() { try { return window.localStorage; } catch { return null; } }

export const useStoryProgressStore = defineStore('storyProgress', {
  state: () => ({ projectKey: '', data: normalizeStoryProgress(), loaded: false, notice: null }),
  getters: {
    activeQuest: (state) => STORY_QUEST_DEFINITIONS.find((quest) => quest.id === state.data.activeQuestId) || null,
    unlockedWorldBalls: (state) => state.data.worldBalls.map((ball) => ball.id),
    worldBalls: (state) => state.data.worldBalls,
    activeWorldBall: (state) => state.data.worldBalls.find((ball) => ball.id === state.data.activeWorldBallId) || null,
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
    unlockWorldBall(id = 'demo-1', overrides = {}) {
      const value = String(id);
      if (!this.data.worldBalls.some((ball) => ball.id === value)) {
        const now = Date.now();
        this.data.worldBalls.push({
          id: value,
          name: String(overrides.name || (value === 'demo-1' ? '我的第一个小世界' : `小世界 ${value}`)),
          sceneName: String(overrides.sceneName || `StoryDemo_${value.replace(/[^a-z0-9_-]/gi, '_')}`),
          status: overrides.status || 'empty',
          actorCount: Number(overrides.actorCount) || 0,
          coreInstalled: Number(overrides.coreInstalled) || 0,
          playable: Boolean(overrides.playable),
          validation: Array.isArray(overrides.validation) ? overrides.validation : [],
          isDefault: this.data.worldBalls.length === 0,
          sourceBallId: String(overrides.sourceBallId || ''),
          createdAt: now,
          updatedAt: now,
        });
      }
      if (!this.data.activeWorldBallId) this.data.activeWorldBallId = value;
      this.save();
      return value;
    },
    upsertWorldBall(record = {}) {
      const id = String(record.id || '').trim();
      if (!id) return null;
      const index = this.data.worldBalls.findIndex((ball) => ball.id === id);
      const now = Date.now();
      const next = {
        ...(index >= 0 ? this.data.worldBalls[index] : {}),
        ...record,
        id,
        updatedAt: now,
        createdAt: Number(record.createdAt || this.data.worldBalls[index]?.createdAt) || now,
      };
      if (index >= 0) this.data.worldBalls.splice(index, 1, next);
      else this.data.worldBalls.push(next);
      if (!this.data.activeWorldBallId) this.data.activeWorldBallId = id;
      this.save();
      return next;
    },
    removeWorldBall(id) {
      const value = String(id || '').trim();
      const index = this.data.worldBalls.findIndex((ball) => ball.id === value);
      if (index < 0) return null;
      const [removed] = this.data.worldBalls.splice(index, 1);
      if (this.data.activeWorldBallId === value) {
        this.data.activeWorldBallId = this.data.worldBalls.find((ball) => ball.isDefault)?.id || this.data.worldBalls[0]?.id || '';
      }
      this.save();
      return removed;
    },
    setActiveWorldBall(id) {
      const value = String(id || '').trim();
      if (!this.data.worldBalls.some((ball) => ball.id === value)) return false;
      this.data.activeWorldBallId = value;
      this.save();
      return true;
    },
    setDefaultWorldBall(id) {
      const value = String(id || '').trim();
      if (!this.data.worldBalls.some((ball) => ball.id === value)) return false;
      this.data.worldBalls = this.data.worldBalls.map((ball) => ({ ...ball, isDefault: ball.id === value }));
      this.data.activeWorldBallId = value;
      this.save();
      return true;
    },
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


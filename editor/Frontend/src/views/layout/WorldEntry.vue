<script setup>
import { defineAsyncComponent } from 'vue';
import { worldModeState } from '@/services/worldModeService.js';

const CreativeWorld = defineAsyncComponent(() => import('./MainPage.vue'));
const StoryWorld = defineAsyncComponent(() => import('./StoryWorld.vue'));
</script>

<template>
  <StoryWorld v-if="worldModeState.status === 'ready' && worldModeState.mode === 'story'" />
  <CreativeWorld v-else-if="worldModeState.status === 'ready' && worldModeState.mode === 'creative'" />
</template>

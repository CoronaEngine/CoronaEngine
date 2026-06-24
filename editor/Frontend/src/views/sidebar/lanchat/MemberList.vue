<template>
  <div class="text-sm">
    <!-- 人类成员 -->
    <div class="px-2 pb-1 text-gray-400">成员</div>
    <div
      v-for="m in displayedMembers"
      :key="m.key"
      class="px-2 py-1.5 truncate text-gray-200"
    >
      {{ m.label }}
    </div>

    <!-- AI 助手 -->
    <div class="px-2 pt-2 pb-1 text-gray-400 border-t border-gray-700 mt-1">
      AI 助手
    </div>
    <div
      v-for="a in agents"
      :key="'a-' + a.agent_id"
      class="group mx-1 px-1.5 py-1.5 rounded flex items-center justify-between gap-1 text-gray-200 hover:bg-[#343434]"
    >
      <span class="truncate">🤖 {{ a.name }}</span>
      <button
        v-if="a.owner === peerId"
        class="h-5 w-5 shrink-0 rounded text-red-400 opacity-0 transition-opacity hover:bg-red-500/20 hover:text-red-300 focus:opacity-100 group-hover:opacity-100"
        title="移除"
        @click="$emit('remove-agent', a.agent_id)"
      >
        ✕
      </button>
    </div>
    <button
      class="mx-1 mt-1 flex w-[calc(100%-0.5rem)] items-center gap-1.5 rounded border border-dashed border-[#84A65B]/45 px-2 py-1.5 text-left text-xs text-[#B8D58D] hover:border-[#84A65B] hover:bg-[#84A65B]/10"
      title="添加 AI 助手"
      @click="$emit('add-agent')"
    >
      <span class="inline-flex h-4 w-4 items-center justify-center rounded bg-[#84A65B]/20 text-[13px] leading-none">+</span>
      <span class="truncate">添加助手</span>
    </button>
  </div>
</template>

<script setup>
import { computed } from 'vue';

const props = defineProps({
  members: { type: Array, default: () => [] },
  memberDetails: { type: Array, default: () => [] },
  peerId: { type: String, default: '' },
  showSelfMarker: { type: Boolean, default: false },
  agents: { type: Array, default: () => [] },
});
defineEmits(['remove-agent', 'add-agent']);

const displayedMembers = computed(() => {
  const details = Array.isArray(props.memberDetails) ? props.memberDetails : [];
  if (details.length) {
    return details.map((member, index) => {
      const memberId = String(member.member_id || member.id || '');
      const nickname = String(member.nickname || member.name || '');
      const isSelf = props.showSelfMarker && memberId && memberId === props.peerId;
      return {
        key: memberId || `m-${index}`,
        label: isSelf ? `${nickname}（我）` : nickname,
      };
    }).filter((member) => member.label);
  }
  return (Array.isArray(props.members) ? props.members : [])
    .map((name, index) => ({ key: `m-${index}`, label: String(name || '') }))
    .filter((member) => member.label);
});
</script>

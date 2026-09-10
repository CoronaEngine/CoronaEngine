/** 玩家生命服务：护甲提供持续减伤，不作为可消耗的护盾。 */
export function mitigatedDamage(damage, armor = 0) {
  return (
    Math.max(0, Number(damage) || 0) * (1 - Math.min(20, Math.max(0, Number(armor) || 0)) * 0.04)
  );
}
export function createPlayerVitals(state, getArmor = () => 0) {
  return {
    damage(amount) {
      if (state.dead) return 0;
      const damage = mitigatedDamage(amount, getArmor());
      state.health = Math.max(0, state.health - damage);
      state.dead = state.health <= 0;
      return damage;
    },
    respawn() {
      state.health = state.maxHealth;
      state.dead = false;
    },
  };
}

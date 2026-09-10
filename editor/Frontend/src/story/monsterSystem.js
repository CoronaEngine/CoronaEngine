/** 主世界怪物模拟：使用纯数据并注入视线和碰撞检测，便于执行可复现测试。 */
export const MONSTER_CONFIG = Object.freeze({
  health: 60,
  wanderRadius: 6,
  detect: 12,
  disengage: 18,
  reach: 1.8,
  windup: 0.35,
  interval: 1.2,
  damage: 10,
  walk: 1.2,
  chase: 2.4,
});
export const MONSTER_SPAWNS = Object.freeze([
  [-8, 0, -4],
  [13, 0, -22],
  [-13, 0, -28],
]);
const distance = (a, b) => Math.hypot(a.x - b.x, a.z - b.z);
export function createMonsterSystem({
  spawns = MONSTER_SPAWNS,
  config = MONSTER_CONFIG,
  random = Math.random,
  visible = () => true,
  canMove = () => true,
  onDamage = () => {},
} = {}) {
  const monsters = spawns.map(([x, y, z], index) => ({
    id: `monster-${index}`,
    position: { x, y, z },
    home: { x, y, z },
    goal: { x, y, z },
    health: config.health,
    state: 'idle',
    timer: 0.5 + random(),
    flash: 0,
    yaw: 0,
  }));
  function transition(m, state, timer = 0) {
    m.state = state;
    m.timer = timer;
  }
  function move(m, destination, speed, dt) {
    const length = distance(m.position, destination);
    if (length < 0.15) return true;
    const step = Math.min(length, speed * dt);
    const dx = (destination.x - m.position.x) / length;
    const dz = (destination.z - m.position.z) / length;
    m.yaw = Math.atan2(dx, dz);
    const next = { ...m.position, x: m.position.x + dx * step, z: m.position.z + dz * step };
    // 碰撞检测和怪物间距检查不通过时拒绝移动，不瞬移或穿过障碍物。
    const free = (point) =>
      canMove(m.position, point) &&
      !monsters.some(
        (other) => other !== m && other.health > 0 && distance(point, other.position) < 0.85
      );
    if (free(next)) Object.assign(m.position, next);
    else {
      const side = { ...m.position, x: m.position.x - dz * step, z: m.position.z + dx * step };
      if (free(side)) Object.assign(m.position, side);
    }
    return false;
  }
  return {
    monsters,
    hit(id, amount) {
      const m = monsters.find((value) => value.id === id);
      if (!m || m.health <= 0) return false;
      m.health = Math.max(0, m.health - Math.max(0, amount));
      m.flash = 0.18;
      if (!m.health) transition(m, 'dead');
      return true;
    },
    resetAggro() {
      for (const m of monsters) if (m.health > 0) transition(m, 'return');
    },
    update(delta, player, paused = false) {
      if (paused) return;
      const dt = Math.min(Math.max(0, delta), 0.05);
      for (const m of monsters) {
        if (m.health <= 0) continue;
        m.timer -= dt;
        m.flash = Math.max(0, m.flash - dt);
        const range = distance(m.position, player.position);
        const groundedTarget =
          Math.abs(player.position.y - (player.height || 1.7) - m.position.y) <= 1.2;
        const inSight = groundedTarget && visible(m.position, player.position);
        const tooFar = range > config.disengage || distance(m.position, m.home) > config.disengage;
        if (['chase', 'windup', 'cooldown'].includes(m.state) && (tooFar || player.dead))
          transition(m, 'return');
        if (
          ['idle', 'wander'].includes(m.state) &&
          !player.dead &&
          range <= config.detect &&
          inSight
        )
          transition(m, 'chase');
        switch (m.state) {
          case 'idle':
            if (m.timer <= 0) {
              const angle = random() * Math.PI * 2,
                radius = Math.sqrt(random()) * config.wanderRadius;
              m.goal = {
                ...m.home,
                x: m.home.x + Math.cos(angle) * radius,
                z: m.home.z + Math.sin(angle) * radius,
              };
              transition(m, 'wander', 6);
            }
            break;
          case 'wander':
            if (move(m, m.goal, config.walk, dt) || m.timer <= 0)
              transition(m, 'idle', 0.5 + random() * 1.5);
            break;
          case 'chase':
            if (range <= config.reach && inSight) transition(m, 'windup', config.windup);
            else move(m, player.position, config.chase, dt);
            break;
          case 'windup':
            if (m.timer <= 0) {
              if (!player.dead && range <= config.reach && inSight) onDamage(config.damage, m);
              transition(m, 'cooldown', config.interval - config.windup);
            }
            break;
          case 'cooldown':
            if (m.timer <= 0) transition(m, 'chase');
            break;
          case 'return':
            if (move(m, m.home, config.chase, dt)) transition(m, 'idle', 1);
            break;
        }
      }
    },
  };
}

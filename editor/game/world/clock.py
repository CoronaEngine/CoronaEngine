"""管理剧情累计时间与昼夜阶段。
根据配置的昼夜时长，将整数纳秒换算为天数、当前阶段和阶段内经过时间。
推进时按顺序返回跨越的昼夜边界，拒绝非法或超出累计时间范围的增量。"""

from copy import deepcopy

from ..core.config import GameConfig
from ..core.state import ClockState, ClockTransition
from ..core.types import MAX_TIME_NS, DayPhase, Error, Result, valid_delta_ns


class WorldClock:
    def __init__(self, config: GameConfig = GameConfig()) -> None:
        if type(config) is not GameConfig:
            raise ValueError("config 必须是 GameConfig")
        config.validate()
        self._config = deepcopy(config)
        self._elapsed_ns = 0

    @property
    def state(self) -> ClockState:
        cycle = self._config.day_duration_ns + self._config.night_duration_ns
        days, within = divmod(self._elapsed_ns, cycle)
        night = within >= self._config.day_duration_ns
        return ClockState(
            self._elapsed_ns, days + 1, DayPhase.NIGHT if night else DayPhase.DAY,
            within - self._config.day_duration_ns if night else within,
        )

    @property
    def next_boundary_ns(self) -> int:
        state = self.state
        duration = (self._config.day_duration_ns if state.phase is DayPhase.DAY
                    else self._config.night_duration_ns)
        return self._elapsed_ns + duration - state.phase_elapsed_ns

    def can_advance(self, delta_ns: object) -> bool:
        return valid_delta_ns(delta_ns) and delta_ns <= MAX_TIME_NS - self._elapsed_ns

    def advance_ns(self, delta_ns: int) -> Result[tuple[ClockTransition, ...]]:
        if not self.can_advance(delta_ns):
            return Result(Error.INVALID_DELTA)
        target = self._elapsed_ns + delta_ns
        changes: list[ClockTransition] = []
        while self.next_boundary_ns <= target:
            self._elapsed_ns = self.next_boundary_ns
            state = self.state
            changes.append(ClockTransition(state.elapsed_ns, state.day, state.phase))
        self._elapsed_ns = target
        return Result(value=tuple(changes))

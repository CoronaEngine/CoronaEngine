export function formatFrameTiming(response, metric = 'render_ms') {
  const sample = response?.data ?? response;
  return sample && Number.isFinite(sample[metric]) && sample[metric] > 0
    && Number.isFinite(sample.age_ms) && sample.age_ms >= 0 && sample.age_ms < 2000
    ? `${sample[metric].toFixed(2)} ms` : '— ms';
}

export function startFrameTimingPolling({
  read, onSample, schedule = setTimeout, cancel = clearTimeout,
}) {
  let stopped = false;
  let timer;
  const poll = async () => {
    try {
      const sample = await read();
      if (!stopped) onSample(sample);
    } catch {
      if (!stopped) onSample(null);
    } finally {
      if (!stopped) timer = schedule(poll, 250);
    }
  };
  void poll();
  return () => { stopped = true; cancel(timer); };
}

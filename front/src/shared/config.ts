export const HWTEST_WS_URL =
  import.meta.env.VITE_HWTEST_WS_URL?.trim() || 'ws://127.0.0.1:18765/ws'

export const SAMPLE_CAPACITY = 50_000
export const DIAGNOSTIC_CAPACITY = 500
export const CHART_COMMIT_INTERVAL_MS = 100
export const DEFAULT_TIME_WINDOW_SECONDS = 300

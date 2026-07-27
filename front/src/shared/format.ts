import type { TestMeasurementDescriptor } from './protocol'

const CONNECTION_STATE_LABELS: Record<string, string> = {
  disconnected: '未连接',
  connecting: '连接中',
  connected: '已连接',
  reconnecting: '重连中',
  error: '连接错误',
}

const PHASE_LABELS: Record<string, string> = {
  empty: '未加载',
  configured: '已配置',
  preparing: '连接中',
  ready: '就绪',
  running: '运行中',
  paused: '已暂停',
  stopping: '停止中',
  stopped: '已停止',
  finished: '已完成',
  error: '错误',
  shutdown_failed: '关闭失败',
}

const VERDICT_LABELS: Record<string, string> = {
  pass: '通过',
  fail: '未通过',
  error: '错误',
  skipped: '已跳过',
}

const FIELD_LABELS: Record<string, string> = {
  cpu_usage: 'CPU 占用率',
  mem_usage: '内存占用率',
  cpu_freq_little: '小核频率',
  cpu_freq_big: '大核频率',
  pcie_speed: 'PCIe 速率',
  pcie_width: 'PCIe 通道宽度',
  net_init_time: '网络初始化时间',
  cpu_temp: 'CPU 温度',
  rk_temp: 'RK 温度',
  k7_temp: 'K7 温度',
  power_on_sec: '上电时长',
  status: '设备状态',
  err_code: '设备错误码',
}

const FIELD_UNITS: Record<string, string> = {
  cpu_usage: '%',
  mem_usage: '%',
  cpu_freq_little: 'MHz',
  cpu_freq_big: 'MHz',
  pcie_speed: 'GT/s',
  pcie_width: 'lane',
  net_init_time: 'ms',
  cpu_temp: '°C',
  rk_temp: '°C',
  k7_temp: '°C',
  power_on_sec: 's',
}

export function fieldLabel(
  path: string,
  measurements: TestMeasurementDescriptor[] = [],
): string {
  const descriptor = measurements.find(({ id }) => id === path)
  return descriptor?.label || FIELD_LABELS[path] || path.replaceAll('_', ' ')
}

export function fieldUnit(
  path: string,
  measurements: TestMeasurementDescriptor[] = [],
): string {
  const descriptor = measurements.find(({ id }) => id === path)
  return descriptor ? descriptor.unit : FIELD_UNITS[path] || ''
}

export function formatValue(value: unknown, digits = 2): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return '—'
  return new Intl.NumberFormat('zh-CN', {
    maximumFractionDigits: digits,
  }).format(value)
}

export function formatTimestamp(timestampUs?: number): string {
  if (!timestampUs) return '尚无样本'
  return new Intl.DateTimeFormat('zh-CN', {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    fractionalSecondDigits: 3,
    hour12: false,
  }).format(new Date(timestampUs / 1000))
}

export function formatDuration(seconds: unknown): string {
  if (typeof seconds !== 'number' || !Number.isFinite(seconds)) return '—'
  const days = Math.floor(seconds / 86_400)
  const hours = Math.floor((seconds % 86_400) / 3_600)
  const minutes = Math.floor((seconds % 3_600) / 60)
  return days > 0 ? `${days}天 ${hours}时` : `${hours}时 ${minutes}分`
}

export function connectionStateLabel(state: string): string {
  return CONNECTION_STATE_LABELS[state.toLowerCase()] || state
}

export function phaseLabel(phase: string): string {
  return PHASE_LABELS[phase.toLowerCase()] || phase
}

export function verdictLabel(verdict: string): string {
  return verdict ? VERDICT_LABELS[verdict.toLowerCase()] || verdict : '待判定'
}

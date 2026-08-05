import {
  ArrowsLeftRight,
  Cpu,
  Gauge,
  Pulse,
  Thermometer,
} from '@phosphor-icons/react'
import { useMemo } from 'react'

import { DigitalStimulusPanel } from '../features/digital-stimulus/DigitalStimulusPanel'
import {
  connectionStateLabel,
  fieldLabel,
  fieldUnit,
  formatDuration,
  formatTimestamp,
  formatValue,
  verdictLabel,
} from '../shared/format'
import type { TestMeasurementDescriptor } from '../shared/protocol'
import { useSession, useTelemetry } from '../features/session/SessionProvider'

function MetricCard({
  field,
  value,
  measurements,
}: {
  field: string
  value: unknown
  measurements: TestMeasurementDescriptor[]
}) {
  const isTemperature = field.includes('temp')
  const isRuntime = field === 'power_on_sec'
  const Icon = isTemperature ? Thermometer : field.includes('usage') ? Gauge : Cpu
  return (
    <article className="metric-card">
      <div className="metric-card__top">
        <span>{fieldLabel(field, measurements)}</span>
        <Icon size={18} />
      </div>
      <div className="metric-card__value">
        {isRuntime ? formatDuration(value) : formatValue(value)}
        {!isRuntime && <small>{fieldUnit(field, measurements)}</small>}
      </div>
    </article>
  )
}

const TIMER_JITTER_ALGORITHM_ID = 'mbddf.timer_jitter'
const TIMER_BUCKET_KEYS = Array.from({ length: 8 }, (_, index) => `buckets[${index}]`)
const TIMER_BUCKET_LABELS = [
  '[0, 2) µs',
  '[2, 4) µs',
  '[4, 8) µs',
  '[8, 16) µs',
  '[16, 32) µs',
  '[32, 64) µs',
  '[64, 100) µs',
  '≥100 µs',
]

interface TimerJitterDistribution {
  counts: Array<number | null>
  total: number | null
  warning: string
}

function recordValue(value: unknown): Record<string, unknown> | null {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null
}

function measurementNumericValue(
  measurement: TestMeasurementDescriptor,
  values: Record<string, unknown>,
): number | undefined {
  const direct = values[measurement.id]
  if (typeof direct === 'number') return direct
  if (measurement.sourceId === undefined || measurement.bitIndex === undefined) return undefined
  const source = values[measurement.sourceId]
  if (typeof source !== 'number' || !Number.isSafeInteger(source) ||
      source < 0 || source > 0xffff_ffff ||
      !Number.isInteger(measurement.bitIndex) ||
      measurement.bitIndex < 0 || measurement.bitIndex > 31) {
    return undefined
  }
  return Number((BigInt(source) >> BigInt(measurement.bitIndex)) & 1n)
}

function hasTimerBucket(values: Record<string, unknown>): boolean {
  return TIMER_BUCKET_KEYS.some((key) => Object.prototype.hasOwnProperty.call(values, key))
}

function hasTimerResponse(values: Record<string, unknown>): boolean {
  return hasTimerBucket(values) ||
    Object.prototype.hasOwnProperty.call(values, 'status') ||
    Object.prototype.hasOwnProperty.call(values, 'err_code')
}

function validBucketCount(value: unknown): value is number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= 0 && value <= 0xFFFF_FFFF
}

function timerJitterDistribution(
  values: Record<string, unknown>,
  verdict: string,
): TimerJitterDistribution | null {
  if (!hasTimerResponse(values)) return null
  const counts = TIMER_BUCKET_KEYS.map((key) => (
    validBucketCount(values[key]) ? values[key] : null
  ))
  const completeBuckets = counts.every((count) => count !== null)
  const total = completeBuckets
    ? counts.reduce((sum, count) => sum + (count ?? 0), 0)
    : null
  const successfulResponse =
    (values.status === 0 && values.err_code === 0) || verdict.toLowerCase() === 'pass'
  let warning = ''
  if (!completeBuckets) {
    warning = '统计不完整：桶字段缺失或不是有效的 U32 计数。'
  } else if (successfulResponse && total !== 250) {
    warning = `统计不完整：成功响应的桶合计为 ${total}，预期为 250。`
  }
  return { counts, total, warning }
}

function TimerJitterHistogram({ distribution }: { distribution: TimerJitterDistribution }) {
  const maximum = Math.max(1, ...distribution.counts.filter((count): count is number => count !== null))
  return (
    <section className="panel timer-jitter-histogram" aria-label="定时器抖动分布">
      <header className="panel__header">
        <div>
          <h3>定时器抖动分布</h3>
          <p>相邻实际周期相对 250 µs 期望周期的绝对偏差</p>
        </div>
        <span className="mono-count">{distribution.total === null ? '合计 —' : `合计 ${distribution.total}`}</span>
      </header>
      <div className="timer-jitter-histogram__bars" role="list">
        {TIMER_BUCKET_LABELS.map((label, index) => {
          const count = distribution.counts[index]
          const height = count === null || count === 0
            ? 0
            : Math.max(4, count / maximum * 100)
          return (
            <div className="timer-jitter-histogram__bucket" key={TIMER_BUCKET_KEYS[index]} role="listitem">
              <div className="timer-jitter-histogram__bar-area">
                <i aria-hidden="true" style={{ height: `${height}%` }} />
              </div>
              <strong>{count === null ? '—' : count}</strong>
              <span>{label}</span>
            </div>
          )
        })}
      </div>
      {distribution.warning && <p className="timer-jitter-histogram__warning" role="status">{distribution.warning}</p>}
    </section>
  )
}

export function OverviewPage() {
  const {
    connectionState,
    resetDigitalStimulus,
    setDigitalStimulus,
    snapshot,
  } = useSession()
  const { latestSample } = useTelemetry()
  const values = latestSample?.values ?? {}
  const isTimerJitter = (snapshot.algorithmId || snapshot.descriptor.algorithmId) === TIMER_JITTER_ALGORITHM_ID
  const responseValues = recordValue(snapshot.rawData.responseValues) ?? {}
  const terminalSnapshot = ['finished', 'stopped', 'error'].includes(snapshot.phase)
  const timerValues = hasTimerBucket(values)
    ? values
    : terminalSnapshot && hasTimerBucket(responseValues)
      ? responseValues
      : hasTimerResponse(values)
        ? values
        : terminalSnapshot && hasTimerResponse(responseValues) ? responseValues : {}
  const timerDistribution = isTimerJitter
    ? timerJitterDistribution(timerValues, snapshot.verdict)
    : null
  const measurements = snapshot.descriptor.measurements
  const taskMeasurements = snapshot.descriptor.taskMeasurements
  const displayMeasurements = [...measurements, ...taskMeasurements]
  const hiddenFields = new Set(measurements
    .filter(({ taskVisible }) => taskVisible === false)
    .map(({ id }) => id))
  const numericValues = Object.entries(values).filter((entry): entry is [string, number] => (
    typeof entry[1] === 'number' && !hiddenFields.has(entry[0])
  ))
  const primaryMeasurements = taskMeasurements.length > 0
    ? taskMeasurements
    : measurements.filter(({ primary, taskVisible }) => (
      primary && taskVisible !== false
    ))
  const visibleValues: Array<[string, number]> = primaryMeasurements.length > 0
    ? primaryMeasurements
      .map((measurement): [string, number | undefined] => (
        [measurement.id, measurementNumericValue(measurement, values)]
      ))
      .filter((entry): entry is [string, number] => entry[1] !== undefined)
    : numericValues.slice(0, 6)
  const visibleFields = visibleValues.map(([field]) => field)
  const secondaryValues = numericValues.filter(([field]) => (
    !visibleFields.includes(field) &&
    !(timerDistribution !== null && TIMER_BUCKET_KEYS.includes(field))
  ))
  const testTitle = snapshot.descriptor.title || snapshot.testItemId || snapshot.algorithmId || '当前测试'
  const digitalStimulus = snapshot.digitalStimulus
  const digitalSwitchBits = digitalStimulus.switches.map(({ dutBit }) => dutBit)
  const digitalSwitchIds = digitalStimulus.switches.map(({ switchId }) => switchId)
  const showDigitalStimulus = digitalStimulus.available &&
    digitalStimulus.switches.length > 0 &&
    digitalStimulus.switches.length <= 16 &&
    digitalStimulus.switches.every(({ dutBit, switchId }) => (
      Number.isInteger(dutBit) && dutBit >= 0 && dutBit < 16 && switchId.length > 0
    )) &&
    new Set(digitalSwitchBits).size === digitalStimulus.switches.length &&
    new Set(digitalSwitchIds).size === digitalStimulus.switches.length
  const digitalStimulusTransport = useMemo(() => ({
    set: setDigitalStimulus,
    reset: resetDigitalStimulus,
  }), [resetDigitalStimulus, setDigitalStimulus])
  const digitalStimulusKey = `${snapshot.descriptor.configId}:${digitalStimulus.switches
    .map(({ switchId, dutBit }) => `${switchId}:${dutBit}`)
    .join('|')}`

  return (
    <div className="page-stack overview-page">
      <section className="overview-summary panel">
        <div className="overview-summary__test">
          <h2>{testTitle}</h2>
        </div>
        <div className="status-facts">
          <span><i className={`status-dot status-dot--${connectionState}`} />{connectionStateLabel(connectionState)}</span>
          <span title={snapshot.providerId}><ArrowsLeftRight size={15} />{snapshot.providerId || '等待配置'}</span>
          <span><Pulse size={15} />{formatTimestamp(latestSample?.timestampUs)}</span>
        </div>
        <strong className={`verdict verdict--${snapshot.verdict.toLowerCase() || 'idle'}`}>
          {verdictLabel(snapshot.verdict)}
        </strong>
        <dl className="overview-summary__counts">
          <div><dt>轮次</dt><dd>{snapshot.cycleIndex || 0}</dd></div>
          <div><dt>样本</dt><dd>{snapshot.sampleCount || 0}</dd></div>
          <div><dt>尝试</dt><dd>{snapshot.attempts || 0}</dd></div>
        </dl>
      </section>

      <section className="metric-grid" aria-label="最新状态量">
        {visibleValues.length > 0 ? visibleValues.map(([field, value]) => (
          <MetricCard field={field} key={field} value={value} measurements={displayMeasurements} />
        )) : (
          <div className="empty-state panel metric-grid__empty">
            <Pulse size={22} />
            <h3>等待遥测样本</h3>
          </div>
        )}
      </section>

      {timerDistribution && <TimerJitterHistogram distribution={timerDistribution} />}

      {showDigitalStimulus && (
        <DigitalStimulusPanel
          key={digitalStimulusKey}
          latestSample={latestSample}
          stimulus={digitalStimulus}
          transport={digitalStimulusTransport}
        />
      )}

      {secondaryValues.length > 0 && (
        <section className="panel latest-values-panel">
          <header className="panel__header">
            <h3>其它测量</h3>
            <span className="mono-count">{secondaryValues.length} 项</span>
          </header>
          <div className="value-table">
            {secondaryValues.map(([field, value]) => (
              <div key={field}>
                <span>{fieldLabel(field, measurements)}</span>
                <strong>{formatValue(value, 3)} <small>{fieldUnit(field, measurements)}</small></strong>
              </div>
            ))}
          </div>
        </section>
      )}
    </div>
  )
}

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
import { useSession } from '../features/session/SessionProvider'

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

export function OverviewPage() {
  const {
    connectionState,
    latestSample,
    resetDigitalStimulus,
    setDigitalStimulus,
    snapshot,
  } = useSession()
  const values = latestSample?.values ?? {}
  const numericValues = Object.entries(values).filter((entry): entry is [string, number] => (
    typeof entry[1] === 'number'
  ))
  const numericFields = numericValues.map(([field]) => field)
  const measurements = snapshot.descriptor.measurements
  const primaryFields = measurements.filter(({ primary }) => primary).map(({ id }) => id)
  const visibleFields = (primaryFields.length > 0
    ? primaryFields
    : numericFields.slice(0, 6))
    .filter((field) => typeof values[field] === 'number')
  const secondaryValues = numericValues.filter(([field]) => !visibleFields.includes(field))
  const testTitle = snapshot.descriptor.title || snapshot.testItemId || snapshot.algorithmId || '当前测试'
  const digitalStimulus = snapshot.digitalStimulus
  const showDigitalStimulus = digitalStimulus.available &&
    digitalStimulus.switches.length === 16 &&
    digitalStimulus.switches.every(({ dutBit }) => Number.isInteger(dutBit) && dutBit >= 0 && dutBit < 16)
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
        {visibleFields.length > 0 ? visibleFields.map((field) => (
          <MetricCard field={field} key={field} value={values[field]} measurements={measurements} />
        )) : (
          <div className="empty-state panel metric-grid__empty">
            <Pulse size={22} />
            <h3>等待遥测样本</h3>
          </div>
        )}
      </section>

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

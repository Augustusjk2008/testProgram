import { ChartLine, WarningCircle } from '@phosphor-icons/react'

import {
  parseBoardTestResult,
  type BoardTestPoint,
  type BoardTestResult,
} from '../shared/protocol'
import { formatValue, verdictLabel } from '../shared/format'
import { useSession } from '../features/session/SessionProvider'

type PointStatus = 'pass' | 'fail' | 'error' | 'pending'

interface PlotPoint {
  index: number
  channel: string
  command: number | null
  measured: number | null
  error: number | null
  tolerance: number | null
  status: PointStatus
}

const PWM_COMMAND_FIELDS = [
  'command', 'commanded', 'commandPercent', 'dutyPercent', 'expectedDutyPercent',
  'targetDutyPercent', 'expected', 'setpoint',
]
const PWM_MEASURED_FIELDS = [
  'measured', 'actual', 'measuredPercent', 'measuredDutyPercent', 'actualDutyPercent',
  'readback', 'value',
]
const FEEDBACK_COMMAND_FIELDS = [
  'command', 'commanded', 'commandVoltage', 'inputVoltage', 'targetVoltage',
  'requestedVoltage', 'commandV', 'expectedV', 'expectedVoltage', 'expected', 'setpoint',
]
const FEEDBACK_MEASURED_FIELDS = [
  'measured', 'actual', 'measuredVoltage', 'feedbackVoltage', 'readbackVoltage',
  'measuredV', 'helm_AD_value', 'helmAdValue', 'readback', 'value',
]
const ERROR_FIELDS = ['error', 'errorPercent', 'errorPct', 'errorPercentagePoints', 'errorVoltage', 'errorV', 'difference', 'delta']
const TOLERANCE_FIELDS = ['tolerance', 'tolerancePercent', 'tolerancePct', 'tolerancePercentagePoints', 'toleranceVoltage', 'toleranceV', 'allowedError']
const CHANNEL_FIELDS = ['channel', 'channelIndex', 'channel_id', 'servo', 'axis']

function ownValue(point: BoardTestPoint, fields: readonly string[]): unknown {
  for (const field of fields) {
    if (Object.prototype.hasOwnProperty.call(point, field)) return point[field]
  }
  return undefined
}

function finiteValue(point: BoardTestPoint, fields: readonly string[]): number | null {
  const value = ownValue(point, fields)
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

function displayValue(value: unknown, digits = 3): string {
  if (typeof value === 'number' && Number.isFinite(value)) return formatValue(value, digits)
  if (typeof value === 'string' || typeof value === 'boolean') return String(value)
  if (value === null || value === undefined) return '—'
  try {
    return JSON.stringify(value)
  } catch {
    return '—'
  }
}

function displayMask(value: unknown): string {
  if (typeof value === 'number' && Number.isSafeInteger(value) && value >= 0) {
    return `0x${value.toString(16).toUpperCase().padStart(4, '0')}`
  }
  return displayValue(value)
}

function pointStatus(point: BoardTestPoint): PointStatus {
  const boolean = ownValue(point, ['pass', 'passed', 'ok'])
  if (boolean === true) return 'pass'
  if (boolean === false) return 'fail'
  const verdict = ownValue(point, ['verdict', 'result', 'outcome'])
  if (typeof verdict !== 'string') return 'pending'
  const normalized = verdict.toLowerCase()
  if (normalized === 'pass' || normalized === 'ok') return 'pass'
  if (normalized === 'fail' || normalized === 'failed') return 'fail'
  if (normalized === 'error' || normalized === 'cancelled') return 'error'
  return 'pending'
}

function pointStatusLabel(status: PointStatus): string {
  if (status === 'pass') return '通过'
  if (status === 'fail') return '未通过'
  if (status === 'error') return '错误'
  return '未判定'
}

function channelLabel(point: BoardTestPoint): string {
  const value = ownValue(point, CHANNEL_FIELDS)
  if (typeof value === 'number' && Number.isFinite(value)) return `通道 ${value}`
  if (typeof value === 'string' && value) return `通道 ${value}`
  return '通道 —'
}

function plotPoints(
  points: readonly BoardTestPoint[],
  commandFields: readonly string[],
  measuredFields: readonly string[],
): PlotPoint[] {
  return points.map((point, index) => {
    const command = finiteValue(point, commandFields)
    const measured = finiteValue(point, measuredFields)
    const explicitError = finiteValue(point, ERROR_FIELDS)
    return {
      index,
      channel: channelLabel(point),
      command,
      measured,
      error: explicitError ?? (command === null || measured === null ? null : measured - command),
      tolerance: finiteValue(point, TOLERANCE_FIELDS),
      status: pointStatus(point),
    }
  })
}

function pointLabel(point: PlotPoint): string {
  return point.command === null
    ? `${point.channel} · ${point.index + 1}`
    : `${point.channel} · ${formatValue(point.command, 3)}`
}

function svgPath(values: Array<number | null>, minimum: number, maximum: number): string {
  const width = 720
  const height = 220
  const left = 46
  const right = 18
  const top = 12
  const bottom = 32
  const usableWidth = width - left - right
  const usableHeight = height - top - bottom
  const scale = maximum - minimum || 1
  let path = ''
  let previousWasGap = true
  values.forEach((value, index) => {
    if (value === null) {
      previousWasGap = true
      return
    }
    const x = values.length <= 1 ? left + usableWidth / 2 : left + index * usableWidth / (values.length - 1)
    const y = top + (maximum - value) * usableHeight / scale
    path += `${previousWasGap ? 'M' : 'L'}${x.toFixed(2)},${y.toFixed(2)}`
    previousWasGap = false
  })
  return path
}

function PlotLegend({ name, className }: { name: string; className: string }) {
  return <span className={`board-plot__legend-item ${className}`}><i />{name}</span>
}

function PointPlot({
  title,
  points,
  unit,
}: {
  title: string
  points: PlotPoint[]
  unit: string
}) {
  const command = points.map((point) => point.command)
  const measured = points.map((point) => point.measured)
  const upperTolerance = points.map((point) => (
    point.command === null || point.tolerance === null ? null : point.command + point.tolerance
  ))
  const lowerTolerance = points.map((point) => (
    point.command === null || point.tolerance === null ? null : point.command - point.tolerance
  ))
  const visibleValues = [...command, ...measured, ...upperTolerance, ...lowerTolerance]
    .filter((value): value is number => value !== null)
  if (visibleValues.length === 0) return null

  const rawMinimum = Math.min(...visibleValues)
  const rawMaximum = Math.max(...visibleValues)
  const padding = Math.max((rawMaximum - rawMinimum) * 0.12, unit === '%' ? 1 : 0.05)
  const minimum = rawMinimum - padding
  const maximum = rawMaximum + padding
  const hasTolerance = upperTolerance.some((value) => value !== null)
  const gridLines = Array.from({ length: 5 }, (_, index) => minimum + (maximum - minimum) * index / 4)

  return (
    <article className="board-plot panel">
      <header className="panel__header">
        <h3>{title}</h3>
        <span className="mono-count">{points.length} 点</span>
      </header>
      <div className="board-plot__canvas">
        <svg aria-label={`${title}曲线`} preserveAspectRatio="none" role="img" viewBox="0 0 720 220">
          {gridLines.map((value) => {
            const y = 12 + (maximum - value) * 176 / (maximum - minimum || 1)
            return (
              <g key={value}>
                <line className="board-plot__grid" x1="46" x2="702" y1={y} y2={y} />
                <text className="board-plot__axis" textAnchor="end" x="40" y={y + 3}>{formatValue(value, 3)}</text>
              </g>
            )
          })}
          <line className="board-plot__axis-line" x1="46" x2="702" y1="188" y2="188" />
          <path className="board-plot__tolerance" d={svgPath(upperTolerance, minimum, maximum)} />
          <path className="board-plot__tolerance" d={svgPath(lowerTolerance, minimum, maximum)} />
          <path className="board-plot__command" d={svgPath(command, minimum, maximum)} />
          <path className="board-plot__measured" d={svgPath(measured, minimum, maximum)} />
        </svg>
        <div className="board-plot__legend">
          <PlotLegend className="board-plot__legend-item--command" name="指令" />
          <PlotLegend className="board-plot__legend-item--measured" name="实测" />
          {hasTolerance && <PlotLegend className="board-plot__legend-item--tolerance" name="容差边界" />}
        </div>
      </div>
      <div className="board-point-list" aria-label={`${title}点明细`}>
        {points.map((point) => (
          <div className="board-point-list__row" key={`${point.index}:${pointLabel(point)}`}>
            <span>{pointLabel(point)}</span>
            <strong>指令 {point.command === null ? '—' : `${formatValue(point.command, 3)} ${unit}`}</strong>
            <strong>实测 {point.measured === null ? '—' : `${formatValue(point.measured, 3)} ${unit}`}</strong>
            <strong>误差 {point.error === null ? '—' : `${formatValue(point.error, 3)} ${unit}`}</strong>
            <strong>容差 {point.tolerance === null ? '—' : `±${formatValue(point.tolerance, 3)} ${unit}`}</strong>
            <StatusBadge status={point.status} />
          </div>
        ))}
      </div>
    </article>
  )
}

function StatusBadge({ status }: { status: PointStatus }) {
  return <span className={`board-status board-status--${status}`}>{pointStatusLabel(status)}</span>
}

function resultSummaryValue(summary: BoardTestPoint, fields: readonly string[]): unknown {
  return ownValue(summary, fields)
}

function maximumError(points: readonly PlotPoint[]): number | null {
  let maximum: number | null = null
  for (const point of points) {
    if (point.error === null) continue
    const value = Math.abs(point.error)
    maximum = maximum === null ? value : Math.max(maximum, value)
  }
  return maximum
}

function worstPoint(points: readonly PlotPoint[]): string {
  const worst = points.reduce<PlotPoint | null>((current, point) => {
    if (point.error === null) return current
    if (current === null || Math.abs(point.error) > Math.abs(current.error ?? 0)) return point
    return current
  }, null)
  return worst ? pointLabel(worst) : '—'
}

function SummaryFacts({ result, pwm, feedback, verdict }: {
  result: BoardTestResult
  pwm: PlotPoint[]
  feedback: PlotPoint[]
  verdict: string
}) {
  const allPoints = [...pwm, ...feedback]
  const maxDutyError = finiteValue(result.summary, [
    'maxDutyErrorPercentagePoints', 'maximumDutyErrorPercentagePoints', 'maxDutyError',
  ]) ?? maximumError(pwm)
  const maxFeedbackError = finiteValue(result.summary, [
    'maxFeedbackErrorVolts', 'maximumFeedbackErrorVolts', 'maxFeedbackError',
  ]) ?? maximumError(feedback)
  const worst = resultSummaryValue(result.summary, ['worstPoint', 'worst_point', 'worst']) ?? worstPoint(allPoints)
  const failures = resultSummaryValue(result.summary, ['failedPoints', 'failedPointCount', 'failCount', 'failures'])
  const automaticHelm = result.kind === 'helm_board_test' && result.mode === 'automatic'
  const modeLabel = result.kind === 'do_write'
    ? '用户配置'
    : result.mode === 'automatic' ? '自动' : '手动'
  return (
    <section className="board-summary panel">
      <div>
        <span className="board-summary__eyebrow">{result.kind === 'do_write' ? '数字量输出' : '舵机板级测试'} · {modeLabel}</span>
        <h2>{result.completedPoints} / {result.totalPoints || '—'} 点已完成</h2>
        <p>{verdictLabel(verdict)}</p>
      </div>
      <dl>
        {automaticHelm && <div><dt>PWM 最大误差</dt><dd>{displayValue(maxDutyError)}{maxDutyError === null ? '' : ' pp'}</dd></div>}
        {automaticHelm && <div><dt>反馈最大误差</dt><dd>{displayValue(maxFeedbackError)}{maxFeedbackError === null ? '' : ' V'}</dd></div>}
        {automaticHelm && <div><dt>最差点</dt><dd>{displayValue(worst)}</dd></div>}
        <div><dt>未通过点</dt><dd>{displayValue(failures)}</dd></div>
        <div><dt>最终状态</dt><dd>{verdictLabel(verdict)}</dd></div>
      </dl>
    </section>
  )
}

function DoReadbackCell({
  point,
  expectedFields,
  measuredFields,
}: {
  point: BoardTestPoint
  expectedFields: readonly string[]
  measuredFields: readonly string[]
}) {
  return (
    <div className="board-do-readback">
      <span>指令 {displayValue(ownValue(point, expectedFields))}</span>
      <span>实测 {displayValue(ownValue(point, measuredFields))}</span>
    </div>
  )
}

function DoStepsMatrix({ steps, doWrite }: { steps: readonly BoardTestPoint[]; doWrite: boolean }) {
  if (steps.length === 0) return null
  return (
    <section className="panel board-table-panel">
      <header className="panel__header"><h3>DO 步骤矩阵</h3><span className="mono-count">{steps.length} 步</span></header>
      {doWrite && <p className="board-do-note">PXI-6259 仅自动验证 DO2 / DO1；其余通道仅显示 DUT 完整应用状态，未配置外部回读。</p>}
      <div className="board-table-wrap">
        <table className="board-table">
          <thead><tr><th>步骤</th><th>{doWrite ? '完整 16 位指令掩码' : '请求掩码'}</th><th>{doWrite ? 'DUT 完整应用状态' : '应答状态'}</th><th>{doWrite ? 'DO1（衰减器 / PXI-6259）' : 'DO1（衰减器）'}</th><th>{doWrite ? 'DO2（发送使能 / PXI-6259）' : 'DO2（发送使能）'}</th><th>结果</th></tr></thead>
          <tbody>{steps.map((step, index) => (
            <tr key={`${index}:${displayValue(ownValue(step, ['step', 'index', 'sequence']))}`}>
              <td>{displayValue(ownValue(step, ['step', 'index', 'sequence', 'name']) ?? index + 1)}</td>
              <td>{displayMask(ownValue(step, ['requestedMask', 'requested_mask', 'requestedState', 'requested_state', 'commandedMask', 'commanded_mask', 'commandMask', 'command_mask', 'mask', 'expectedMask', 'expected_mask']))}</td>
              <td>{displayMask(ownValue(step, ['appliedState', 'applied_state', 'appliedMask', 'applied_mask', 'reportedState', 'reported_state', 'responseState', 'response_state']))}</td>
              <td>{doWrite ? (
                <DoReadbackCell
                  expectedFields={['expectedAttenuator', 'expected_attenuator', 'expectedDo1']}
                  measuredFields={['measuredAttenuator', 'measured_attenuator', 'do1', 'readDo1', 'do1Readback', 'observedDo1']}
                  point={step}
                />
              ) : displayValue(ownValue(step, ['do1', 'readDo1', 'do1Readback', 'observedDo1', 'measuredAttenuator', 'measured_attenuator', 'expectedAttenuator', 'expected_attenuator', 'expectedDo1']))}</td>
              <td>{doWrite ? (
                <DoReadbackCell
                  expectedFields={['expectedTxEnable', 'expected_tx_enable', 'expectedDo2']}
                  measuredFields={['measuredTxEnable', 'measured_tx_enable', 'do2', 'readDo2', 'do2Readback', 'observedDo2']}
                  point={step}
                />
              ) : displayValue(ownValue(step, ['do2', 'readDo2', 'do2Readback', 'observedDo2', 'measuredTxEnable', 'measured_tx_enable', 'expectedTxEnable', 'expected_tx_enable', 'expectedDo2']))}</td>
              <td><StatusBadge status={pointStatus(step)} /></td>
            </tr>
          ))}</tbody>
        </table>
      </div>
    </section>
  )
}

function directionChannel(point: BoardTestPoint, zeroBased: boolean): number | null {
  const value = finiteValue(point, CHANNEL_FIELDS)
  if (value === null || !Number.isInteger(value)) return null
  return zeroBased ? value + 1 : value
}

function aggregateDirectionCell(
  points: readonly BoardTestPoint[],
  channel: number,
  direction: number,
): { point?: BoardTestPoint; measured: number | null } | null {
  const aggregate = points.filter((point) => (
    finiteValue(point, ['expectedMask']) !== null && Array.isArray(point.measuredVoltages)
  ))
  if (aggregate.length === 0) return null
  const expectedMask = direction === 0 ? 0 : 1 << (channel - 1)
  const point = aggregate.find((candidate) => finiteValue(candidate, ['expectedMask']) === expectedMask)
  if (!point || !Array.isArray(point.measuredVoltages)) return { measured: null }
  const value = point.measuredVoltages[channel - 1]
  return { point, measured: typeof value === 'number' && Number.isFinite(value) ? value : null }
}

function DirectionMatrix({ points }: { points: readonly BoardTestPoint[] }) {
  if (points.length === 0) return null
  const zeroBased = points.some((point) => finiteValue(point, CHANNEL_FIELDS) === 0)
  const cells = new Map<string, BoardTestPoint>()
  points.forEach((point) => {
    const channel = directionChannel(point, zeroBased)
    const direction = finiteValue(point, ['direction', 'directionBit', 'commandDirection', 'expectedDirection'])
    if (channel !== null && (direction === 0 || direction === 1)) cells.set(`${channel}:${direction}`, point)
  })
  return (
    <section className="panel board-table-panel">
      <header className="panel__header"><h3>方向 0 / 1 矩阵</h3><span className="mono-count">{points.length} 点</span></header>
      <div className="board-direction-grid" role="table" aria-label="方向 0 / 1 矩阵">
        <div className="board-direction-grid__head">通道</div><div className="board-direction-grid__head">方向 0</div><div className="board-direction-grid__head">方向 1</div>
        {[1, 2, 3, 4].map((channel) => (
          <div className="board-direction-grid__row" key={channel}>
            <strong>通道 {channel}</strong>
            {[0, 1].map((direction) => {
              const aggregate = aggregateDirectionCell(points, channel, direction)
              const point = aggregate?.point ?? cells.get(`${channel}:${direction}`)
              const measured = aggregate?.measured ?? (point
                ? finiteValue(point, ['measured', 'measuredVoltage', 'feedbackVoltage', 'readback', 'value'])
                : null)
              const status = aggregate && measured !== null
                ? (direction === 0 ? measured <= 0.8 : measured >= 3.8) ? 'pass' : 'fail'
                : point ? pointStatus(point) : 'pending'
              return (
                <div className="board-direction-cell" key={direction}>
                  <StatusBadge status={status} />
                  <span>实测 {measured === null ? '—' : `${formatValue(measured, 3)} V`}</span>
                </div>
              )
            })}
          </div>
        ))}
      </div>
    </section>
  )
}

function KeyValueGrid({ entries, title }: { entries: readonly [string, unknown][]; title: string }) {
  if (entries.length === 0) return null
  return (
    <section className="panel board-key-values-panel">
      <header className="panel__header"><h3>{title}</h3><span className="mono-count">{entries.length} 项</span></header>
      <div className="board-key-values">
        {entries.map(([key, value]) => <div key={key}><span>{key}</span><strong>{displayValue(value)}</strong></div>)}
      </div>
    </section>
  )
}

function ManualResponse({ response }: { response?: BoardTestPoint }) {
  return response ? <KeyValueGrid entries={Object.entries(response)} title="一次响应表" /> : (
    <section className="empty-state panel board-test-empty"><WarningCircle size={26} /><h3>等待协议响应</h3></section>
  )
}

export function BoardTestPage() {
  const { snapshot } = useSession()
  const result = parseBoardTestResult(snapshot.rawData.boardTest)
  if (!result) {
    return (
      <div className="page-stack board-test-page">
        <section className="empty-state panel board-test-empty">
          <ChartLine size={30} />
          <h3>等待板级测试结果</h3>
          <p>运行完成后，此处显示步骤、测量点与最终汇总。</p>
        </section>
      </div>
    )
  }

  const pwm = plotPoints(result.pwmPoints, PWM_COMMAND_FIELDS, PWM_MEASURED_FIELDS)
  const feedback = plotPoints(result.feedbackPoints, FEEDBACK_COMMAND_FIELDS, FEEDBACK_MEASURED_FIELDS)
  return (
    <div className="page-stack board-test-page">
      <SummaryFacts feedback={feedback} pwm={pwm} result={result} verdict={snapshot.verdict} />
      <KeyValueGrid entries={Object.entries(result.summary)} title="结果摘要" />
      {result.kind === 'do_write' ? (
        <DoStepsMatrix doWrite steps={result.doSteps} />
      ) : result.mode === 'manual' ? (
        <ManualResponse response={result.manualResponse} />
      ) : <>
          <DoStepsMatrix doWrite={false} steps={result.doSteps} />
          <DirectionMatrix points={result.directionPoints} />
          <PointPlot points={pwm} title="PWM 指令、实测、误差与容差" unit="%" />
          <PointPlot points={feedback} title="反馈电压指令、读回、误差与容差" unit="V" />
        </>}
    </div>
  )
}

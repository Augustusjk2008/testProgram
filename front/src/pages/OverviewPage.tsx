import {
  ArrowsLeftRight,
  CheckCircle,
  Cpu,
  Gauge,
  Pulse,
  Thermometer,
} from '@phosphor-icons/react'

import {
  fieldLabel,
  fieldUnit,
  formatDuration,
  formatTimestamp,
  formatValue,
} from '../shared/format'
import { useSession } from '../features/session/SessionProvider'

const PRIMARY_FIELDS = [
  'cpu_usage',
  'mem_usage',
  'cpu_temp',
  'rk_temp',
  'k7_temp',
  'power_on_sec',
]

function MetricCard({ field, value }: { field: string; value: unknown }) {
  const isTemperature = field.includes('temp')
  const isRuntime = field === 'power_on_sec'
  const Icon = isTemperature ? Thermometer : field.includes('usage') ? Gauge : Cpu
  return (
    <article className="metric-card">
      <div className="metric-card__top">
        <span>{fieldLabel(field)}</span>
        <Icon size={18} />
      </div>
      <div className="metric-card__value">
        {isRuntime ? formatDuration(value) : formatValue(value)}
        {!isRuntime && <small>{fieldUnit(field)}</small>}
      </div>
      <code>{field}</code>
    </article>
  )
}

export function OverviewPage() {
  const { connectionState, latestSample, snapshot } = useSession()
  const values = latestSample?.values ?? {}
  const visibleFields = PRIMARY_FIELDS.filter((field) => typeof values[field] === 'number')

  return (
    <div className="page-stack overview-page">
      <section className="overview-lead panel">
        <div className="overview-lead__status">
          <span className="eyebrow">SYSTEM STATUS / LIVE</span>
          <h2>{snapshot.testItemId || 'SYSTEM_STATUS'}</h2>
          <p>
            当前演示通过 PC 主机发出状态指令，并将每轮设备反馈实时投影到浏览器。
            设备自主持续回告保持为独立运行能力。
          </p>
          <div className="status-facts">
            <span><i className={`status-dot status-dot--${connectionState}`} />{connectionState}</span>
            <span><ArrowsLeftRight size={15} />{snapshot.providerId || '等待配置'}</span>
            <span><Pulse size={15} />{formatTimestamp(latestSample?.timestampUs)}</span>
          </div>
        </div>
        <div className="overview-lead__verdict">
          <span className="eyebrow">LAST VERDICT</span>
          <strong className={`verdict verdict--${snapshot.verdict.toLowerCase() || 'idle'}`}>
            {snapshot.verdict || 'STANDBY'}
          </strong>
          <dl>
            <div><dt>当前轮次</dt><dd>{snapshot.cycleIndex || 0}</dd></div>
            <div><dt>采样总数</dt><dd>{snapshot.sampleCount || 0}</dd></div>
            <div><dt>尝试次数</dt><dd>{snapshot.attempts || 0}</dd></div>
          </dl>
        </div>
      </section>

      <section className="metric-grid" aria-label="最新状态量">
        {visibleFields.length > 0 ? visibleFields.map((field) => (
          <MetricCard field={field} key={field} value={values[field]} />
        )) : (
          <div className="empty-state panel metric-grid__empty">
            <Pulse size={30} />
            <h3>等待第一条遥测样本</h3>
            <p>在上方完成“加载配置 → 连接设备 → 开始测试”，这里会显示真实 SYSTEM_STATUS 数值。</p>
          </div>
        )}
      </section>

      <div className="overview-columns">
        <section className="panel pipeline-panel">
          <header className="panel__header">
            <div>
              <span className="eyebrow">DATA PATH</span>
              <h3>双向测试链路</h3>
            </div>
            <ArrowsLeftRight size={20} />
          </header>
          <ol className="pipeline">
            {[
              ['01', 'PC 调度', snapshot.runMode === 'pc_periodic' ? `${snapshot.intervalMs} ms` : snapshot.runMode],
              ['02', 'WebSocket', connectionState],
              ['03', '应用 / BIZ', snapshot.testState],
              ['04', 'HAL 控制通道', snapshot.providerId || '未选择'],
              ['05', 'DUT 反馈', latestSample ? '已采集' : '等待中'],
            ].map(([index, title, state]) => (
              <li key={index}>
                <b>{index}</b>
                <span><strong>{title}</strong><small>{state}</small></span>
                {state === '已采集' && <CheckCircle className="pipeline__ok" size={17} weight="fill" />}
              </li>
            ))}
          </ol>
        </section>

        <section className="panel latest-values-panel">
          <header className="panel__header">
            <div>
              <span className="eyebrow">LATEST FRAME</span>
              <h3>最新数值全集</h3>
            </div>
            <span className="mono-count">{Object.keys(values).length} fields</span>
          </header>
          {Object.keys(values).length === 0 ? (
            <div className="compact-empty">尚无解码数据。</div>
          ) : (
            <div className="value-table">
              {Object.entries(values)
                .filter(([, value]) => typeof value === 'number')
                .map(([field, value]) => (
                  <div key={field}>
                    <span>{fieldLabel(field)}<code>{field}</code></span>
                    <strong>{formatValue(value, 3)} <small>{fieldUnit(field)}</small></strong>
                  </div>
                ))}
            </div>
          )}
        </section>
      </div>
    </div>
  )
}

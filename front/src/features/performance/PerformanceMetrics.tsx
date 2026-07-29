import type { AnalysisChannelSummary, AnalysisMetric } from '../../shared/protocol'

function formatMetricValue(metric: AnalysisMetric): string {
  if (metric.value === null) return '—'
  return metric.value.toLocaleString('zh-CN', { maximumFractionDigits: 4 })
}

function MetricGroup({ title, metrics }: { title: string; metrics: readonly AnalysisMetric[] }) {
  if (metrics.length === 0) return null
  return (
    <section className="performance-metric-group">
      <h4>{title}</h4>
      <div className="performance-metric-grid">
        {metrics.map((metric) => (
          <article className="performance-metric" key={metric.key} title={metric.detail}>
            <span>{metric.label}</span>
            <strong>{formatMetricValue(metric)} <small>{metric.unit}</small></strong>
            <i>{metric.status}</i>
            {metric.detail && <p>{metric.detail}</p>}
          </article>
        ))}
      </div>
    </section>
  )
}

export function PerformanceMetrics({ channels }: { channels: readonly AnalysisChannelSummary[] }) {
  const visibleChannels = channels.filter(({ enabled }) => enabled)
  if (visibleChannels.length === 0) return null
  return (
    <section className="performance-metrics panel">
      <header className="panel__header">
        <h3>性能指标</h3>
        <span>仅展示后端离线分析投影</span>
      </header>
      <div className="performance-metrics__body">
        {visibleChannels.map((channel) => (
          <section className="performance-channel-metrics" key={channel.channel}>
            <header>
              <strong>舵 {channel.channel + 1}</strong>
              <span className={`performance-channel-status performance-channel-status--${channel.status}`}>
                {channel.status}
              </span>
            </header>
            {channel.warnings.length > 0 && (
              <ul className="performance-warnings">
                {channel.warnings.map((warning, index) => <li key={`${warning}:${index}`}>{warning}</li>)}
              </ul>
            )}
            {(channel.message || channel.reasonCode) && (
              <p className="performance-channel-reason">
                {channel.message || channel.reasonCode}
                {channel.message && channel.reasonCode ? `（${channel.reasonCode}）` : ''}
              </p>
            )}
            <MetricGroup metrics={channel.commonMetrics} title="通用指标" />
            <MetricGroup metrics={channel.waveformMetrics} title="波形专用指标" />
          </section>
        ))}
      </div>
    </section>
  )
}

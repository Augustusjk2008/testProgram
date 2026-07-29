import { ChartLine, WarningCircle } from '@phosphor-icons/react'
import { useEffect, useMemo, useRef, useState } from 'react'

import {
  analysisIdentityFromSnapshot,
  analysisIdentityKey,
  analysisStageLabel,
  isAnalysisTerminal,
} from '../features/performance/analysis-session-state'
import { BodePlot } from '../features/performance/BodePlot'
import type { BodeCurve, FrequencyUnit } from '../features/performance/bode-chart-model'
import { PerformanceMetrics } from '../features/performance/PerformanceMetrics'
import { isPerformanceCapabilityEnabled } from '../features/performance/performance-navigation'
import { useSession } from '../features/session/SessionProvider'
import type { AnalysisChannel, AnalysisChannelSummary } from '../shared/protocol'

function scalarSourceEntries(sourceSummary: Record<string, unknown>): Array<[string, string]> {
  return Object.entries(sourceSummary).flatMap(([key, value]) => {
    if (typeof value === 'number' && Number.isFinite(value)) return [[key, value.toLocaleString('zh-CN')]]
    if (typeof value === 'string' || typeof value === 'boolean') return [[key, String(value)]]
    return []
  })
}

function analysisOutcomeMessage(state: string, message: string, reasonCode: string): string {
  if (message) return message
  let fallback = ''
  if (state === 'partial') fallback = '只覆盖了部分可计算数据；未覆盖的指标和频点会保持为空。'
  if (state === 'unavailable') fallback = '输入不足或无有效激励，未生成可用性能结果。'
  if (state === 'failed') fallback = '后处理基础设施或结果存储失败，采集 Pass/Fail 未被改写。'
  if (state === 'cancelled') fallback = '连接关闭或应用收尾已取消尚未完成的分析。'
  return fallback && reasonCode ? `${fallback}（${reasonCode}）` : fallback
}

function firstSourceValue(sourceSummary: Record<string, unknown>, keys: string[]): string {
  for (const key of keys) {
    const value = sourceSummary[key]
    if (typeof value === 'number' && Number.isFinite(value)) return value.toLocaleString('zh-CN', { maximumFractionDigits: 4 })
    if (typeof value === 'string' && value) return value
  }
  return '—'
}

function scalarRunParameters(parameters: Record<string, unknown>): Array<[string, string]> {
  return Object.entries(parameters).flatMap(([key, value]) => {
    if (typeof value === 'number' && Number.isFinite(value)) return [[key, value.toLocaleString('zh-CN', { maximumFractionDigits: 4 })]]
    if (typeof value === 'string' || typeof value === 'boolean') return [[key, String(value)]]
    return []
  })
}

export function PerformancePage() {
  const {
    analysisResultErrors,
    analysisResultLoading,
    analysisResults,
    fetchAnalysisResult,
    snapshot,
  } = useSession()
  const analysis = snapshot.analysis
  const capability = snapshot.descriptor.postRunAnalysis
  const identity = useMemo(() => analysisIdentityFromSnapshot(analysis), [analysis])
  const identityKey = identity ? analysisIdentityKey(identity) : ''
  const [selectedChannels, setSelectedChannels] = useState<AnalysisChannel[]>([])
  const [frequencyUnit, setFrequencyUnit] = useState<FrequencyUnit>('hz')
  const selectedIdentityKeyRef = useRef('')
  const initializedSelectionKeyRef = useRef('')
  const summaries = useMemo(
    () => [...analysis.channelSummaries].sort((left, right) => left.channel - right.channel),
    [analysis.channelSummaries],
  )

  useEffect(() => {
    if (!identity) {
      selectedIdentityKeyRef.current = ''
      initializedSelectionKeyRef.current = ''
      setSelectedChannels([])
      return
    }
    if (selectedIdentityKeyRef.current !== identityKey) {
      selectedIdentityKeyRef.current = identityKey
      initializedSelectionKeyRef.current = ''
      setSelectedChannels([])
    }
    if (initializedSelectionKeyRef.current === identityKey) return
    const firstBodeChannel = summaries.find(({ enabled, bodeAvailable }) => enabled && bodeAvailable)
    const firstEnabledChannel = summaries.find(({ enabled }) => enabled)
    if (!firstBodeChannel && !firstEnabledChannel) return
    initializedSelectionKeyRef.current = identityKey
    setSelectedChannels(firstBodeChannel ? [firstBodeChannel.channel] : firstEnabledChannel ? [firstEnabledChannel.channel] : [])
  }, [identity, identityKey, summaries])

  const selectedKey = selectedChannels.join(',')
  useEffect(() => {
    if (!identity || !isAnalysisTerminal(analysis.state)) return
    let disposed = false
    const selected = [...selectedChannels].sort((left, right) => left - right)
    void (async () => {
      for (const channel of selected) {
        const summary = summaries.find((item) => item.channel === channel)
        if (!summary?.bodeAvailable || analysisResults[channel] || analysisResultLoading[channel]) continue
        try {
          await fetchAnalysisResult(identity, channel)
        } catch {
          // The per-channel error is retained in SessionProvider without changing global busy state.
        }
        if (disposed) return
      }
    })()
    return () => { disposed = true }
  }, [analysis.state, analysisResultLoading, analysisResults, fetchAnalysisResult, identity, selectedKey, selectedChannels, summaries])

  const selectedSummaries = summaries.filter(({ channel }) => selectedChannels.includes(channel))
  const curves = selectedChannels.flatMap((channel): BodeCurve[] => {
    const result = analysisResults[channel]
    if (!result?.channelSummary.bodeAvailable) return []
    return [{
      channel,
      label: `舵 ${channel + 1}`,
      bode: result.bode,
      metrics: [...result.channelSummary.commonMetrics, ...result.channelSummary.waveformMetrics],
    }]
  })
  const sourceEntries = scalarSourceEntries(analysis.sourceSummary)
  const runParameterEntries = scalarRunParameters(snapshot.effectiveRunParameters)
  const outcomeMessage = analysisOutcomeMessage(
    analysis.state,
    analysis.message,
    analysis.reasonCode,
  )
  const samplingFrequency = firstSourceValue(analysis.sourceSummary, [
    'samplingFrequencyHz', 'sampling_frequency_hz', 'sampleRateHz', 'sample_rate_hz',
  ])
  const effectiveDuration = firstSourceValue(analysis.sourceSummary, [
    'analysisDurationS', 'analysis_duration_s', 'rawDurationS', 'raw_duration_s', 'durationS',
  ])
  const enabledChannels = summaries.filter(({ enabled }) => enabled).map(({ channel }) => `舵 ${channel + 1}`).join('、') || '—'

  function toggleChannel(channel: AnalysisChannel) {
    setSelectedChannels((current) => (
      current.includes(channel)
        ? current.filter((item) => item !== channel)
        : [...current, channel].sort((left, right) => left - right)
    ))
  }

  if (!isPerformanceCapabilityEnabled(capability)) {
    return (
      <div className="page-stack performance-page">
        <section className="empty-state panel performance-empty">
          <ChartLine size={30} />
          <h3>当前测试不支持性能分析</h3>
          <p>请选择声明后处理能力的舵机连续测试。</p>
        </section>
      </div>
    )
  }

  return (
    <div className="page-stack performance-page">
      <section className="performance-summary panel">
        <div>
          <span className="performance-summary__eyebrow">POST-RUN ANALYSIS</span>
          <h2>舵机性能分析</h2>
        </div>
        <dl>
          <div><dt>任务 ID</dt><dd title={analysis.taskId || snapshot.taskId}>{analysis.taskId || snapshot.taskId || '—'}</dd></div>
          <div><dt>分析代次</dt><dd>{analysis.taskId ? analysis.analysisGeneration : '—'}</dd></div>
          <div><dt>分析器</dt><dd>{analysis.analyzerId || capability.analyzerId || '—'} · v{analysis.schemaVersion || capability.schemaVersion || '—'}</dd></div>
          <div><dt>运行波形</dt><dd>{String(snapshot.effectiveRunParameters.waveform ?? '—')}</dd></div>
          <div><dt>启用通道</dt><dd>{enabledChannels}</dd></div>
          <div><dt>采样率</dt><dd>{samplingFrequency === '—' ? '—' : `${samplingFrequency} Hz`}</dd></div>
          <div><dt>有效时长</dt><dd>{effectiveDuration === '—' ? '—' : `${effectiveDuration} s`}</dd></div>
        </dl>
      </section>

      <section className="analysis-progress panel" aria-live="polite">
        <header className="panel__header">
          <h3>分析状态</h3>
          <strong className={`analysis-state analysis-state--${analysis.state}`}>{analysisStageLabel(analysis.state, analysis.stage)}</strong>
        </header>
        <div className="analysis-progress__body">
          <div className="analysis-progress__bar"><i style={{ width: `${Math.max(0, Math.min(100, analysis.progress))}%` }} /></div>
          <span>{analysis.message || analysisStageLabel(analysis.state, analysis.stage)}</span>
          {outcomeMessage && (
            <p className="analysis-outcome"><WarningCircle size={17} />{outcomeMessage}</p>
          )}
        </div>
      </section>

      {(sourceEntries.length > 0 || analysis.resultFilePath || analysis.diagnosticInputFilePath) && (
        <section className="performance-source panel">
          <header className="panel__header"><h3>分析输入与产物</h3></header>
          <div className="performance-source__body">
            {sourceEntries.map(([key, value]) => <span key={key}><small>{key}</small><strong>{value}</strong></span>)}
            {analysis.resultFilePath && <span><small>结果文件</small><code title={analysis.resultFilePath}>{analysis.resultFilePath}</code></span>}
            {analysis.diagnosticInputFilePath && <span><small>诊断输入</small><code title={analysis.diagnosticInputFilePath}>{analysis.diagnosticInputFilePath}</code></span>}
          </div>
        </section>
      )}

      {runParameterEntries.length > 0 && (
        <section className="performance-source panel">
          <header className="panel__header"><h3>生效运行参数</h3></header>
          <div className="performance-source__body">
            {runParameterEntries.map(([key, value]) => <span key={key}><small>{key}</small><strong>{value}</strong></span>)}
          </div>
        </section>
      )}

      <section className="performance-channel-picker panel">
        <header className="panel__header">
          <h3>查看通道</h3>
          <div className="performance-unit-toggle" role="group" aria-label="频率显示单位">
            <button className={frequencyUnit === 'hz' ? 'is-active' : ''} onClick={() => setFrequencyUnit('hz')} type="button">Hz</button>
            <button className={frequencyUnit === 'rad/s' ? 'is-active' : ''} onClick={() => setFrequencyUnit('rad/s')} type="button">rad/s</button>
          </div>
        </header>
        <div className="performance-channel-picker__body">
          {summaries.length === 0 ? <span>等待后端发布通道摘要。</span> : summaries.map((summary) => (
            <label className={summary.enabled ? '' : 'is-disabled'} key={summary.channel}>
              <input
                checked={selectedChannels.includes(summary.channel)}
                disabled={!summary.enabled}
                onChange={() => toggleChannel(summary.channel)}
                type="checkbox"
              />
              <span>舵 {summary.channel + 1}</span>
              <i>{summary.enabled ? summary.status : '未参与'}</i>
              {summary.bodeAvailable && <small>伯德 {summary.bodePointCount} 点</small>}
            </label>
          ))}
        </div>
      </section>

      <PerformanceMetrics channels={selectedSummaries} />

      {selectedChannels.some((channel) => analysisResultLoading[channel]) && (
        <section className="performance-loading panel">正在按通道读取后端伯德投影…</section>
      )}
      {selectedChannels.map((channel) => analysisResultErrors[channel]).filter(Boolean).map((error, index) => (
        <section className="performance-fetch-error" key={`${error}:${index}`}><WarningCircle size={17} />{error}</section>
      ))}
      {curves.length > 0 ? <BodePlot curves={curves} frequencyUnit={frequencyUnit} /> : (
        isAnalysisTerminal(analysis.state) && selectedChannels.length > 0 && (
          <section className="empty-state panel performance-empty">
            <ChartLine size={28} />
            <h3>当前选择没有可用伯德图</h3>
            <p>非扫频波形或未覆盖频点不会生成频响曲线。</p>
          </section>
        )
      )}
    </div>
  )
}

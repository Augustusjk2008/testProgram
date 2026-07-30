import {
  ArrowClockwise,
  Pause,
  Play,
  Plug,
  Stop as StopIcon,
  WarningCircle,
} from '@phosphor-icons/react'
import { useEffect, useMemo, useState } from 'react'

import type { RunMode, TestRunOptions } from '../../shared/protocol'
import { phaseLabel } from '../../shared/format'
import { removeLocalStorageValue, setLocalStorageValue } from '../../shared/storage'
import {
  analysisStageLabel,
  isAnalysisBlockingWrites,
} from '../performance/analysis-session-state'
import { normalizeRunOptionsForStart, validatePcPeriodicOptions } from './run-options'
import {
  loadRunParameterValues,
  persistableRunParameterValues,
  runParameterStorageKey,
  validateRunParameterValues,
  type RunParameterValues,
} from './run-parameters'
import { RunParameterEditor } from './RunParameterEditor'
import { useSession, useTelemetry } from './SessionProvider'

const RUN_OPTIONS_KEY = 'hwtest.run-options.v1'

function loadRunOptions(): TestRunOptions {
  try {
    const stored = window.localStorage.getItem(RUN_OPTIONS_KEY)
    if (stored) {
      const parsed = JSON.parse(stored) as Partial<TestRunOptions>
      return {
        mode: parsed.mode ?? 'pc_periodic',
        intervalMs: parsed.intervalMs ?? 500,
        maxCycles: parsed.maxCycles ?? 0,
        saveData: parsed.saveData === true,
        algorithmParameters: {},
      }
    }
  } catch {
    // Ignore browser storage restrictions and use safe defaults.
  }
  return {
    mode: 'pc_periodic', intervalMs: 500, maxCycles: 0, saveData: false,
    algorithmParameters: {},
  }
}

const MODE_LABELS: Array<{ mode: RunMode; title: string }> = [
  { mode: 'single', title: '单次' },
  { mode: 'pc_periodic', title: 'PC 周期' },
  { mode: 'device_stream', title: '设备持续' },
]

function finiteNumber(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

function isSweepWaveform(value: unknown): boolean {
  return value === 4 || (typeof value === 'string' && value.toLowerCase().includes('sweep'))
}

function TelemetryStatus() {
  const { telemetryStats } = useTelemetry()
  return (
    <div className="run-console__telemetry-stats" aria-label="浏览器遥测统计">
      <span>本次已接收 {telemetryStats.receivedCount.toLocaleString('zh-CN')}</span>
      <span>当前缓存 {telemetryStats.retainedCount.toLocaleString('zh-CN')}</span>
      <span>已淘汰 {telemetryStats.evictedCount.toLocaleString('zh-CN')}</span>
      <span>
        序号状态：{telemetryStats.sequenceStatus === 'continuous'
          ? '连续'
          : telemetryStats.sequenceStatus === 'reconnect_incomplete'
            ? '重连后不完整'
            : '存在缺口'}
      </span>
    </div>
  )
}

function AnalysisRunHint({ activeRunParameters }: { activeRunParameters: Record<string, unknown> }) {
  const { latestSample } = useTelemetry()
  const sweepDurationS = finiteNumber(activeRunParameters.sweep_duration_s)
  const maxDelayMs = finiteNumber(activeRunParameters.maxDelayMs) ??
    finiteNumber(activeRunParameters.max_delay_ms) ?? 100
  const observedStreamSeconds = latestSample?.streamElapsedUs === undefined
    ? null
    : latestSample.streamElapsedUs / 1_000_000
  const sweepReached = isSweepWaveform(activeRunParameters.waveform) &&
    sweepDurationS !== null && observedStreamSeconds !== null &&
    observedStreamSeconds >= sweepDurationS + maxDelayMs / 1000

  return (
    <div className="analysis-run-hint" role="status">
      <strong>停止后自动计算</strong>
      <span>设备流：{formatObservedStreamDuration(latestSample?.streamElapsedUs)}</span>
      {sweepReached && (
        <span>理论时长已达到，可手动停止；完整性将在停止后确认</span>
      )}
    </div>
  )
}

function formatObservedStreamDuration(streamElapsedUs: number | undefined): string {
  if (streamElapsedUs === undefined || !Number.isSafeInteger(streamElapsedUs) || streamElapsedUs < 0) return '等待设备流'
  return `${(streamElapsedUs / 1_000_000).toLocaleString('zh-CN', { maximumFractionDigits: 3 })} s`
}

export function RunControlBar() {
  const {
    actionError,
    busyAction,
    connectionState,
    connect,
    invoke,
    snapshot,
    selectedConfigId,
    start,
    testConfigs,
    testConfigsReady,
  } = useSession()
  const [options, setOptions] = useState<TestRunOptions>(loadRunOptions)

  const active = ['running', 'paused', 'stopping'].includes(snapshot.phase)
  const analysisBlockingWrites = isAnalysisBlockingWrites(snapshot.analysis)
  const testChangeBlocked = ['running', 'paused', 'stopping', 'preparing'].includes(snapshot.phase) || analysisBlockingWrites
  const canStart = ['ready', 'finished', 'stopped'].includes(snapshot.phase) && !analysisBlockingWrites
  const testTitle = snapshot.descriptor.title || snapshot.descriptor.productName || '当前测试'
  const selectedTestId = snapshot.descriptor.configId || selectedConfigId
  const hasRunModeCapabilities = snapshot.descriptor.supportedRunModes.length > 0
  const supportedModes: RunMode[] = hasRunModeCapabilities
    ? snapshot.descriptor.supportedRunModes
    : ['single']
  const modeOptions = MODE_LABELS.filter(({ mode }) => supportedModes.includes(mode))
  const loadingConfig = !testConfigsReady || busyAction === 'load' || busyAction === 'selectTest'
  const periodicError = useMemo(() => {
    if (options.mode !== 'pc_periodic') return ''
    return validatePcPeriodicOptions(options.intervalMs, options.maxCycles)
  }, [options])
  const parameterError = useMemo(
    () => validateRunParameterValues(snapshot.descriptor, options.algorithmParameters),
    [options.algorithmParameters, snapshot.descriptor],
  )
  const unsupported = hasRunModeCapabilities && !supportedModes.includes(options.mode)
  const controlError = periodicError || parameterError || (unsupported ? `${testTitle}不支持当前运行模式` : '') || snapshot.dataSaveError || actionError
  const activeRunParameters = Object.keys(snapshot.effectiveRunParameters).length > 0
    ? snapshot.effectiveRunParameters
    : options.algorithmParameters
  const helmBoardAutomatic =
    (snapshot.algorithmId || snapshot.descriptor.algorithmId) === 'mbddf.helm_board_test' &&
    Number(options.algorithmParameters.test_mode ??
      snapshot.descriptor.runParameterDefaults.test_mode ?? 0) === 0

  useEffect(() => {
    const descriptor = snapshot.descriptor
    if (!descriptor.configId || !descriptor.runParameterSchemaVersion) {
      setOptions((current) => ({ ...current, algorithmParameters: {} }))
      return
    }
    let stored: string | null = null
    try {
      stored = window.localStorage.getItem(runParameterStorageKey(descriptor))
    } catch {
      // Browser privacy settings may disable file:// storage.
    }
    const algorithmParameters = loadRunParameterValues(descriptor, stored)
    setOptions((current) => ({ ...current, algorithmParameters }))
  }, [snapshot.descriptor.configId, snapshot.descriptor.runParameterSchemaVersion])

  useEffect(() => {
    if (!hasRunModeCapabilities) return
    if (supportedModes.includes(options.mode)) return
    const mode = supportedModes[0] ?? 'single'
    const next = { ...options, mode }
    setOptions(next)
    setLocalStorageValue(RUN_OPTIONS_KEY, JSON.stringify({
      mode: next.mode,
      intervalMs: next.intervalMs,
      maxCycles: next.maxCycles,
      saveData: next.saveData,
    }))
  }, [hasRunModeCapabilities, options, supportedModes])

  function saveOptions(next: TestRunOptions) {
    setOptions(next)
    setLocalStorageValue(RUN_OPTIONS_KEY, JSON.stringify({
      mode: next.mode,
      intervalMs: next.intervalMs,
      maxCycles: next.maxCycles,
      saveData: next.saveData,
    }))
  }

  function saveRunParameters(values: RunParameterValues) {
    setOptions((current) => ({ ...current, algorithmParameters: values }))
    const descriptor = snapshot.descriptor
    if (descriptor.configId && descriptor.runParameterSchemaVersion) {
      const storageKey = runParameterStorageKey(descriptor)
      const persisted = persistableRunParameterValues(descriptor, values)
      if (Object.keys(persisted).length === 0) {
        removeLocalStorageValue(storageKey)
      } else {
        setLocalStorageValue(storageKey, JSON.stringify(persisted))
      }
    }
  }

  function execute(action: Parameters<typeof invoke>[0], params?: Record<string, unknown>) {
    void invoke(action, params).catch(() => undefined)
  }

  function beginRun() {
    if (periodicError || parameterError || unsupported) return
    void start(normalizeRunOptionsForStart(options)).catch(() => undefined)
  }

  return (
    <section className="run-console" aria-label="全局测试运行控制">
      <div className="run-console__test-picker">
        <select
          aria-label="测试项目"
          disabled={testChangeBlocked || busyAction !== null || !testConfigsReady || testConfigs.length === 0}
          id="test-config-select"
          onChange={(event) => {
            if (event.target.value) execute('selectTest', { configId: event.target.value })
          }}
          value={selectedTestId}
        >
          {testConfigs.length === 0 ? (
            <option value={selectedTestId}>{selectedTestId ? '当前配置' : '等待配置'}</option>
          ) : testConfigs.map((test) => (
            <option key={test.configId} value={test.configId}>{test.title}</option>
          ))}
        </select>
      </div>

      <div className="run-console__status" title={snapshot.progressStep || '等待运行指令'}>
        <div className="run-console__phase-row">
          <span className={`phase-beacon phase-beacon--${snapshot.phase}`} />
          <strong>{phaseLabel(snapshot.phase)}</strong>
          <span>轮 {snapshot.cycleIndex || 0}</span>
          <span>样本 {snapshot.sampleCount || 0}</span>
        </div>
        <TelemetryStatus />
      </div>

      <div className="run-mode" role="group" aria-label="运行模式">
        {modeOptions.map(({ mode, title }) => (
          <button
            className={options.mode === mode ? 'run-mode__item is-active' : 'run-mode__item'}
            disabled={active || analysisBlockingWrites}
            key={mode}
            onClick={() => saveOptions({ ...options, mode })}
            type="button"
          >
            <strong>{title}</strong>
          </button>
        ))}
      </div>

      <div className="run-parameters">
        {options.mode === 'pc_periodic' ? (
          <>
            <label>
              <span>间隔（0 表示上一轮完成后立即开始下一轮）</span>
              <span className="number-input">
                <input
                  aria-label="PC 周期轮间隔毫秒"
                  disabled={active || analysisBlockingWrites}
                  min={0}
                  max={3_600_000}
                  onChange={(event) => saveOptions({ ...options, intervalMs: Number(event.target.value) })}
                  type="number"
                  value={options.intervalMs}
                />
                <b>ms</b>
              </span>
            </label>
            <label>
              <span>轮数</span>
              <span className="number-input">
                <input
                  aria-label="PC 周期最大轮数，零表示无限"
                  disabled={active || analysisBlockingWrites}
                  min={0}
                  max={1_000_000_000}
                  onChange={(event) => saveOptions({ ...options, maxCycles: Number(event.target.value) })}
                  type="number"
                  value={options.maxCycles}
                />
                <b>{options.maxCycles === 0 ? '∞' : '轮'}</b>
              </span>
            </label>
          </>
        ) : null}
        {options.mode !== 'single' ? (
          <label className="run-save-option">
            <input
              aria-label="保存连续测试全部测量列"
              checked={options.saveData}
              disabled={active || analysisBlockingWrites}
              onChange={(event) => saveOptions({ ...options, saveData: event.target.checked })}
              type="checkbox"
            />
            <span>保存全部测量列</span>
          </label>
        ) : null}
      </div>

      <div className="run-actions">
        {connectionState !== 'connected' ? (
          <button
            className="button button--primary"
            disabled={connectionState === 'connecting' || connectionState === 'reconnecting'}
            onClick={() => void connect(true)}
            type="button"
          >
            <ArrowClockwise size={17} />重连
          </button>
        ) : snapshot.phase === 'empty' ? (
          <button className="button button--primary" disabled={busyAction !== null || !testConfigsReady} onClick={() => execute('load')} type="button">
            <Plug size={17} />{loadingConfig ? '加载中…' : '加载配置'}
          </button>
        ) : snapshot.phase === 'configured' ? (
          <button className="button button--primary" disabled={busyAction !== null || analysisBlockingWrites} onClick={() => execute('prepare')} type="button">
            <Plug size={17} />连接设备
          </button>
        ) : !active ? (
          <button
            className="button button--primary"
            disabled={!canStart || Boolean(periodicError) || Boolean(parameterError) || unsupported || busyAction !== null}
            onClick={beginRun}
            type="button"
          >
            <Play size={17} weight="fill" />开始
          </button>
        ) : null}

        {snapshot.descriptor.stoppable && snapshot.phase === 'running' && (
          <button className="button" disabled={busyAction !== null} onClick={() => execute('pause')} type="button">
            <Pause size={17} weight="fill" />暂停
          </button>
        )}
        {snapshot.descriptor.stoppable && snapshot.phase === 'paused' && (
          <button className="button" disabled={busyAction !== null} onClick={() => execute('resume')} type="button">
            <Play size={17} weight="fill" />继续
          </button>
        )}
        {snapshot.descriptor.stoppable && active && snapshot.phase !== 'stopping' && (
          <button className="button button--danger" disabled={busyAction !== null} onClick={() => execute('stop')} type="button">
            <StopIcon size={17} weight="fill" />停止
          </button>
        )}
        {busyAction && <span className="command-busy">处理中…</span>}
        {analysisBlockingWrites && (
          <span className="analysis-run-state" title={snapshot.analysis.message}>
            性能分析：{analysisStageLabel(snapshot.analysis.state, snapshot.analysis.stage)}
          </span>
        )}
        {snapshot.dataSaveEnabled && !snapshot.dataSaveError && (
          <span
            className="data-save-state"
            title={snapshot.dataFilePath || '后端正在保存全部测量列'}
          >
            {active ? '数据写入中' : '数据已保存'}
          </span>
        )}
      </div>

      <RunParameterEditor
        descriptor={snapshot.descriptor}
        disabled={active || analysisBlockingWrites}
        effective={active && Object.keys(snapshot.effectiveRunParameters).length > 0}
        onChange={saveRunParameters}
        onReset={() => saveRunParameters({ ...snapshot.descriptor.runParameterDefaults })}
        values={options.algorithmParameters}
      />

      {helmBoardAutomatic && (
        <div className="run-console__warning" role="status">
          <WarningCircle size={16} />自动测试前请确保 MB_DDF_v2_HelmControl 已停止
        </div>
      )}

      {snapshot.descriptor.postRunAnalysis.supported && active && (
        <AnalysisRunHint activeRunParameters={activeRunParameters} />
      )}

      <div className="run-console__progress" aria-label={`测试进度 ${snapshot.progress}%`}>
        <i style={{ width: `${Math.max(0, Math.min(100, snapshot.progress))}%` }} />
      </div>

      {controlError && (
        <div className="run-console__error" role="alert">
          <WarningCircle size={16} />{controlError}
        </div>
      )}
    </section>
  )
}

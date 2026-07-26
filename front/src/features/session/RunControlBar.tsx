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
import { useSession } from './SessionProvider'

const RUN_OPTIONS_KEY = 'hwtest.run-options.v1'

function loadRunOptions(): TestRunOptions {
  try {
    const stored = window.localStorage.getItem(RUN_OPTIONS_KEY)
    if (stored) return { mode: 'pc_periodic', intervalMs: 500, maxCycles: 0, ...JSON.parse(stored) }
  } catch {
    // Ignore browser storage restrictions and use safe defaults.
  }
  return { mode: 'pc_periodic', intervalMs: 500, maxCycles: 0 }
}

const MODE_LABELS: Array<{ mode: RunMode; title: string; caption: string }> = [
  { mode: 'single', title: '单次', caption: '一轮指令 / 反馈' },
  { mode: 'pc_periodic', title: 'PC 周期', caption: '主机重复双向交互' },
  { mode: 'device_stream', title: '设备持续', caption: '一次启动 / 主动回告' },
]

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
  const testChangeBlocked = ['running', 'paused', 'stopping', 'preparing'].includes(snapshot.phase)
  const canStart = ['ready', 'finished', 'stopped'].includes(snapshot.phase)
  const testTitle = snapshot.descriptor.title || snapshot.descriptor.productName || '当前测试'
  const selectedTestId = snapshot.descriptor.configId || selectedConfigId
  const supportedModes: RunMode[] = snapshot.descriptor.supportedRunModes.length > 0
    ? snapshot.descriptor.supportedRunModes
    : ['single', 'pc_periodic']
  const modeOptions = MODE_LABELS.filter(({ mode }) => supportedModes.includes(mode))
  const loadingConfig = !testConfigsReady || busyAction === 'load' || busyAction === 'selectTest'
  const periodicError = useMemo(() => {
    if (options.mode !== 'pc_periodic') return ''
    if (!Number.isInteger(options.intervalMs) || options.intervalMs < 10 || options.intervalMs > 3_600_000) {
      return '周期需为 10–3,600,000 ms 的整数'
    }
    if (!Number.isInteger(options.maxCycles) || options.maxCycles < 0 || options.maxCycles > 1_000_000_000) {
      return '轮数需为 0–1,000,000,000；0 表示持续运行'
    }
    return ''
  }, [options])
  const unsupported = !supportedModes.includes(options.mode)

  useEffect(() => {
    if (supportedModes.includes(options.mode)) return
    const mode = supportedModes[0] ?? 'single'
    const next = { ...options, mode }
    setOptions(next)
    window.localStorage.setItem(RUN_OPTIONS_KEY, JSON.stringify(next))
  }, [options, supportedModes])

  function saveOptions(next: TestRunOptions) {
    setOptions(next)
    window.localStorage.setItem(RUN_OPTIONS_KEY, JSON.stringify(next))
  }

  function execute(action: Parameters<typeof invoke>[0], params?: Record<string, unknown>) {
    void invoke(action, params).catch(() => undefined)
  }

  function beginRun() {
    if (periodicError || unsupported) return
    const normalized: TestRunOptions = options.mode === 'single'
      ? { mode: 'single', intervalMs: 1000, maxCycles: 1 }
      : options
    void start(normalized).catch(() => undefined)
  }

  return (
    <section className="run-console" aria-label="全局测试运行控制">
      <div className="run-console__identity">
        <span className="eyebrow">RUN CONTROL</span>
        <div className="run-console__test-picker">
          <label htmlFor="test-config-select">测试项目</label>
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
              <option value={selectedTestId}>{selectedTestId ? '当前启动配置' : '等待配置目录'}</option>
            ) : testConfigs.map((test) => (
              <option key={test.configId} value={test.configId}>{test.title}</option>
            ))}
          </select>
        </div>
        <div className="run-console__phase-row">
          <span className={`phase-beacon phase-beacon--${snapshot.phase}`} />
          <strong>{snapshot.phase.toUpperCase()}</strong>
          <span>循环 {snapshot.cycleIndex || 0}</span>
          <span>样本 {snapshot.sampleCount || 0}</span>
        </div>
        <div className="run-console__progress" aria-label={`测试进度 ${snapshot.progress}%`}>
          <i style={{ width: `${Math.max(0, Math.min(100, snapshot.progress))}%` }} />
        </div>
        <small>{snapshot.progressStep || '等待运行指令'}</small>
      </div>

      <div className="run-mode" role="group" aria-label="运行模式">
        {modeOptions.map(({ mode, title, caption }) => (
          <button
            className={options.mode === mode ? 'run-mode__item is-active' : 'run-mode__item'}
            disabled={active}
            key={mode}
            onClick={() => saveOptions({ ...options, mode })}
            type="button"
          >
            <strong>{title}</strong>
            <span>{caption}</span>
          </button>
        ))}
      </div>

      <div className="run-parameters">
        {options.mode === 'pc_periodic' ? (
          <>
            <label>
              <span>轮间隔</span>
              <span className="number-input">
                <input
                  aria-label="PC 周期轮间隔毫秒"
                  disabled={active}
                  min={10}
                  max={3_600_000}
                  onChange={(event) => saveOptions({ ...options, intervalMs: Number(event.target.value) })}
                  type="number"
                  value={options.intervalMs}
                />
                <b>ms</b>
              </span>
            </label>
            <label>
              <span>最大轮数</span>
              <span className="number-input">
                <input
                  aria-label="PC 周期最大轮数，零表示无限"
                  disabled={active}
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
        ) : (
          <div className={unsupported ? 'mode-note mode-note--warning' : 'mode-note'}>
            {unsupported
              ? `${testTitle} 不支持当前运行模式。`
              : options.mode === 'single'
                ? (snapshot.descriptor.description || `执行一次${testTitle}并采集一次反馈。`)
                : '由 PC 按设定间隔重复采集反馈。'}
          </div>
        )}
      </div>

      <div className="run-actions">
        {connectionState !== 'connected' ? (
          <button
            className="button button--primary"
            disabled={connectionState === 'connecting' || connectionState === 'reconnecting'}
            onClick={() => void connect(true)}
            type="button"
          >
            <ArrowClockwise size={17} />重连后端
          </button>
        ) : snapshot.phase === 'empty' ? (
          <button className="button button--primary" disabled={busyAction !== null || !testConfigsReady} onClick={() => execute('load')} type="button">
            <Plug size={17} />{loadingConfig ? '加载配置中…' : '重试加载'}
          </button>
        ) : snapshot.phase === 'configured' ? (
          <button className="button button--primary" disabled={busyAction !== null} onClick={() => execute('prepare')} type="button">
            <Plug size={17} />连接设备
          </button>
        ) : !active ? (
          <button
            className="button button--primary"
            disabled={!canStart || Boolean(periodicError) || unsupported || busyAction !== null}
            onClick={beginRun}
            type="button"
          >
            <Play size={17} weight="fill" />开始测试
          </button>
        ) : null}

        {snapshot.phase === 'running' && (
          <button className="button" disabled={busyAction !== null} onClick={() => execute('pause')} type="button">
            <Pause size={17} weight="fill" />暂停
          </button>
        )}
        {snapshot.phase === 'paused' && (
          <button className="button" disabled={busyAction !== null} onClick={() => execute('resume')} type="button">
            <Play size={17} weight="fill" />继续
          </button>
        )}
        {active && snapshot.phase !== 'stopping' && (
          <button className="button button--danger" disabled={busyAction !== null} onClick={() => execute('stop')} type="button">
            <StopIcon size={17} weight="fill" />停止
          </button>
        )}
        {busyAction && <span className="command-busy">执行 {busyAction}…</span>}
      </div>

      {(periodicError || actionError) && (
        <div className="run-console__error" role="alert">
          <WarningCircle size={16} />{periodicError || actionError}
        </div>
      )}
    </section>
  )
}

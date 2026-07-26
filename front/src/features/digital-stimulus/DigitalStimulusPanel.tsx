import { ArrowCounterClockwise } from '@phosphor-icons/react'
import { useEffect, useRef, useState } from 'react'

import type {
  ApplicationSample,
  DigitalStimulusSnapshot,
  DigitalSwitchDescriptor,
} from '../../shared/protocol'
import {
  appliedBit,
  DigitalStimulusCommandQueue,
  type DigitalStimulusTransport,
  physicalLevel,
  readbackBit,
  readbackGroup,
  readbackStatus,
  type DigitalReadbackStatus,
} from './digital-stimulus'

export interface DigitalStimulusPanelProps {
  stimulus: DigitalStimulusSnapshot
  latestSample: ApplicationSample | null
  transport: DigitalStimulusTransport
}

const READBACK_LABEL: Record<DigitalReadbackStatus, string> = {
  match: '匹配',
  mismatch: '不匹配',
  settling: '稳定中',
  unknown: '未知',
}

function isDutDi0Descriptor(descriptor: DigitalSwitchDescriptor): boolean {
  return Number.isInteger(descriptor.dutBit) && descriptor.dutBit >= 0 && descriptor.dutBit < 16
}

function formatDiagnostic(value: number | null): string {
  return value === null ? '未知' : `0x${value.toString(16).padStart(8, '0').toUpperCase()}`
}

function readbackValue(sample: ApplicationSample | null, descriptor: DigitalSwitchDescriptor): string {
  const value = readbackBit(sample, descriptor.dutBit)
  return value === null ? '未知' : value ? '1' : '0'
}

export function DigitalStimulusPanel({
  stimulus,
  latestSample,
  transport,
}: DigitalStimulusPanelProps) {
  const transportRef = useRef(transport)
  const queueRef = useRef<DigitalStimulusCommandQueue | null>(null)
  const mountedRef = useRef(false)
  const [optimistic, setOptimistic] = useState(stimulus)
  const [queueError, setQueueError] = useState('')
  const [, refreshSettling] = useState(0)

  transportRef.current = transport

  useEffect(() => {
    mountedRef.current = true
    let queue: DigitalStimulusCommandQueue | null = null
    queue = new DigitalStimulusCommandQueue(
      {
        set: (switchId, active, expectedRevision) => (
          transportRef.current.set(switchId, active, expectedRevision)
        ),
        reset: () => transportRef.current.reset(),
      },
      stimulus,
      (state) => {
        if (mountedRef.current && queue !== null && queueRef.current === queue) setOptimistic(state)
      },
      (message) => {
        if (mountedRef.current && queue !== null && queueRef.current === queue) setQueueError(message)
      },
    )
    queueRef.current = queue
    queue.sync(stimulus)

    return () => {
      mountedRef.current = false
      queue?.dispose()
      if (queueRef.current === queue) queueRef.current = null
    }
  }, [])

  useEffect(() => {
    queueRef.current?.sync(stimulus)
    if (!stimulus.errorCode) setQueueError('')
  }, [stimulus])

  useEffect(() => {
    const { lastWriteTimestampUs, settlingMs } = optimistic
    if (lastWriteTimestampUs <= 0 || settlingMs <= 0) return undefined

    const deadlineUs = lastWriteTimestampUs + settlingMs * 1000
    const remainingUs = deadlineUs - Date.now() * 1000
    if (remainingUs <= 0) return undefined

    const timer = window.setTimeout(() => refreshSettling((value) => value + 1), Math.ceil(remainingUs / 1000) + 1)
    return () => window.clearTimeout(timer)
  }, [optimistic.lastWriteTimestampUs, optimistic.settlingMs])

  const descriptors = optimistic.switches.filter(isDutDi0Descriptor)
  const disabled = !optimistic.available || !optimistic.configured
  const backendError = optimistic.errorCode
    ? `${optimistic.errorCode}${optimistic.message ? `: ${optimistic.message}` : ''}`
    : ''
  const liveMessage = queueError || backendError || optimistic.message || `数字刺激 revision ${optimistic.revision}`
  const nowUs = Date.now() * 1000
  const diagnostic = readbackGroup(latestSample, 1)

  function toggle(descriptor: DigitalSwitchDescriptor, active: boolean) {
    setQueueError('')
    queueRef.current?.toggle(descriptor.switchId, active)
  }

  function reset() {
    setQueueError('')
    queueRef.current?.requestReset()
  }

  return (
    <section className="panel digital-stimulus" aria-label="DUT 数字刺激控制">
      <header className="panel__header digital-stimulus__header">
        <div>
          <span className="eyebrow">DIGITAL STIMULUS</span>
          <h3>16 路 DUT DI 刺激</h3>
        </div>
        <button
          aria-label="恢复数字刺激安全态"
          className="button button--quiet digital-stimulus__reset"
          disabled={disabled}
          onClick={reset}
          title="恢复安全态"
          type="button"
        >
          <ArrowCounterClockwise aria-hidden="true" size={17} />
          <span>恢复安全态</span>
        </button>
      </header>

      <p aria-atomic="true" aria-live="polite" className="digital-stimulus__live">
        {liveMessage}
      </p>

      <div className="digital-stimulus__grid" role="group" aria-label="16 路数字刺激开关">
        {descriptors.map((descriptor) => {
          const active = appliedBit(optimistic.appliedMask, descriptor.dutBit)
          const status = readbackStatus(optimistic, descriptor, latestSample, nowUs)
          const physical = physicalLevel(descriptor, active)
          return (
            <article
              className={`digital-stimulus__switch digital-stimulus__switch--${status}`}
              data-readback-status={status}
              key={descriptor.switchId}
            >
              <div className="digital-stimulus__switch-heading">
                <strong>{descriptor.label}</strong>
                <code>DI[{descriptor.dutBit}]</code>
              </div>
              <label className="digital-stimulus__toggle">
                <input
                  aria-label={`${descriptor.label} 逻辑刺激`}
                  aria-checked={active}
                  checked={active}
                  disabled={disabled}
                  onChange={(event) => toggle(descriptor, event.target.checked)}
                  role="switch"
                  type="checkbox"
                />
                <span aria-hidden="true" className="digital-stimulus__toggle-track" />
                <span className="digital-stimulus__logical">逻辑 {active ? '激活' : '安全'}</span>
              </label>
              <dl className="digital-stimulus__facts">
                <div><dt>物理电平</dt><dd>{physical}</dd></div>
                <div><dt>DUT 回读位</dt><dd>{readbackValue(latestSample, descriptor)}</dd></div>
                <div><dt>回读</dt><dd className={`digital-stimulus__status digital-stimulus__status--${status}`}>{READBACK_LABEL[status]}</dd></div>
              </dl>
            </article>
          )
        })}
      </div>

      <footer className="digital-stimulus__diagnostic">
        <span>di_state[1] 诊断位图</span>
        <code>{formatDiagnostic(diagnostic)}</code>
      </footer>
    </section>
  )
}

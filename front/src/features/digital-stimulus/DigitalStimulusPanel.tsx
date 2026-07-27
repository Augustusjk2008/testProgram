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
  return value === null ? '—' : value ? '1' : '0'
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
  const liveMessage = queueError || backendError || optimistic.message || `版本 ${optimistic.revision}`
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
    <section className="panel digital-stimulus" aria-label="DUT 数字激励控制">
      <header className="panel__header digital-stimulus__header">
        <div className="digital-stimulus__title">
          <h3>16 路 DI 激励</h3>
          <span aria-atomic="true" aria-live="polite">{liveMessage}</span>
        </div>
        <div className="digital-stimulus__tools">
          <span className="digital-stimulus__diagnostic">诊断 <code>{formatDiagnostic(diagnostic)}</code></span>
          <button
            aria-label="恢复数字激励安全态"
            className="button button--quiet digital-stimulus__reset"
            disabled={disabled}
            onClick={reset}
            title="恢复安全态"
            type="button"
          >
            <ArrowCounterClockwise aria-hidden="true" size={17} />
          </button>
        </div>
      </header>

      <div className="digital-stimulus__grid" role="group" aria-label="16 路数字激励开关">
        {descriptors.map((descriptor) => {
          const active = appliedBit(optimistic.appliedMask, descriptor.dutBit)
          const status = readbackStatus(optimistic, descriptor, latestSample, nowUs)
          const echo = readbackValue(latestSample, descriptor)
          return (
            <article
              className={`digital-stimulus__switch digital-stimulus__switch--${status}`}
              data-readback-status={status}
              key={descriptor.switchId}
              title={`${descriptor.label}：${active ? '激励' : '安全'}，回显 ${echo}，${READBACK_LABEL[status]}`}
            >
              <strong>DI{descriptor.dutBit}</strong>
              <label className="digital-stimulus__toggle">
                <input
                  aria-label={`${descriptor.label} 激励`}
                  aria-checked={active}
                  checked={active}
                  disabled={disabled}
                  onChange={(event) => toggle(descriptor, event.target.checked)}
                  role="switch"
                  type="checkbox"
                />
                <span aria-hidden="true" className="digital-stimulus__toggle-track" />
              </label>
              <output
                aria-label={`${descriptor.label} 回显 ${echo}，${READBACK_LABEL[status]}`}
                className={`digital-stimulus__echo digital-stimulus__status--${status}`}
              >
                {echo}
              </output>
            </article>
          )
        })}
      </div>
    </section>
  )
}

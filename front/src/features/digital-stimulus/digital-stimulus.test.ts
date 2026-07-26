import { describe, expect, it } from 'vitest'

import type { ApplicationSample, DigitalStimulusSnapshot } from '../../shared/protocol'
import {
  appliedBit,
  DigitalStimulusCommandQueue,
  physicalLevel,
  readbackBit,
  readbackGroup,
  readbackStatus,
} from './digital-stimulus'

function snapshot(revision = 5, appliedMask = 0): DigitalStimulusSnapshot {
  return {
    available: true,
    configured: true,
    switches: [
      { switchId: 'di0', dutBit: 0, label: 'DI0', activeLevel: 'High' },
      { switchId: 'di1', dutBit: 1, label: 'DI1', activeLevel: 'Low' },
    ],
    appliedMask,
    revision,
    lastWriteTimestampUs: 1_000_000,
    settlingMs: 20,
    errorCode: '',
    message: '',
  }
}

describe('digital stimulus command queue', () => {
  it('coalesces rapid toggles and sends one revisioned request at a time', async () => {
    const calls: Array<[string, boolean, number]> = []
    const states: DigitalStimulusSnapshot[] = []
    const transport = {
      set: async (switchId: string, active: boolean, expectedRevision: number) => {
        calls.push([switchId, active, expectedRevision])
        const bit = switchId === 'di0' ? 1 : 2
        const previous = states.at(-1) ?? snapshot(expectedRevision, 0)
        return {
          ...previous,
          appliedMask: active ? previous.appliedMask | bit : previous.appliedMask & ~bit,
          revision: expectedRevision + 1,
        }
      },
      reset: async () => snapshot(99, 0),
    }
    const queue = new DigitalStimulusCommandQueue(
      transport,
      snapshot(),
      (state) => states.push(state),
      () => undefined,
    )

    queue.toggle('di0', true)
    queue.toggle('di0', false)
    queue.toggle('di0', true)
    queue.toggle('di1', true)
    await queue.flushNow()

    expect(calls).toEqual([
      ['di0', true, 5],
      ['di1', true, 6],
    ])
    expect(queue.state().appliedMask).toBe(3)
    expect(queue.state().revision).toBe(7)
  })

  it('rolls back optimistic state when the backend rejects a command', async () => {
    const errors: string[] = []
    const states: DigitalStimulusSnapshot[] = []
    const queue = new DigitalStimulusCommandQueue(
      {
        set: async () => { throw new Error('revision conflict') },
        reset: async () => snapshot(),
      },
      snapshot(4, 0),
      (state) => states.push(state),
      (message) => errors.push(message),
    )

    queue.toggle('di0', true)
    expect(queue.state().appliedMask).toBe(1)
    await queue.flushNow()
    expect(queue.state().appliedMask).toBe(0)
    expect(queue.state().revision).toBe(4)
    expect(errors).toEqual(['revision conflict'])
    expect(states.at(-1)?.appliedMask).toBe(0)
  })

  it('serializes reset behind an in-flight command and adopts backend state', async () => {
    let resolveSet: ((value: DigitalStimulusSnapshot) => void) | undefined
    const calls: string[] = []
    const queue = new DigitalStimulusCommandQueue(
      {
        set: async () => {
          calls.push('set')
          return new Promise((resolve) => { resolveSet = resolve })
        },
        reset: async () => {
          calls.push('reset')
          return snapshot(8, 0)
        },
      },
      snapshot(6, 0),
      () => undefined,
      () => undefined,
    )
    queue.toggle('di0', true)
    const flushing = queue.flushNow()
    await Promise.resolve()
    queue.requestReset()
    resolveSet?.(snapshot(7, 1))
    await flushing
    await queue.flushNow()
    expect(calls).toEqual(['set', 'reset'])
    expect(queue.state().appliedMask).toBe(0)
    expect(queue.state().revision).toBe(8)
  })

  it('adopts a newer authoritative snapshot without overwriting it with an older revision', () => {
    const queue = new DigitalStimulusCommandQueue(
      {
        set: async () => snapshot(),
        reset: async () => snapshot(),
      },
      snapshot(5, 0),
      () => undefined,
      () => undefined,
    )

    queue.sync(snapshot(7, 0b10))
    queue.sync(snapshot(6, 0b01))

    expect(queue.state().revision).toBe(7)
    expect(queue.state().appliedMask).toBe(0b10)
  })

  it('does not let an older in-flight reply overwrite a newer authoritative snapshot', async () => {
    let resolveSet: ((value: DigitalStimulusSnapshot) => void) | undefined
    const queue = new DigitalStimulusCommandQueue(
      {
        set: async () => new Promise((resolve) => { resolveSet = resolve }),
        reset: async () => snapshot(),
      },
      snapshot(5, 0),
      () => undefined,
      () => undefined,
    )

    queue.toggle('di0', true)
    const flushing = queue.flushNow()
    await Promise.resolve()
    queue.sync(snapshot(7, 0b10))
    resolveSet?.(snapshot(6, 0b01))
    await flushing

    expect(queue.state().revision).toBe(7)
    expect(queue.state().appliedMask).toBe(0b10)
  })
})

describe('digital stimulus readback', () => {
  const sample: ApplicationSample = {
    taskId: 'task',
    stepId: 'DI_READ',
    channelId: 'DI_READ',
    timestampUs: 2_000_000,
    cycleIndex: 1,
    values: { 'di_state[0]': 0b10, 'di_state[1]': 0x8000_0000 },
    tags: {},
  }

  it('extracts DUT bits and preserves the second bitmap as diagnostics', () => {
    expect(appliedBit(0b10, 0)).toBe(false)
    expect(appliedBit(0b10, 1)).toBe(true)
    expect(readbackBit(sample, 0)).toBe(false)
    expect(readbackBit(sample, 1)).toBe(true)
    expect(readbackBit(sample, 63)).toBe(true)
    expect(readbackGroup(sample, 0)).toBe(0b10)
    expect(readbackGroup(sample, 1)).toBe(0x8000_0000)
    expect(readbackGroup(sample, 2)).toBeNull()
    expect(readbackBit(null, 0)).toBeNull()
    expect(readbackGroup({ ...sample, values: { 'di_state[0]': -1 } }, 0)).toBeNull()
    expect(readbackGroup({ ...sample, values: { 'di_state[0]': 0x1_0000_0000 } }, 0)).toBeNull()
    expect(readbackGroup({ ...sample, values: { 'di_state[0]': '0x100000000' } }, 0)).toBeNull()
  })

  it('maps active-low physical levels and compares DUT physical readback after settling', () => {
    const activeLow = snapshot().switches[1]
    const lowReadback: ApplicationSample = {
      ...sample,
      values: { ...sample.values, 'di_state[0]': 0 },
    }
    expect(physicalLevel(activeLow, true)).toBe('Low')
    expect(physicalLevel(activeLow, false)).toBe('High')
    expect(readbackStatus(snapshot(5, 0b10), activeLow, lowReadback, 1_010_000)).toBe('settling')
    expect(readbackStatus(snapshot(5, 0b10), activeLow, lowReadback, 1_100_000)).toBe('match')
    expect(readbackStatus(
      snapshot(5, 0b10),
      activeLow,
      { ...lowReadback, timestampUs: 900_000 },
      1_100_000,
    )).toBe('settling')
    expect(readbackStatus(snapshot(5, 0b10), activeLow, sample, 1_100_000)).toBe('mismatch')
    expect(readbackStatus(snapshot(5, 0), activeLow, sample, 1_100_000)).toBe('match')
  })
})

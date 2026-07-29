import { describe, expect, it } from 'vitest'

import type { ApplicationSample } from '../../shared/protocol'
import { TelemetryStore, type TelemetryCommitScheduler } from './telemetry-store'

interface ScheduledCallbacks {
  timeout: Array<() => void>
  frame: Array<() => void>
}

function createScheduler(): {
  scheduled: ScheduledCallbacks
  scheduler: TelemetryCommitScheduler
  flush: () => void
} {
  const scheduled: ScheduledCallbacks = { timeout: [], frame: [] }
  const scheduler: TelemetryCommitScheduler = {
    now: () => 0,
    setTimeout: (callback: () => void) => {
      scheduled.timeout.push(callback)
      return callback
    },
    clearTimeout: (handle: unknown) => {
      const index = scheduled.timeout.indexOf(handle as () => void)
      if (index >= 0) scheduled.timeout.splice(index, 1)
    },
    requestAnimationFrame: (callback: () => void) => {
      scheduled.frame.push(callback)
      return callback
    },
    cancelAnimationFrame: (handle: unknown) => {
      const index = scheduled.frame.indexOf(handle as () => void)
      if (index >= 0) scheduled.frame.splice(index, 1)
    },
  }
  return {
    scheduled,
    scheduler,
    flush: () => {
      const timeout = scheduled.timeout.splice(0)
      timeout.forEach((callback) => callback())
      const frame = scheduled.frame.splice(0)
      frame.forEach((callback) => callback())
    },
  }
}

function sample(
  timestampUs: number,
  cycleIndex: number,
  taskId = 'task-1',
): ApplicationSample {
  return {
    taskId,
    stepId: 'SYSTEM_STATUS',
    channelId: 'SYSTEM_STATUS',
    timestampUs,
    cycleIndex,
    values: { cpu_usage: cycleIndex },
    tags: {},
  }
}

describe('TelemetryStore', () => {
  it('keeps 120,000 received samples bounded to the newest 50,000 points', () => {
    const clock = createScheduler()
    const store = new TelemetryStore(50_000, { commitIntervalMs: 100, scheduler: clock.scheduler })
    const batchCount = 120_000 / 64
    for (let batchIndex = 0; batchIndex < batchCount; batchIndex += 1) {
      const firstSeq = batchIndex * 64 + 1
      store.appendMany({
        firstSeq,
        lastSeq: firstSeq + 63,
        samples: Array.from({ length: 64 }, (_, offset) => {
          const index = firstSeq + offset
          return sample(index, index)
        }),
      })
    }
    clock.flush()

    expect(store.getSnapshot()).toMatchObject({
      receivedCount: 120_000,
      retainedCount: 50_000,
      evictedCount: 70_000,
      latestSample: expect.objectContaining({ cycleIndex: 120_000 }),
    })
  })

  it('commits one batch once and exposes bounded cumulative statistics', () => {
    const clock = createScheduler()
    const store = new TelemetryStore(3, { commitIntervalMs: 100, scheduler: clock.scheduler })
    let commits = 0
    store.subscribe(() => { commits += 1 })

    const summary = store.appendMany({
      firstSeq: 101,
      lastSeq: 104,
      samples: [sample(1_000_000, 1), sample(2_000_000, 2), sample(3_000_000, 3), sample(4_000_000, 4)],
    })

    expect(summary).toEqual({
      firstSeq: 101,
      lastSeq: 104,
      sampleCount: 4,
      channelIds: ['SYSTEM_STATUS'],
      cycleIndexRange: [1, 4],
    })
    expect(summary).not.toHaveProperty('samples')
    expect(clock.scheduled.timeout).toHaveLength(1)
    expect(commits).toBe(0)

    clock.flush()

    expect(commits).toBe(1)
    expect(store.getSnapshot()).toMatchObject({
      version: 1,
      receivedCount: 4,
      retainedCount: 3,
      evictedCount: 1,
      sequenceComplete: true,
      sequenceStatus: 'continuous',
    })
  })

  it('marks gaps, duplicate or reversed batches, and reconnects as incomplete', () => {
    const clock = createScheduler()
    const store = new TelemetryStore(10, { commitIntervalMs: 100, scheduler: clock.scheduler })

    store.appendMany({ firstSeq: 1, lastSeq: 2, samples: [sample(1, 1), sample(2, 2)] })
    clock.flush()
    store.appendMany({ firstSeq: 4, lastSeq: 4, samples: [sample(4, 4)] })
    clock.flush()
    expect(store.getSnapshot()).toMatchObject({ sequenceComplete: false, sequenceStatus: 'gap' })

    store.clear()
    store.appendMany({ firstSeq: 10, lastSeq: 10, samples: [sample(10, 1)] })
    clock.flush()
    store.appendMany({ firstSeq: 10, lastSeq: 10, samples: [sample(11, 2)] })
    clock.flush()
    expect(store.getSnapshot()).toMatchObject({ sequenceComplete: false, sequenceStatus: 'gap' })

    store.clear()
    store.appendMany({ firstSeq: 20, lastSeq: 20, samples: [sample(20, 1)] })
    clock.flush()
    store.markReconnected()
    expect(store.getSnapshot()).toMatchObject({
      sequenceComplete: false,
      sequenceStatus: 'reconnect_incomplete',
    })
  })

  it('marks any batch after the last safe sequence as incomplete', () => {
    const clock = createScheduler()
    const store = new TelemetryStore(10, { commitIntervalMs: 100, scheduler: clock.scheduler })

    store.appendMany({
      firstSeq: Number.MAX_SAFE_INTEGER,
      lastSeq: Number.MAX_SAFE_INTEGER,
      samples: [sample(1, 1)],
    })
    clock.flush()
    expect(store.getSnapshot()).toMatchObject({ sequenceComplete: true, sequenceStatus: 'continuous' })

    store.appendMany({ firstSeq: 0, lastSeq: 0, samples: [sample(2, 2)] })
    clock.flush()
    expect(store.getSnapshot()).toMatchObject({ sequenceComplete: false, sequenceStatus: 'gap' })
  })

  it('cancels a queued commit when cleared, switched to a new task, or disposed', () => {
    const clock = createScheduler()
    const store = new TelemetryStore(10, { commitIntervalMs: 100, scheduler: clock.scheduler })

    store.appendMany({ firstSeq: 1, lastSeq: 1, samples: [sample(1, 1)] })
    expect(clock.scheduled.timeout).toHaveLength(1)
    store.clear()
    expect(clock.scheduled.timeout).toHaveLength(0)

    store.appendMany({ firstSeq: 2, lastSeq: 2, samples: [sample(2, 2)] })
    expect(clock.scheduled.timeout).toHaveLength(1)
    store.appendMany({ firstSeq: 3, lastSeq: 3, samples: [sample(3, 1, 'task-2')] })
    expect(clock.scheduled.timeout).toHaveLength(1)
    store.dispose()
    expect(clock.scheduled.timeout).toHaveLength(0)
  })
})

import { describe, expect, it } from 'vitest'

import type { ApplicationSample } from '../../shared/protocol'
import { SampleBuffer, downsampleAlignedMinMax } from './sample-buffer'

function sample(timestampUs: number, values: Record<string, unknown>): ApplicationSample {
  return {
    taskId: 'task-1',
    stepId: 'SYSTEM_STATUS',
    channelId: 'SYSTEM_STATUS',
    timestampUs,
    cycleIndex: timestampUs,
    values,
    tags: {},
  }
}

describe('SampleBuffer', () => {
  it('appends batches and reports ring-buffer evictions', () => {
    const buffer = new SampleBuffer(3)
    const batchable = buffer as unknown as {
      appendMany?: (samples: readonly ApplicationSample[]) => void
      evictedCount?: number
    }

    expect(batchable.appendMany).toBeTypeOf('function')
    if (typeof batchable.appendMany !== 'function') return

    batchable.appendMany([
      sample(1_000_000, { cpu_usage: 1 }),
      sample(2_000_000, { cpu_usage: 2 }),
      sample(3_000_000, { cpu_usage: 3 }),
      sample(4_000_000, { cpu_usage: 4 }),
    ])

    expect(buffer.query(['cpu_usage'])[0]).toEqual([2, 3, 4])
    expect(batchable.evictedCount).toBe(1)
  })

  it('keeps only the newest configured number of points', () => {
    const buffer = new SampleBuffer(3)
    ;[1, 2, 3, 4].forEach((value) => {
      buffer.append(sample(value * 1_000_000, { cpu_usage: value }))
    })

    const data = buffer.query(['cpu_usage'])
    expect(data[0]).toEqual([2, 3, 4])
    expect(data[1]).toEqual([2, 3, 4])
    expect(buffer.size).toBe(3)
  })

  it('discovers finite nested numeric values and ignores protocol text', () => {
    const buffer = new SampleBuffer(10)
    buffer.append(sample(1_000_000, {
      cpu_usage: 12.5,
      thermal: { cpu: 44.25 },
      state: 'ready',
      invalid: Number.NaN,
    }))

    expect(buffer.fields()).toEqual(['cpu_usage', 'thermal.cpu'])
    expect(buffer.latest()?.values.cpu_usage).toBe(12.5)
  })

  it('filters a rolling time window', () => {
    const buffer = new SampleBuffer(10)
    buffer.append(sample(1_000_000, { cpu_usage: 1 }))
    buffer.append(sample(5_000_000, { cpu_usage: 5 }))
    buffer.append(sample(10_000_000, { cpu_usage: 10 }))

    const data = buffer.query(['cpu_usage'], { windowSeconds: 6 })
    expect(data[0]).toEqual([5, 10])
    expect(data[1]).toEqual([5, 10])
  })

  it('queries the ring directly with bounded multi-field extrema output', () => {
    const buffer = new SampleBuffer(1_000)
    for (let index = 0; index < 1_000; index += 1) {
      buffer.append(sample(index * 1_000_000, {
        primary: index === 500 ? -100 : index === 501 ? 100 : 0,
        secondary: index === 502 ? -10 : index === 503 ? 10 : 0,
      }))
    }

    const data = buffer.query(['primary', 'secondary'], { pixelWidth: 80 })

    expect(data[0][0]).toBe(0)
    expect(data[0].at(-1)).toBe(999)
    expect(data[1]).toContain(-100)
    expect(data[1]).toContain(100)
    expect(data[2]).toContain(-10)
    expect(data[2]).toContain(10)
    expect(data[0].length).toBeLessThanOrEqual(2 * 2 * 80 + 2)
    expect(data[0]).toEqual([...data[0]].sort((left, right) => Number(left) - Number(right)))
  })
})

describe('downsampleAlignedMinMax', () => {
  it('keeps each field\'s extrema when they share a pixel bucket', () => {
    const x = Array.from({ length: 1_000 }, (_, index) => index)
    const primary = x.map(() => 0)
    const secondary = x.map(() => 0)
    primary[500] = -100
    primary[501] = 100
    secondary[502] = -10
    secondary[503] = 10

    const downsampled = downsampleAlignedMinMax([x, primary, secondary], 80)

    expect(downsampled[1]).toContain(-100)
    expect(downsampled[1]).toContain(100)
    expect(downsampled[2]).toContain(-10)
    expect(downsampled[2]).toContain(10)
    const timestamps = downsampled[0].filter((value): value is number => value !== null)
    expect(timestamps).toEqual([...timestamps].sort((left, right) => left - right))
    expect(downsampled[0].length).toBeLessThanOrEqual(2 * 2 * 80 + 2)
  })

  it('limits points to roughly two per pixel while retaining a narrow spike', () => {
    const x = Array.from({ length: 1_000 }, (_, index) => index)
    const y = x.map(() => 1)
    y[513] = 100

    const downsampled = downsampleAlignedMinMax([x, y], 80)

    expect(downsampled[0].length).toBeLessThanOrEqual(162)
    expect(downsampled[1]).toContain(100)
    expect(downsampled[0][0]).toBe(0)
    expect(downsampled[0].at(-1)).toBe(999)
  })
})

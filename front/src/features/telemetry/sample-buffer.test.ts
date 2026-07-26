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
})

describe('downsampleAlignedMinMax', () => {
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

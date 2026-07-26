import { describe, expect, it } from 'vitest'

import { makeRequest, parseServerMessage } from './HwtestClient'

describe('HwtestClient protocol boundary', () => {
  it('builds a versioned start request without owning a repeat timer', () => {
    expect(JSON.parse(makeRequest('run-1', 'start', {
      mode: 'pc_periodic',
      intervalMs: 250,
      maxCycles: 0,
    }))).toEqual({
      v: 1,
      type: 'request',
      id: 'run-1',
      action: 'start',
      params: { mode: 'pc_periodic', intervalMs: 250, maxCycles: 0 },
    })
  })

  it('accepts a typed sample and rejects unsupported envelopes', () => {
    const sample = parseServerMessage(JSON.stringify({
      v: 1,
      type: 'sample',
      seq: 7,
      sample: {
        taskId: 'task-1',
        stepId: 'SYSTEM_STATUS',
        channelId: 'SYSTEM_STATUS',
        timestampUs: 1_785_000_000_123_456,
        cycleIndex: 2,
        values: { cpu_usage: 12.5 },
        tags: {},
      },
    }))
    expect(sample.type).toBe('sample')

    expect(() => parseServerMessage('{"v":2,"type":"hello"}'))
      .toThrow(/protocol/i)
  })
})

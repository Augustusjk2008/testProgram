import { describe, expect, it } from 'vitest'

import { makeRequest, parseServerMessage, parseTestConfigCatalog } from './HwtestClient'

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

  it('parses the configuration descriptor carried by snapshots', () => {
    const snapshot = parseServerMessage(JSON.stringify({
      v: 1,
      type: 'snapshot',
      seq: 3,
      snapshot: {
        phase: 'configured',
        testState: 'Uninitialized',
        controlResourceId: 'CONTROL_NETWORK',
        providerId: 'qt.udp',
        serialPortName: '',
        taskId: '',
        stepId: '',
        testItemId: '',
        algorithmId: '',
        progress: 0,
        progressStep: '',
        hasResult: false,
        verdict: '',
        errorCode: '',
        message: '',
        attempts: 0,
        rawData: {},
        runMode: 'single',
        intervalMs: 1000,
        maxCycles: 1,
        cycleIndex: 0,
        sampleCount: 0,
        descriptor: {
          configId: 'mbddf-elec-health',
          productModel: 'MB_DDF_v2',
          productName: 'MB_DDF electrical health',
          configVersion: '1.0.0',
          stepId: 'ELEC_HEALTH_STATUS',
          testItemId: 'elec_health_status',
          algorithmId: 'mbddf.elec_health_status',
          title: '电气健康',
          description: '读取电源与辅助电压健康量。',
          supportedRunModes: ['single', 'pc_periodic'],
          measurements: [
            { id: 'c_volt', label: 'C 路电压', unit: 'V', primary: true },
          ],
        },
      },
    }))

    expect(snapshot.type).toBe('snapshot')
    if (snapshot.type === 'snapshot') {
      expect(snapshot.snapshot.descriptor.title).toBe('电气健康')
      expect(snapshot.snapshot.descriptor.measurements[0].id).toBe('c_volt')
    }
  })

  it('rejects snapshots with an invalid configuration descriptor', () => {
    expect(() => parseServerMessage(JSON.stringify({
      v: 1,
      type: 'snapshot',
      seq: 4,
      snapshot: { descriptor: { title: 'missing required fields' } },
    }))).toThrow(/descriptor/i)
  })

  it('parses the allowlisted test configuration catalog', () => {
    const catalog = parseTestConfigCatalog({
      selectedConfigId: 'mbddf-system-status',
      configs: [
        {
          configId: 'mbddf-elec-health',
          title: '电气健康',
          description: '读取电源与辅助电压健康量。',
          algorithmId: 'mbddf.elec_health_status',
        },
      ],
    })

    expect(catalog.selectedConfigId).toBe('mbddf-system-status')
    expect(catalog.configs[0].title).toBe('电气健康')
  })
})

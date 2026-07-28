import { describe, expect, it } from 'vitest'

import { HwtestClient, makeRequest, parseServerMessage, parseTestConfigCatalog } from './HwtestClient'

function digitalStimulusPayload(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    available: true,
    configured: true,
    switches: [
      { switchId: 'di0', dutBit: 0, label: 'DI 0', activeLevel: 'High' },
      { switchId: 'di8', dutBit: 8, label: 'DI 8', activeLevel: 'Low' },
    ],
    appliedMask: 0x101,
    revision: 7,
    lastWriteTimestampUs: 1_785_000_000_123_456,
    settlingMs: 20,
    errorCode: '',
    message: '',
    ...overrides,
  }
}

describe('HwtestClient protocol boundary', () => {
  it('builds a versioned start request without owning a repeat timer', () => {
    expect(JSON.parse(makeRequest('run-1', 'start', {
      mode: 'pc_periodic',
      intervalMs: 250,
      maxCycles: 0,
      saveData: true,
      algorithmParameters: { waveform: 4, ampl: 250 },
    }))).toEqual({
      v: 1,
      type: 'request',
      id: 'run-1',
      action: 'start',
      params: {
        mode: 'pc_periodic',
        intervalMs: 250,
        maxCycles: 0,
        saveData: true,
        algorithmParameters: { waveform: 4, ampl: 250 },
      },
    })
  })

  it('builds versioned digital stimulus requests with only their allowlisted parameters', () => {
    expect(JSON.parse(makeRequest('stimulus-1', 'setDigitalStimulus', {
      switchId: 'di0',
      active: true,
      expectedRevision: 7,
    }))).toEqual({
      v: 1,
      type: 'request',
      id: 'stimulus-1',
      action: 'setDigitalStimulus',
      params: { switchId: 'di0', active: true, expectedRevision: 7 },
    })
    expect(JSON.parse(makeRequest('stimulus-reset', 'resetDigitalStimulus'))).toEqual({
      v: 1,
      type: 'request',
      id: 'stimulus-reset',
      action: 'resetDigitalStimulus',
      params: {},
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
        streamElapsedUs: 2_500,
        cycleIndex: 2,
        values: { cpu_usage: 12.5 },
        tags: {},
      },
    }))
    expect(sample.type).toBe('sample')
    if (sample.type === 'sample') {
      expect(Object.hasOwn(sample.sample, 'streamElapsedUs')).toBe(true)
      expect(sample.sample.streamElapsedUs).toBe(2_500)
    }

    expect(() => parseServerMessage('{"v":2,"type":"hello"}'))
      .toThrow(/protocol/i)
  })

  it('keeps legacy samples compatible and validates stream time integers', () => {
    const samplePayload = {
      taskId: 'task-1',
      stepId: 'IMU_STREAM',
      channelId: 'IMU_STREAM',
      timestampUs: 1_785_000_000_123_456,
      cycleIndex: 1,
      values: {},
      tags: {},
    }
    const parseSample = (overrides: Record<string, unknown>) => parseServerMessage(JSON.stringify({
      v: 1,
      type: 'sample',
      seq: 8,
      sample: { ...samplePayload, ...overrides },
    }))

    const legacy = parseSample({})
    expect(legacy.type).toBe('sample')
    if (legacy.type === 'sample') {
      expect(Object.hasOwn(legacy.sample, 'streamElapsedUs')).toBe(false)
    }

    const atOrigin = parseSample({ streamElapsedUs: 0 })
    expect(atOrigin.type).toBe('sample')
    if (atOrigin.type === 'sample') {
      expect(atOrigin.sample.streamElapsedUs).toBe(0)
    }

    const atSafeLimit = parseSample({
      timestampUs: Number.MAX_SAFE_INTEGER,
      streamElapsedUs: Number.MAX_SAFE_INTEGER,
    })
    expect(atSafeLimit.type).toBe('sample')
    if (atSafeLimit.type === 'sample') {
      expect(atSafeLimit.sample.timestampUs).toBe(Number.MAX_SAFE_INTEGER)
      expect(atSafeLimit.sample.streamElapsedUs).toBe(Number.MAX_SAFE_INTEGER)
    }

    for (const invalid of [-1, 0.5, Number.MAX_SAFE_INTEGER + 1]) {
      expect(() => parseSample({ streamElapsedUs: invalid })).toThrow(/streamElapsedUs/i)
      expect(() => parseSample({ timestampUs: invalid })).toThrow(/timestampUs/i)
    }
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
        effectiveRunParameters: { waveform: 4, ampl: 250 },
        runMode: 'single',
        intervalMs: 1000,
        maxCycles: 1,
        cycleIndex: 0,
        sampleCount: 0,
        dataSaveEnabled: false,
        dataFilePath: '',
        dataSaveError: '',
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
          runParameterSchemaVersion: '1',
          runParameters: [
            {
              id: 'waveform',
              label: '波形',
              description: '所有启用通道共用',
              kind: 'choice',
              unit: '',
              required: true,
              minimumExclusive: false,
              maximumExclusive: false,
              choices: [
                { value: 0, label: '正弦波' },
                { value: 4, label: '连续对数扫频' },
              ],
            },
            {
              id: 'max_freq',
              label: '终止频率',
              description: '',
              kind: 'number',
              unit: 'Hz',
              required: true,
              minimum: 0,
              minimumExclusive: true,
              maximumExclusive: false,
              choices: [],
              visibleWhen: { parameter: 'waveform', equals: 4 },
            },
          ],
          runParameterDefaults: { waveform: 0, max_freq: 80 },
        },
      },
    }))

    expect(snapshot.type).toBe('snapshot')
    if (snapshot.type === 'snapshot') {
      expect(snapshot.snapshot.descriptor.title).toBe('电气健康')
      expect(snapshot.snapshot.descriptor.measurements[0].id).toBe('c_volt')
      expect(snapshot.snapshot.descriptor.runParameters[1].visibleWhen).toEqual({
        parameter: 'waveform',
        equals: 4,
      })
      expect(snapshot.snapshot.descriptor.runParameterDefaults.max_freq).toBe(80)
      expect(snapshot.snapshot.effectiveRunParameters.ampl).toBe(250)
      expect(snapshot.snapshot.digitalStimulus).toEqual({
        available: false,
        configured: false,
        switches: [],
        appliedMask: 0,
        revision: 0,
        lastWriteTimestampUs: 0,
        settlingMs: 0,
        errorCode: '',
        message: '',
      })
    }

    expect(() => parseServerMessage(JSON.stringify({
      v: 1,
      type: 'snapshot',
      seq: 4,
      snapshot: { dataSaveEnabled: 'true' },
    }))).toThrow(/dataSaveEnabled/i)
  })

  it('strictly parses digital stimulus snapshots and replies', () => {
    const stimulus = digitalStimulusPayload()
    const snapshot = parseServerMessage(JSON.stringify({
      v: 1,
      type: 'snapshot',
      seq: 5,
      snapshot: { digitalStimulus: stimulus },
    }))
    const reply = parseServerMessage(JSON.stringify({
      v: 1,
      type: 'reply',
      id: 'stimulus-2',
      ok: true,
      code: '',
      message: '',
      data: { digitalStimulus: stimulus },
    }))

    expect(snapshot.type).toBe('snapshot')
    if (snapshot.type === 'snapshot') {
      expect(snapshot.snapshot.digitalStimulus).toEqual(stimulus)
      expect(snapshot.snapshot.digitalStimulus.switches[1].activeLevel).toBe('Low')
    }
    expect(reply.type).toBe('reply')
    if (reply.type === 'reply') {
      expect(reply.data.digitalStimulus).toEqual(stimulus)
      expect(reply.data.digitalStimulus?.revision).toBe(7)
    }
  })

  it('keeps replies without digital stimulus data backwards compatible', () => {
    const reply = parseServerMessage(JSON.stringify({
      v: 1,
      type: 'reply',
      id: 'legacy-reply',
      ok: true,
      code: '',
      message: '',
      data: { controls: [] },
    }))

    expect(reply.type).toBe('reply')
    if (reply.type === 'reply') {
      expect(reply.data.controls).toEqual([])
      expect(reply.data.digitalStimulus).toBeUndefined()
    }
  })

  it('rejects malformed digital stimulus fields', () => {
    const cases: Array<{ name: string; stimulus: unknown }> = [
      { name: 'non-object state', stimulus: null },
      {
        name: 'unsupported physical level',
        stimulus: digitalStimulusPayload({
          switches: [{ switchId: 'di0', dutBit: 0, label: 'DI 0', activeLevel: 'Unknown' }],
        }),
      },
      {
        name: 'unsafe revision',
        stimulus: digitalStimulusPayload({ revision: Number.MAX_SAFE_INTEGER + 1 }),
      },
      {
        name: 'DUT bit outside WebSocket v1 range',
        stimulus: digitalStimulusPayload({
          switches: [{ switchId: 'di16', dutBit: 16, label: 'DI 16', activeLevel: 'High' }],
        }),
      },
      {
        name: 'mask outside WebSocket v1 range',
        stimulus: digitalStimulusPayload({ appliedMask: 0x1_0000 }),
      },
      {
        name: 'duplicate switch id',
        stimulus: digitalStimulusPayload({
          switches: [
            { switchId: 'di0', dutBit: 0, label: 'DI 0', activeLevel: 'High' },
            { switchId: 'di0', dutBit: 1, label: 'DI 1', activeLevel: 'High' },
          ],
        }),
      },
      {
        name: 'duplicate DUT bit',
        stimulus: digitalStimulusPayload({
          switches: [
            { switchId: 'di0', dutBit: 0, label: 'DI 0', activeLevel: 'High' },
            { switchId: 'di1', dutBit: 0, label: 'DI 1', activeLevel: 'Low' },
          ],
        }),
      },
    ]

    for (const testCase of cases) {
      expect(() => parseServerMessage(JSON.stringify({
        v: 1,
        type: 'snapshot',
        seq: 6,
        snapshot: { digitalStimulus: testCase.stimulus },
      })), testCase.name).toThrow(/digitalStimulus/i)
    }
  })

  it('rejects a matching pending request when its digital stimulus reply is malformed', async () => {
    const client = new HwtestClient('ws://127.0.0.1:18765/ws')
    const internal = client as unknown as {
      pending: Map<string, {
        resolve: () => void
        reject: (error: Error) => void
      }>
      handleMessage: (text: string) => void
    }
    let rejection: Error | undefined
    const completed = new Promise<void>((resolve) => {
      internal.pending.set('stimulus-malformed', {
        resolve: () => undefined,
        reject: (error) => {
          rejection = error
          resolve()
        },
      })
    })

    internal.handleMessage(JSON.stringify({
      v: 1,
      type: 'reply',
      id: 'stimulus-malformed',
      ok: true,
      code: '',
      message: '',
      data: { digitalStimulus: digitalStimulusPayload({ revision: 1.5 }) },
    }))
    await completed

    expect(rejection?.message).toMatch(/digitalStimulus/i)
    expect(internal.pending.has('stimulus-malformed')).toBe(false)
  })

  it('rejects snapshots with an invalid configuration descriptor', () => {
    expect(() => parseServerMessage(JSON.stringify({
      v: 1,
      type: 'snapshot',
      seq: 4,
      snapshot: { descriptor: { title: 'missing required fields' } },
    }))).toThrow(/descriptor/i)
  })

  it('rejects malformed run parameter schemas', () => {
    expect(() => parseServerMessage(JSON.stringify({
      v: 1,
      type: 'snapshot',
      seq: 8,
      snapshot: {
        descriptor: {
          configId: 'bad',
          productModel: '',
          productName: '',
          configVersion: '1',
          stepId: 'bad',
          testItemId: 'bad',
          algorithmId: 'bad',
          title: 'bad',
          description: '',
          supportedRunModes: ['single'],
          measurements: [],
          runParameterSchemaVersion: '1',
          runParameters: [{ id: 'x', kind: 'unsupported' }],
          runParameterDefaults: { x: 1 },
        },
      },
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

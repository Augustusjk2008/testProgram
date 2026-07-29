import { describe, expect, it } from 'vitest'

import { makeRequest, parseServerMessage } from './HwtestClient'

function descriptor(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    configId: 'mbddf-helm-stream',
    productModel: 'MB_DDF_v2',
    productName: 'MB_DDF helm',
    configVersion: '1.0.0',
    stepId: 'HELM_STREAM',
    testItemId: 'helm_stream',
    algorithmId: 'mbddf.helm_stream',
    title: '舵机连续测试',
    description: '连续舵机测试',
    supportedRunModes: ['device_stream'],
    measurements: [],
    runParameterSchemaVersion: '1',
    runParameters: [],
    runParameterDefaults: {},
    ...overrides,
  }
}

function metric(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    key: 'rmse',
    label: '均方根误差',
    unit: 'deg',
    status: 'available',
    value: 0.25,
    detail: '',
    ...overrides,
  }
}

function channelSummary(channel = 0, overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    channel,
    enabled: true,
    status: 'completed',
    warnings: [],
    commonMetrics: [metric()],
    waveformMetrics: [],
    bodeAvailable: true,
    bodePointCount: 3,
    reasonCode: '',
    message: '',
    ...overrides,
  }
}

function analysis(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    supported: true,
    analyzerId: 'mbddf.helm.performance',
    schemaVersion: '1',
    taskId: 'task-helm-1',
    analysisGeneration: 3,
    state: 'completed',
    progress: 100,
    stage: 'completed',
    message: '',
    reasonCode: 'analysis_resource_limit',
    resultFilePath: 'C:/results/task-helm-1.json',
    diagnosticInputFilePath: '',
    sourceSummary: { rawSampleCount: 1000, durationS: 2.5 },
    channelSummaries: [channelSummary()],
    ...overrides,
  }
}

function snapshotMessage(snapshot: Record<string, unknown>): string {
  return JSON.stringify({ v: 1, type: 'snapshot', seq: 1, snapshot })
}

function analysisReplyData(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    analysisResult: {
      channelSummary: channelSummary(),
      bode: {
        frequencyHz: [1, 10, 100],
        magnitudeDb: [0, null, -3],
        phaseDeg: [0, null, -45],
        pointStatus: ['valid', 'not_covered', 'valid'],
      },
      ...overrides,
    },
  }
}

describe('HwtestClient post-run analysis protocol', () => {
  it('normalizes absent analysis extensions from an old server', () => {
    const message = parseServerMessage(snapshotMessage({ descriptor: descriptor() }))

    expect(message.type).toBe('snapshot')
    if (message.type === 'snapshot') {
      expect(message.snapshot.descriptor.postRunAnalysis).toEqual({
        supported: false,
        analyzerId: '',
        schemaVersion: '',
      })
      expect(message.snapshot.analysis).toEqual({
        supported: false,
        analyzerId: '',
        schemaVersion: '',
        taskId: '',
        analysisGeneration: 0,
        state: 'none',
        progress: 0,
        stage: '',
        message: '',
        reasonCode: '',
        resultFilePath: '',
        diagnosticInputFilePath: '',
        sourceSummary: {},
        channelSummaries: [],
      })
    }
  })

  it('strictly parses the analysis capability and snapshot summary', () => {
    const message = parseServerMessage(snapshotMessage({
      descriptor: descriptor({
        postRunAnalysis: {
          supported: true,
          analyzerId: 'mbddf.helm.performance',
          schemaVersion: '1',
        },
      }),
      analysis: analysis(),
    }))

    expect(message.type).toBe('snapshot')
    if (message.type === 'snapshot') {
      expect(message.snapshot.descriptor.postRunAnalysis.supported).toBe(true)
      expect(message.snapshot.analysis.analysisGeneration).toBe(3)
      expect(message.snapshot.analysis.reasonCode).toBe('analysis_resource_limit')
      expect(message.snapshot.analysis.channelSummaries[0]?.commonMetrics[0]?.value).toBe(0.25)
    }
  })

  it('rejects malformed analysis capability and snapshot fields', () => {
    const malformedCapabilities = [
      { supported: 'true', analyzerId: 'mbddf.helm.performance', schemaVersion: '1' },
      { supported: true, analyzerId: 12, schemaVersion: '1' },
      { supported: true, analyzerId: 'mbddf.helm.performance' },
    ]
    for (const postRunAnalysis of malformedCapabilities) {
      expect(() => parseServerMessage(snapshotMessage({
        descriptor: descriptor({ postRunAnalysis }),
      }))).toThrow(/postRunAnalysis|descriptor/i)
    }

    const malformedAnalyses = [
      analysis({ analysisGeneration: Number.MAX_SAFE_INTEGER + 1 }),
      analysis({ state: 'almost-completed' }),
      analysis({ sourceSummary: [] }),
      analysis({ channelSummaries: [channelSummary(0), channelSummary(0)] }),
      analysis({ channelSummaries: [channelSummary(4)] }),
      analysis({ channelSummaries: [channelSummary(0, { status: 'failed' })] }),
      analysis({ channelSummaries: [channelSummary(0, { commonMetrics: [metric({ value: '0.25' })] })] }),
    ]
    for (const malformed of malformedAnalyses) {
      expect(() => parseServerMessage(snapshotMessage({ analysis: malformed }))).toThrow(/analysis/i)
    }
  })

  it('builds and strictly parses channel-scoped analysisResult replies', () => {
    expect(JSON.parse(makeRequest('analysis-1', 'analysisResult', {
      taskId: 'task-helm-1',
      analysisGeneration: 3,
      channel: 0,
    }))).toEqual({
      v: 1,
      type: 'request',
      id: 'analysis-1',
      action: 'analysisResult',
      params: { taskId: 'task-helm-1', analysisGeneration: 3, channel: 0 },
    })

    const message = parseServerMessage(JSON.stringify({
      v: 1,
      type: 'reply',
      id: 'analysis-1',
      ok: true,
      code: '',
      message: '',
      data: analysisReplyData(),
    }))
    expect(message.type).toBe('reply')
    if (message.type === 'reply') {
      expect(message.data.analysisResult?.bode.magnitudeDb).toEqual([0, null, -3])
    }
  })

  it('rejects invalid Bode projections but keeps old replies compatible', () => {
    const malformedReplies = [
      analysisReplyData({ bode: {
        frequencyHz: [1, 10], magnitudeDb: [0], phaseDeg: [0, 1], pointStatus: ['valid', 'valid'],
      } }),
      analysisReplyData({ bode: {
        frequencyHz: [0, 10], magnitudeDb: [0, 1], phaseDeg: [0, 1], pointStatus: ['valid', 'valid'],
      } }),
      analysisReplyData({ bode: {
        frequencyHz: [1, 10], magnitudeDb: [0, 1], phaseDeg: [0, 1], pointStatus: ['valid', 1],
      } }),
      analysisReplyData({ channelSummary: channelSummary(4) }),
    ]
    for (const data of malformedReplies) {
      expect(() => parseServerMessage(JSON.stringify({
        v: 1,
        type: 'reply',
        id: 'analysis-invalid',
        ok: true,
        code: '',
        message: '',
        data,
      }))).toThrow(/analysisResult|bode/i)
    }

    const legacy = parseServerMessage(JSON.stringify({
      v: 1,
      type: 'reply',
      id: 'legacy',
      ok: true,
      code: '',
      message: '',
      data: {},
    }))
    expect(legacy.type).toBe('reply')
    if (legacy.type === 'reply') expect(legacy.data.analysisResult).toBeUndefined()
  })
})

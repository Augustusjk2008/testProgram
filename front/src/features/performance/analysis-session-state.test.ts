import { describe, expect, it } from 'vitest'

import type { AnalysisChannel, AnalysisResult } from '../../shared/protocol'

import {
  AnalysisResultCache,
  analysisIdentityKey,
  analysisStageLabel,
  isAnalysisTerminal,
} from './analysis-session-state'

const firstIdentity = { taskId: 'task-a', analysisGeneration: 4 }
const secondIdentity = { taskId: 'task-a', analysisGeneration: 5 }
const otherTaskIdentity = { taskId: 'task-b', analysisGeneration: 4 }

function result(channel: AnalysisChannel): AnalysisResult {
  return {
    channelSummary: {
      channel,
      enabled: true,
      status: 'completed',
      warnings: [],
      commonMetrics: [],
      waveformMetrics: [],
      bodeAvailable: true,
      bodePointCount: 1,
      reasonCode: '',
      message: '',
    },
    bode: {
      frequencyHz: [1],
      magnitudeDb: [0],
      phaseDeg: [0],
      pointStatus: ['valid'],
    },
  }
}

describe('analysis session state', () => {
  it('uses taskId:generation keys and isolates all four channel results', () => {
    const cache = new AnalysisResultCache()
    expect(analysisIdentityKey(firstIdentity)).toBe('task-a:4')
    expect(cache.begin(firstIdentity)).toBe(true)
    expect(cache.store(firstIdentity, 0, result(0))).toBe(true)
    expect(cache.store(firstIdentity, 3, result(3))).toBe(true)
    expect(cache.get(firstIdentity, 0)).toEqual(result(0))
    expect(cache.get(firstIdentity, 1)).toBeUndefined()
    expect(cache.get(firstIdentity, 3)).toEqual(result(3))
  })

  it('clears every channel for a new identity and discards delayed replies', () => {
    const cache = new AnalysisResultCache()
    cache.begin(firstIdentity)
    cache.store(firstIdentity, 0, result(0))
    cache.store(firstIdentity, 1, result(1))

    expect(cache.begin(secondIdentity)).toBe(true)
    expect(cache.get(secondIdentity, 0)).toBeUndefined()
    expect(cache.get(firstIdentity, 0)).toBeUndefined()
    expect(cache.store(firstIdentity, 0, result(0))).toBe(false)
    expect(cache.get(secondIdentity, 0)).toBeUndefined()

    expect(cache.begin(otherTaskIdentity)).toBe(true)
    expect(cache.store(secondIdentity, 1, result(1))).toBe(false)
    expect(cache.get(otherTaskIdentity, 1)).toBeUndefined()
  })

  it('presents independent lifecycle stages without treating analysis as hardware stop', () => {
    expect(isAnalysisTerminal('calculating')).toBe(false)
    expect(isAnalysisTerminal('partial')).toBe(true)
    expect(analysisStageLabel('queued')).toBe('等待尾样本')
    expect(analysisStageLabel('calculating', 'channel_2')).toBe('计算舵 3')
    expect(analysisStageLabel('persisting')).toBe('保存结果')
  })
})

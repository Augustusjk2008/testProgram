import { describe, expect, it } from 'vitest'

import type { TestDescriptor } from '../../shared/protocol'
import {
  loadRunParameterValues,
  runParameterStorageKey,
  validateRunParameterValues,
  visibleRunParameters,
} from './run-parameters'

const descriptor: TestDescriptor = {
  configId: 'mbddf-helm-stream',
  productModel: 'MB_DDF_v2',
  productName: '舵机连续测试',
  configVersion: '1',
  stepId: 'HELM_STREAM',
  testItemId: 'helm_stream',
  algorithmId: 'mbddf.helm_stream',
  title: '舵机连续测试',
  description: '',
  supportedRunModes: ['device_stream'],
  measurements: [],
  runParameterSchemaVersion: '1',
  runParameters: [
    {
      id: 'waveform', label: '波形', description: '', kind: 'choice', unit: '', required: true,
      minimumExclusive: false, maximumExclusive: false,
      choices: [{ value: 0, label: '正弦波' }, { value: 4, label: '连续对数扫频' }],
    },
    {
      id: 'ampl', label: '幅值', description: '', kind: 'number', unit: '°', required: true,
      minimumExclusive: false, maximumExclusive: false, choices: [],
    },
    {
      id: 'max_freq', label: '终止频率', description: '', kind: 'number', unit: 'Hz', required: true,
      minimum: 0, minimumExclusive: true, maximumExclusive: false, choices: [],
      visibleWhen: { parameter: 'waveform', equals: 4 },
    },
  ],
  runParameterDefaults: { waveform: 0, ampl: 1.8, max_freq: 80 },
}

describe('algorithm-owned run parameters', () => {
  it('keys browser persistence by configuration and schema version', () => {
    expect(runParameterStorageKey(descriptor)).toBe(
      'hwtest.run-parameters.v1:mbddf-helm-stream:1',
    )
  })

  it('merges only known stored values over algorithm defaults without clamping angles', () => {
    expect(loadRunParameterValues(descriptor, JSON.stringify({
      waveform: 4,
      ampl: 250,
      removed_parameter: 7,
    }))).toEqual({ waveform: 4, ampl: 250, max_freq: 80 })
  })

  it('evaluates conditional visibility from current values', () => {
    expect(visibleRunParameters(descriptor, { ...descriptor.runParameterDefaults, waveform: 0 })
      .map((parameter) => parameter.id)).toEqual(['waveform', 'ampl'])
    expect(visibleRunParameters(descriptor, { ...descriptor.runParameterDefaults, waveform: 4 })
      .map((parameter) => parameter.id)).toEqual(['waveform', 'ampl', 'max_freq'])
  })

  it('validates finite values and exclusive bounds while leaving unrestricted angles unrestricted', () => {
    expect(validateRunParameterValues(descriptor, {
      waveform: 4, ampl: -1000, max_freq: 80,
    })).toBe('')
    expect(validateRunParameterValues(descriptor, {
      waveform: 4, ampl: Number.NaN, max_freq: 80,
    })).toMatch(/幅值/)
    expect(validateRunParameterValues(descriptor, {
      waveform: 4, ampl: 1, max_freq: 0,
    })).toMatch(/终止频率/)
  })
})

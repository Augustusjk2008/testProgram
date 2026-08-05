import { describe, expect, it } from 'vitest'

import type { TestDescriptor } from '../../shared/protocol'
import {
  loadRunParameterValues,
  normalizeGuardedRunParameterValues,
  persistableRunParameterValues,
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
  stoppable: true,
  supportedRunModes: ['device_stream'],
  measurements: [],
  taskMeasurements: [],
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
  postRunAnalysis: { supported: false, analyzerId: '', schemaVersion: '' },
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

  it('rejects an empty visible required value', () => {
    expect(validateRunParameterValues(descriptor, {
      ...descriptor.runParameterDefaults,
      ampl: '',
    })).toBe('幅值为必填项')
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

  it('does not load or serialize parameters explicitly marked as non-persistent', () => {
    const nonPersistentDescriptor = {
      ...descriptor,
      runParameters: [
        ...descriptor.runParameters,
        {
          id: 'test_mode', label: '测试模式', description: '', kind: 'choice', unit: '', required: true,
          minimumExclusive: false, maximumExclusive: false,
          choices: [{ value: 'automatic', label: '自动' }, { value: 'manual', label: '手动' }],
          persistValues: false,
        },
      ],
      runParameterDefaults: { ...descriptor.runParameterDefaults, test_mode: 'automatic' },
    } as TestDescriptor

    expect(loadRunParameterValues(nonPersistentDescriptor, JSON.stringify({
      waveform: 4,
      test_mode: 'manual',
    }))).toEqual({ waveform: 4, ampl: 1.8, max_freq: 80, test_mode: 'automatic' })
    expect(persistableRunParameterValues(nonPersistentDescriptor, {
      waveform: 4,
      ampl: 1.8,
      max_freq: 80,
      test_mode: 'manual',
    })).toEqual({ waveform: 4, ampl: 1.8, max_freq: 80 })
  })

  it('drops stale values when the selected algorithm exposes no editable run parameters', () => {
    const spiFlashDescriptor: TestDescriptor = {
      ...descriptor,
      configId: 'mbddf-spi-flash',
      algorithmId: 'mbddf.spi_flash',
      runParameterSchemaVersion: '',
      runParameters: [],
      runParameterDefaults: {},
    }

    expect(normalizeGuardedRunParameterValues(spiFlashDescriptor, {
      memperf_type: 1,
      length: 4096,
      seed: 7,
    })).toEqual({})
  })

  it('keeps the disabled DO5 and DO6 channels false when restoring or persisting output parameters', () => {
    const channels = Array.from({ length: 16 }, (_, index) => ({
      id: `channel_enabled[${index}]`, label: `DO${index}`, description: '', kind: 'boolean' as const,
      unit: '', required: true, minimumExclusive: false, maximumExclusive: false, choices: [],
    }))
    const doDescriptor: TestDescriptor = {
      ...descriptor,
      configId: 'mbddf-do-write',
      algorithmId: 'mbddf.do_write',
      runParameters: channels,
      runParameterDefaults: Object.fromEntries(channels.map((channel) => [channel.id, false])),
    }
    const unsafe = Object.fromEntries(channels.map((channel) => [channel.id, true]))

    expect(loadRunParameterValues(doDescriptor, JSON.stringify(unsafe))).toMatchObject({
      'channel_enabled[4]': true,
      'channel_enabled[5]': false,
      'channel_enabled[6]': false,
    })
    expect(persistableRunParameterValues(doDescriptor, unsafe)).toMatchObject({
      'channel_enabled[5]': false,
      'channel_enabled[6]': false,
    })
  })

  it('canonicalizes restored DH checkbox values to protocol 0 or 1', () => {
    const dhDescriptor: TestDescriptor = {
      ...descriptor,
      configId: 'mbddf-dh-ignite-stream',
      algorithmId: 'mbddf.dh_ignite_stream',
      runParameters: [
        {
          id: 'power_enable', label: '点火电源使能', description: '', kind: 'integer', unit: '',
          required: true, minimumExclusive: false, maximumExclusive: false, choices: [],
        },
        {
          id: 'return_enable', label: '点火回线使能', description: '', kind: 'integer', unit: '',
          required: true, minimumExclusive: false, maximumExclusive: false, choices: [],
        },
      ],
      runParameterDefaults: { power_enable: 0, return_enable: 0 },
    }

    expect(loadRunParameterValues(dhDescriptor, JSON.stringify({
      power_enable: 255,
      return_enable: true,
    }))).toEqual({ power_enable: 0, return_enable: 1 })
    expect(persistableRunParameterValues(dhDescriptor, {
      power_enable: 255,
      return_enable: 1,
    })).toEqual({ power_enable: 0, return_enable: 1 })
  })
})

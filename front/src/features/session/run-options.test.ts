import { describe, expect, it } from 'vitest'

import { normalizeRunOptionsForStart, validatePcPeriodicOptions } from './run-options'

describe('continuous data run options', () => {
  it('accepts zero periodic delay and still rejects negative delays', () => {
    expect(validatePcPeriodicOptions(0, 0)).toBe('')
    expect(validatePcPeriodicOptions(-1, 0)).toMatch(/0–3,600,000/)
    expect(validatePcPeriodicOptions(3_600_001, 0)).toMatch(/0–3,600,000/)
  })

  it('keeps zero periodic delay unchanged for start', () => {
    expect(normalizeRunOptionsForStart({
      mode: 'pc_periodic',
      intervalMs: 0,
      maxCycles: 0,
      saveData: true,
      algorithmParameters: {},
    })).toMatchObject({
      mode: 'pc_periodic',
      intervalMs: 0,
      maxCycles: 0,
    })
  })

  it('keeps periodic saving enabled', () => {
    expect(normalizeRunOptionsForStart({
      mode: 'pc_periodic',
      intervalMs: 250,
      maxCycles: 0,
      saveData: true,
      algorithmParameters: { pulse_width: 123 },
    })).toEqual({
      mode: 'pc_periodic',
      intervalMs: 250,
      maxCycles: 0,
      saveData: true,
      algorithmParameters: { pulse_width: 123 },
    })
  })

  it('keeps an enabled device-stream save destination in the start request', () => {
    expect(normalizeRunOptionsForStart({
      mode: 'device_stream',
      intervalMs: 250,
      maxCycles: 0,
      saveData: true,
      dataDirectory: 'D:\\captures\\MB_DDF',
      dataFileName: 'continuous-run.txt',
      algorithmParameters: { waveform: 4 },
    })).toEqual({
      mode: 'device_stream',
      intervalMs: 1000,
      maxCycles: 1,
      saveData: true,
      dataDirectory: 'D:\\captures\\MB_DDF',
      dataFileName: 'continuous-run.txt',
      algorithmParameters: { waveform: 4 },
    })
  })

  it('omits save destinations when periodic saving is disabled', () => {
    expect(normalizeRunOptionsForStart({
      mode: 'pc_periodic',
      intervalMs: 250,
      maxCycles: 0,
      saveData: false,
      dataDirectory: 'D:\\captures\\MB_DDF',
      dataFileName: 'continuous-run.txt',
      algorithmParameters: { waveform: 4 },
    })).toEqual({
      mode: 'pc_periodic',
      intervalMs: 250,
      maxCycles: 0,
      saveData: false,
      algorithmParameters: { waveform: 4 },
    })
  })

  it('forces single runs not to save', () => {
    expect(normalizeRunOptionsForStart({
      mode: 'single',
      intervalMs: 250,
      maxCycles: 8,
      saveData: true,
      algorithmParameters: { config_enable: 1 },
    })).toEqual({
      mode: 'single',
      intervalMs: 1000,
      maxCycles: 1,
      saveData: false,
      algorithmParameters: { config_enable: 1 },
    })
  })

  it('keeps device-stream saving enabled without carrying PC periodic settings', () => {
    expect(normalizeRunOptionsForStart({
      mode: 'device_stream',
      intervalMs: 250,
      maxCycles: 0,
      saveData: true,
      algorithmParameters: { waveform: 4 },
    })).toEqual({
      mode: 'device_stream',
      intervalMs: 1000,
      maxCycles: 1,
      saveData: true,
      algorithmParameters: { waveform: 4 },
    })
  })
})

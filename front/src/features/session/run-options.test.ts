import { describe, expect, it } from 'vitest'

import { normalizeRunOptionsForStart } from './run-options'

describe('continuous data run options', () => {
  it('keeps periodic saving enabled', () => {
    expect(normalizeRunOptionsForStart({
      mode: 'pc_periodic',
      intervalMs: 250,
      maxCycles: 0,
      saveData: true,
    })).toEqual({
      mode: 'pc_periodic',
      intervalMs: 250,
      maxCycles: 0,
      saveData: true,
    })
  })

  it('forces single runs not to save', () => {
    expect(normalizeRunOptionsForStart({
      mode: 'single',
      intervalMs: 250,
      maxCycles: 8,
      saveData: true,
    })).toEqual({
      mode: 'single',
      intervalMs: 1000,
      maxCycles: 1,
      saveData: false,
    })
  })

  it('keeps device-stream saving enabled without carrying PC periodic settings', () => {
    expect(normalizeRunOptionsForStart({
      mode: 'device_stream',
      intervalMs: 250,
      maxCycles: 0,
      saveData: true,
    })).toEqual({
      mode: 'device_stream',
      intervalMs: 1000,
      maxCycles: 1,
      saveData: true,
    })
  })
})

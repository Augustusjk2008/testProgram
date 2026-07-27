import { describe, expect, it } from 'vitest'

import type { TestMeasurementDescriptor } from './protocol'
import {
  connectionStateLabel,
  fieldLabel,
  fieldUnit,
  formatDuration,
  phaseLabel,
  verdictLabel,
} from './format'

describe('descriptor-backed field formatting', () => {
  const measurements: TestMeasurementDescriptor[] = [
    { id: 'c_volt', label: 'C 路电压', unit: 'V', primary: true },
  ]

  it('uses labels and units supplied by the active test descriptor', () => {
    expect(fieldLabel('c_volt', measurements)).toBe('C 路电压')
    expect(fieldUnit('c_volt', measurements)).toBe('V')
  })

  it('retains a readable fallback for newly discovered fields', () => {
    expect(fieldLabel('future_sensor_value', measurements)).toBe('future sensor value')
    expect(fieldUnit('future_sensor_value', measurements)).toBe('')
  })
})

describe('Chinese console formatting', () => {
  it('localizes connection, phase, and verdict values', () => {
    expect(connectionStateLabel('connected')).toBe('已连接')
    expect(phaseLabel('pc_vendor_extension')).toBe('pc_vendor_extension')
    expect(phaseLabel('shutdown_failed')).toBe('关闭失败')
    expect(verdictLabel('Pass')).toBe('通过')
    expect(verdictLabel('')).toBe('待判定')
  })

  it('formats durations without English abbreviations', () => {
    expect(formatDuration(90)).toBe('0时 1分')
    expect(formatDuration(90_000)).toBe('1天 1时')
  })
})

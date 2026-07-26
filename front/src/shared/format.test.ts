import { describe, expect, it } from 'vitest'

import type { TestMeasurementDescriptor } from './protocol'
import { fieldLabel, fieldUnit } from './format'

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

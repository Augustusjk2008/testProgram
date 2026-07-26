import { describe, expect, it } from 'vitest'

import { buildChartGroups, createAssignments } from './series-config'
import type { TestMeasurementDescriptor } from '../../shared/protocol'

describe('series chart grouping', () => {
  const assignments = [
    { path: 'cpu_usage', enabled: true, groupId: 'load' },
    { path: 'mem_usage', enabled: true, groupId: 'load' },
    { path: 'cpu_temp', enabled: true, groupId: 'thermal' },
    { path: 'power_on_sec', enabled: false, groupId: 'runtime' },
  ]

  it('can place every selected quantity in one plot', () => {
    expect(buildChartGroups(assignments, 'combined')).toEqual([
      { id: 'combined', fields: ['cpu_usage', 'mem_usage', 'cpu_temp'] },
    ])
  })

  it('can place each selected quantity in its own plot', () => {
    expect(buildChartGroups(assignments, 'separate')).toEqual([
      { id: 'cpu_usage', fields: ['cpu_usage'] },
      { id: 'mem_usage', fields: ['mem_usage'] },
      { id: 'cpu_temp', fields: ['cpu_temp'] },
    ])
  })

  it('keeps user-defined groups and appends newly discovered fields', () => {
    expect(buildChartGroups(assignments, 'custom')).toEqual([
      { id: 'load', fields: ['cpu_usage', 'mem_usage'] },
      { id: 'thermal', fields: ['cpu_temp'] },
    ])
    expect(createAssignments(['cpu_usage', 'rk_temp'], assignments)).toEqual([
      ...assignments,
      { path: 'rk_temp', enabled: true, groupId: 'temperature' },
    ])
  })

  it('uses descriptor primary flags for the initial chart selection', () => {
    const measurements: TestMeasurementDescriptor[] = [
      { id: 'c_volt', label: 'C 路电压', unit: 'V', primary: true },
      { id: 'activate_bits', label: '激活位', unit: '', primary: false },
    ]

    expect(createAssignments(['c_volt', 'activate_bits'], [], measurements)).toEqual([
      { path: 'c_volt', enabled: true, groupId: 'system' },
      { path: 'activate_bits', enabled: false, groupId: 'system' },
    ])
  })
})

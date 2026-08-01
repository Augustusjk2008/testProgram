import { describe, expect, it } from 'vitest'

import { selectInitialTestConfig } from './config-selection'

const configs = [
  { configId: 'system-status', title: '系统状态', description: '', algorithmId: 'mbddf.system_status' },
  { configId: 'elec-health', title: '电气健康', description: '', algorithmId: 'mbddf.elec_health_status' },
]

describe('initial test configuration selection', () => {
  it('prefers the backend selected configuration', () => {
    expect(selectInitialTestConfig({ selectedConfigId: 'elec-health', configs })).toEqual(configs[1])
  })

  it('falls back to the first catalog entry', () => {
    expect(selectInitialTestConfig({ selectedConfigId: 'missing', configs })).toEqual(configs[0])
  })

  it('does not auto-load a configuration that the richer catalog marks disabled', () => {
    expect(selectInitialTestConfig(
      { selectedConfigId: 'elec-health', configs },
      new Set(['elec-health']),
    )).toEqual(configs[0])
  })

  it('returns null for an empty catalog', () => {
    expect(selectInitialTestConfig({ selectedConfigId: '', configs: [] })).toBeNull()
  })
})

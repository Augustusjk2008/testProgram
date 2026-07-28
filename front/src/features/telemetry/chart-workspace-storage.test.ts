import { describe, expect, it } from 'vitest'

import { chartWorkspaceStorageKey } from './chart-workspace-storage'

describe('chartWorkspaceStorageKey', () => {
  it('isolates workspaces by config id', () => {
    const first = chartWorkspaceStorageKey('mbddf-system-status', 'mbddf.system_status')
    const second = chartWorkspaceStorageKey('mbddf-elec-health', 'mbddf.system_status')

    expect(first).not.toBe(second)
    expect(first).toContain('mbddf-system-status')
    expect(second).toContain('mbddf-elec-health')
  })

  it('falls back to algorithm id before a config id is available', () => {
    expect(chartWorkspaceStorageKey('', 'mbddf.imu_stream')).toContain('mbddf.imu_stream')
    expect(chartWorkspaceStorageKey('', '')).toContain('unselected')
  })
})

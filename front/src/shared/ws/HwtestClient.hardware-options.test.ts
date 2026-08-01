import { describe, expect, it } from 'vitest'

import { parseHardwareOptions } from './HwtestClient'

describe('hardware options protocol', () => {
  it('parses detected NI devices while retaining manual-entry capability', () => {
    expect(parseHardwareOptions({
      state: 'available',
      message: '',
      allowManualEntry: true,
      devices: [{
        deviceName: 'PXI1Slot2',
        deviceId: 'PXI1Slot2',
        model: 'PXI-6259',
        serialNumber: '62590002',
        supportedModules: ['analog', 'digital', 'counter'],
      }],
    })).toEqual({
      state: 'available',
      message: '',
      allowManualEntry: true,
      devices: [{
        deviceName: 'PXI1Slot2',
        deviceId: 'PXI1Slot2',
        model: 'PXI-6259',
        serialNumber: '62590002',
        supportedModules: ['analog', 'digital', 'counter'],
      }],
    })
  })

  it('rejects unknown states and duplicate device names', () => {
    expect(() => parseHardwareOptions({
      state: 'busy', message: '', allowManualEntry: true, devices: [],
    })).toThrow('Invalid protocol hardwareOptions')
    expect(() => parseHardwareOptions({
      state: 'available', message: '', allowManualEntry: true,
      devices: [
        { deviceName: 'Dev1', deviceId: '1', model: 'PXI-6259', serialNumber: '1', supportedModules: [] },
        { deviceName: 'dev1', deviceId: '2', model: 'PXI-6733', serialNumber: '2', supportedModules: [] },
      ],
    })).toThrow('Invalid protocol hardwareOptions')
  })
})

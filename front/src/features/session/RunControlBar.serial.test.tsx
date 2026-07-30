import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'

import {
  EMPTY_SNAPSHOT,
  EMPTY_TEST_DESCRIPTOR,
  type ApplicationSnapshot,
} from '../../shared/protocol'

const sessionHooks = vi.hoisted(() => ({
  useSession: vi.fn(),
  useTelemetry: vi.fn(),
}))

vi.mock('./SessionProvider', () => sessionHooks)

import { RunControlBar } from './RunControlBar'

function renderSerialControl(
  testMode: 0 | 1,
  phase: ApplicationSnapshot['phase'] = 'configured',
): string {
  const snapshot: ApplicationSnapshot = {
    ...EMPTY_SNAPSHOT,
    phase,
    serialPortName: 'COM7',
    auxiliarySerialPortName: '',
    descriptor: {
      ...EMPTY_TEST_DESCRIPTOR,
      configId: 'mbddf-serial-test',
      algorithmId: 'mbddf.serial_test',
      title: '串口测试',
      supportedRunModes: ['single'],
      runParameterSchemaVersion: '1',
      runParameters: [{
        id: 'test_mode',
        label: '测试方式',
        description: '',
        kind: 'choice',
        unit: '',
        required: true,
        minimumExclusive: false,
        maximumExclusive: false,
        choices: [
          { value: 0, label: '回环' },
          { value: 1, label: '回显' },
        ],
      }],
      runParameterDefaults: { test_mode: testMode },
    },
  }
  sessionHooks.useSession.mockReturnValue({
    actionError: '',
    busyAction: null,
    connectionState: 'connected',
    connect: vi.fn(),
    invoke: vi.fn(),
    serialPorts: [
      { portName: 'COM7', description: '控制口', manufacturer: '', serialNumber: '', systemLocation: '' },
      { portName: 'COM8', description: '本地回显口', manufacturer: '', serialNumber: '', systemLocation: '' },
    ],
    snapshot,
    selectedConfigId: 'mbddf-serial-test',
    start: vi.fn(),
    testConfigs: [{ configId: 'mbddf-serial-test', title: '串口测试', description: '', algorithmId: 'mbddf.serial_test' }],
    testConfigsReady: true,
  })
  sessionHooks.useTelemetry.mockReturnValue({ latestSample: null })
  return renderToStaticMarkup(<RunControlBar />)
}

describe('RunControlBar unified serial local port', () => {
  it('shows only non-control system ports for echo mode', () => {
    const markup = renderSerialControl(1)

    expect(markup).toContain('本地（PC）串口')
    expect(markup).toContain('COM8')
    expect(markup).not.toContain('>COM7<')
    expect(markup).toContain('请选择本地（PC）串口')
  })

  it('hides the local PC port for loopback mode', () => {
    expect(renderSerialControl(0)).not.toContain('本地（PC）串口')
  })

  it('locks serial topology controls after prepare', () => {
    const markup = renderSerialControl(1, 'ready')
    const selector = markup.match(/<select\b[^>]*aria-label="本地（PC）串口"[^>]*>/)?.[0]
    const modeSelector = markup.match(/<select\b[^>]*aria-label="测试方式"[^>]*>/)?.[0]

    expect(selector).toContain('disabled=""')
    expect(modeSelector).not.toContain('disabled=""')
  })
})

import { renderToStaticMarkup } from 'react-dom/server'
import { beforeEach, describe, expect, it, vi } from 'vitest'

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

function renderControlBar(phase: ApplicationSnapshot['phase'], stoppable: boolean): string {
  const snapshot: ApplicationSnapshot = {
    ...EMPTY_SNAPSHOT,
    phase,
    descriptor: {
      ...EMPTY_TEST_DESCRIPTOR,
      configId: 'mbddf-device-stream',
      title: '设备持续测试',
      supportedRunModes: ['device_stream'],
      stoppable,
    },
  }
  sessionHooks.useSession.mockReturnValue({
    actionError: '',
    busyAction: null,
    connectionState: 'connected',
    connect: vi.fn(),
    invoke: vi.fn(),
    snapshot,
    selectedConfigId: 'mbddf-device-stream',
    start: vi.fn(),
    testConfigs: [{ configId: 'mbddf-device-stream', title: '设备持续测试', description: '', algorithmId: 'mbddf.device_stream' }],
    testConfigsReady: true,
  })
  return renderToStaticMarkup(<RunControlBar />)
}

function testConfigSelect(markup: string): string {
  const select = markup.match(/<select\b[^>]*aria-label="测试项目"[^>]*>/)?.[0]
  expect(select).toBeDefined()
  return select ?? ''
}

function renderHelmBoardControlBar(testMode: number): string {
  const snapshot: ApplicationSnapshot = {
    ...EMPTY_SNAPSHOT,
    phase: 'ready',
    descriptor: {
      ...EMPTY_TEST_DESCRIPTOR,
      configId: 'mbddf-helm-board-test',
      algorithmId: 'mbddf.helm_board_test',
      title: '舵机板级测试',
      supportedRunModes: ['single'],
      runParameterSchemaVersion: '1',
      runParameterDefaults: { test_mode: testMode },
    },
  }
  sessionHooks.useSession.mockReturnValue({
    actionError: '',
    busyAction: null,
    connectionState: 'connected',
    connect: vi.fn(),
    invoke: vi.fn(),
    snapshot,
    selectedConfigId: 'mbddf-helm-board-test',
    start: vi.fn(),
    testConfigs: [{ configId: 'mbddf-helm-board-test', title: '舵机板级测试', description: '', algorithmId: 'mbddf.helm_board_test' }],
    testConfigsReady: true,
  })
  return renderToStaticMarkup(<RunControlBar />)
}

beforeEach(() => {
  sessionHooks.useTelemetry.mockReturnValue({
    telemetryStats: {
      receivedCount: 0,
      retainedCount: 0,
      evictedCount: 0,
      sequenceStatus: 'continuous',
    },
  })
})

describe('RunControlBar stoppable descriptor capability', () => {
  it('hides pause and stop while locking configuration for a non-stoppable running descriptor', () => {
    const markup = renderControlBar('running', false)

    expect(testConfigSelect(markup)).toContain('disabled=""')
    expect(markup).not.toContain('暂停')
    expect(markup).not.toContain('停止')
  })

  it('hides resume and stop for a non-stoppable paused descriptor', () => {
    const markup = renderControlBar('paused', false)

    expect(testConfigSelect(markup)).toContain('disabled=""')
    expect(markup).not.toContain('继续')
    expect(markup).not.toContain('停止')
  })

  it.each(['finished', 'stopped', 'error'])('unlocks configuration after terminal phase %s', (phase) => {
    const markup = renderControlBar(phase, false)

    expect(testConfigSelect(markup)).not.toContain('disabled')
  })
})

describe('RunControlBar helm board fixture warning', () => {
  it('warns that HelmControl must be stopped for automatic mode only', () => {
    expect(renderHelmBoardControlBar(0)).toContain('MB_DDF_v2_HelmControl 已停止')
    expect(renderHelmBoardControlBar(1)).not.toContain('MB_DDF_v2_HelmControl 已停止')
  })
})

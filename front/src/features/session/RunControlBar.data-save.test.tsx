import { renderToStaticMarkup } from 'react-dom/server'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

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

function renderControlBar(runOptions: Record<string, unknown>): string {
  vi.stubGlobal('window', {
    localStorage: {
      getItem: () => JSON.stringify(runOptions),
    },
  })
  const snapshot: ApplicationSnapshot = {
    ...EMPTY_SNAPSHOT,
    phase: 'ready',
    descriptor: {
      ...EMPTY_TEST_DESCRIPTOR,
      configId: 'mbddf-system-status',
      title: '系统状态',
      supportedRunModes: ['pc_periodic'],
    },
  }
  sessionHooks.useSession.mockReturnValue({
    actionError: '',
    busyAction: null,
    connectionState: 'connected',
    connect: vi.fn(),
    invoke: vi.fn(),
    snapshot,
    selectedConfigId: 'mbddf-system-status',
    start: vi.fn(),
    testConfigs: [{
      configId: 'mbddf-system-status',
      title: '系统状态',
      description: '',
      algorithmId: 'mbddf.system_status',
    }],
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

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('RunControlBar continuous data destination', () => {
  it('shows the directory and file-name inputs after continuous saving is enabled', () => {
    const markup = renderControlBar({
      mode: 'pc_periodic',
      intervalMs: 500,
      maxCycles: 0,
      saveData: true,
      dataDirectory: 'D:\\captures\\MB_DDF',
      dataFileName: 'continuous-run.txt',
    })

    expect(markup).toContain('aria-label="保存目录"')
    expect(markup).toContain('aria-label="文件名"')
    expect(markup).toContain('value="D:\\captures\\MB_DDF"')
    expect(markup).toContain('value="continuous-run.txt"')
  })

  it('hides destination inputs while continuous saving is disabled', () => {
    const markup = renderControlBar({
      mode: 'pc_periodic',
      intervalMs: 500,
      maxCycles: 0,
      saveData: false,
      dataDirectory: 'D:\\captures\\MB_DDF',
      dataFileName: 'continuous-run.txt',
    })

    expect(markup).not.toContain('aria-label="保存目录"')
    expect(markup).not.toContain('aria-label="文件名"')
  })
})

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

function renderControlBar(): string {
  const snapshot: ApplicationSnapshot = {
    ...EMPTY_SNAPSHOT,
    phase: 'ready',
    descriptor: {
      ...EMPTY_TEST_DESCRIPTOR,
      configId: 'enabled.testcfg.json',
      supportedRunModes: ['single'],
    },
  }
  sessionHooks.useSession.mockReturnValue({
    actionError: '',
    busyAction: null,
    configCatalog: {
      revision: 'catalog-r1',
      items: [
        {
          documentId: 'enabled.testcfg.json',
          configId: 'enabled.testcfg.json',
          title: '目录刷新后的测试',
          enabled: true,
          order: 0,
          valid: true,
          message: '',
        },
        {
          documentId: 'disabled.testcfg.json',
          configId: 'disabled.testcfg.json',
          title: '已禁用测试',
          enabled: false,
          order: 1,
          valid: true,
          message: '维护中',
        },
      ],
    },
    connectionState: 'connected',
    connect: vi.fn(),
    invoke: vi.fn(),
    selectedConfigId: 'enabled.testcfg.json',
    snapshot,
    start: vi.fn(),
    testConfigs: [
      { configId: 'enabled.testcfg.json', title: '旧目录标题', description: '', algorithmId: 'mbddf.enabled' },
      { configId: 'disabled.testcfg.json', title: '已禁用测试', description: '', algorithmId: 'mbddf.disabled' },
    ],
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

describe('RunControlBar configuration catalog availability', () => {
  it('uses refreshed enabled catalog entries for user initiated selection', () => {
    const markup = renderControlBar()
    const select = markup.match(/<select\b[^>]*aria-label="测试项目"[^>]*>[\s\S]*?<\/select>/)?.[0] ?? ''

    expect(select).toContain('目录刷新后的测试')
    expect(select).not.toContain('旧目录标题')
    expect(select).not.toContain('已禁用测试')
    expect(select).toContain('disabled=""')
  })
})

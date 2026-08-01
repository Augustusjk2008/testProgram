import { renderToStaticMarkup } from 'react-dom/server'
import { beforeEach, describe, expect, it, vi } from 'vitest'

import {
  EMPTY_SNAPSHOT,
  EMPTY_TEST_DESCRIPTOR,
  type ApplicationSnapshot,
} from '../shared/protocol'

const sessionHooks = vi.hoisted(() => ({
  useSession: vi.fn(),
  useTelemetry: vi.fn(),
}))

const themeHooks = vi.hoisted(() => ({
  useTheme: vi.fn(),
}))

vi.mock('../features/session/SessionProvider', () => sessionHooks)
vi.mock('./ThemeProvider', () => themeHooks)

import { AppShell } from './AppShell'

function renderShell(algorithmId: string): string {
  const snapshot: ApplicationSnapshot = {
    ...EMPTY_SNAPSHOT,
    algorithmId,
    descriptor: {
      ...EMPTY_TEST_DESCRIPTOR,
      algorithmId,
    },
  }
  sessionHooks.useSession.mockReturnValue({
    actionError: '',
    busyAction: null,
    connectionDetail: '',
    connectionState: 'connected',
    connect: vi.fn(),
    invoke: vi.fn(),
    selectedConfigId: '',
    snapshot,
    start: vi.fn(),
    testConfigs: [],
    testConfigsReady: true,
    wsUrl: 'ws://127.0.0.1:18765/ws',
  })
  sessionHooks.useTelemetry.mockReturnValue({
    latestSample: undefined,
    telemetryStats: {
      evictedCount: 0,
      receivedCount: 0,
      retainedCount: 0,
      sequenceStatus: 'continuous',
    },
  })
  return renderToStaticMarkup(
    <AppShell onPageChange={vi.fn()} page="overview"><div>内容</div></AppShell>,
  )
}

beforeEach(() => {
  themeHooks.useTheme.mockReturnValue({ theme: 'dark', toggleTheme: vi.fn() })
})

describe('board-test navigation', () => {
  it('always exposes the configuration page', () => {
    expect(renderShell('mbddf.unrelated')).toContain('<strong>配置</strong>')
  })

  it.each(['mbddf.do_write', 'mbddf.helm_board_test'])(
    'shows the board-test page only for %s',
    (algorithmId) => {
      expect(renderShell(algorithmId)).toContain('板级测试')
    },
  )

  it('does not expose the board-test page for unrelated algorithms', () => {
    expect(renderShell('mbddf.helm_stream')).not.toContain('板级测试')
  })
})

import { renderToStaticMarkup } from 'react-dom/server'
import { beforeEach, describe, expect, it, vi } from 'vitest'

import { EMPTY_SNAPSHOT } from '../shared/protocol'

const sessionHooks = vi.hoisted(() => ({
  useSession: vi.fn(),
}))

vi.mock('../features/session/SessionProvider', () => sessionHooks)

import { ConfigPage } from './ConfigPage'

beforeEach(() => {
  sessionHooks.useSession.mockReturnValue({
    configCatalog: {
      revision: 'catalog-r1',
      items: [
        {
          documentId: 'timer-jitter.testcfg.json',
          configId: 'mbddf.timer_jitter',
          title: '定时器抖动',
          enabled: false,
          order: 3,
          valid: true,
          message: '维护中',
        },
      ],
    },
    configCatalogError: '',
    configCatalogReady: true,
    connectionState: 'connected',
    getConfigDocument: vi.fn(),
    refreshConfigCatalog: vi.fn(),
    saveConfig: vi.fn(),
    snapshot: { ...EMPTY_SNAPSHOT, phase: 'running' },
  })
})

describe('ConfigPage', () => {
  it('shows the fixed documents and catalog entries while disabling saves during a run', () => {
    const markup = renderToStaticMarkup(<ConfigPage />)

    expect(markup).toContain('测试配置目录')
    expect(markup).toContain('MB_DDF 工位')
    expect(markup).toContain('定时器抖动')
    expect(markup).toContain('已停用')
    expect(markup).toContain('运行态禁止保存配置')
    const saveButton = markup.match(/<button\b[^>]*aria-label="保存配置"[^>]*>/)?.[0] ?? ''
    expect(saveButton).toContain('disabled=""')
  })
})

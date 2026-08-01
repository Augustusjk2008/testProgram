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
  it('uses product-facing configuration names and disables saves during a run', () => {
    const markup = renderToStaticMarkup(<ConfigPage />)

    expect(markup).toContain('测试项目管理')
    expect(markup).toContain('工位硬件')
    expect(markup).toContain('定时器抖动')
    expect(markup).toContain('已停用')
    expect(markup).toContain('测试正在运行，系统配置暂时只读')
    expect(markup).not.toContain('test-config-catalog')
    expect(markup).not.toContain('mbddf-station')
    expect(markup).not.toContain('timer-jitter.testcfg.json')
    const saveButton = markup.match(/<button\b[^>]*aria-label="保存配置"[^>]*>/)?.[0] ?? ''
    expect(saveButton).toContain('disabled=""')
  })
})

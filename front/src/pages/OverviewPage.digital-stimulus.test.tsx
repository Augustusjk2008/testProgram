import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'

import { EMPTY_SNAPSHOT } from '../shared/protocol'

const sessionHooks = vi.hoisted(() => ({
  useSession: vi.fn(),
  useTelemetry: vi.fn(),
}))

vi.mock('../features/session/SessionProvider', () => sessionHooks)

import { OverviewPage } from './OverviewPage'

describe('OverviewPage digital stimulus', () => {
  it('shows the configured three-channel DI fixture without requiring sixteen switches', () => {
    sessionHooks.useSession.mockReturnValue({
      connectionState: 'connected',
      resetDigitalStimulus: vi.fn(),
      setDigitalStimulus: vi.fn(),
      snapshot: {
        ...EMPTY_SNAPSHOT,
        digitalStimulus: {
          available: true,
          configured: true,
          switches: [
            { switchId: 'di3', dutBit: 3, label: 'DI3', activeLevel: 'High' },
            { switchId: 'di1', dutBit: 1, label: 'DI1', activeLevel: 'High' },
            { switchId: 'di2', dutBit: 2, label: 'DI2', activeLevel: 'High' },
          ],
          appliedMask: 0,
          revision: 0,
          lastWriteTimestampUs: 0,
          settlingMs: 100,
          errorCode: '',
          message: '',
        },
      },
    })
    sessionHooks.useTelemetry.mockReturnValue({ latestSample: null })

    const markup = renderToStaticMarkup(<OverviewPage />)

    expect(markup).toContain('3 路 DI 激励')
    expect(markup).toContain('aria-label="3 路数字激励开关"')
    expect(markup).toContain('<strong>DI3</strong>')
    expect(markup).toContain('<strong>DI1</strong>')
    expect(markup).toContain('<strong>DI2</strong>')
  })

  it('does not expose the internal digital stimulus revision', () => {
    sessionHooks.useSession.mockReturnValue({
      connectionState: 'connected',
      resetDigitalStimulus: vi.fn(),
      setDigitalStimulus: vi.fn(),
      snapshot: {
        ...EMPTY_SNAPSHOT,
        digitalStimulus: {
          available: true,
          configured: true,
          switches: [
            { switchId: 'di3', dutBit: 3, label: 'DI3', activeLevel: 'High' },
          ],
          appliedMask: 0,
          revision: 7,
          lastWriteTimestampUs: 0,
          settlingMs: 100,
          errorCode: '',
          message: '',
        },
      },
    })
    sessionHooks.useTelemetry.mockReturnValue({ latestSample: null })

    const markup = renderToStaticMarkup(<OverviewPage />)

    expect(markup).not.toContain('版本 7')
  })
})

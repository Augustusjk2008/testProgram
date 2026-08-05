import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'

import {
  EMPTY_SNAPSHOT,
  EMPTY_TEST_DESCRIPTOR,
  type ApplicationSample,
  type TestMeasurementDescriptor,
} from '../shared/protocol'

const sessionHooks = vi.hoisted(() => ({
  useSession: vi.fn(),
  useTelemetry: vi.fn(),
}))

vi.mock('../features/session/SessionProvider', () => sessionHooks)

import { OverviewPage } from './OverviewPage'

describe('OverviewPage DI readback', () => {
  it('renders all configured DI0-DI15 bit states instead of either source bitmap', () => {
    const labels = [
      'DI0 联锁、电气弹动',
      'DI1 引信报警',
      'DI2 引信起爆指令',
      'DI3 锁相环锁定指示',
      'DI4',
      'DI5',
      'DI6',
      'DI7',
      'DI8 投放允许',
      'DI9',
      'DI10',
      'DI11',
      'DI12',
      'DI13',
      'DI14',
      'DI15',
    ]
    const derived = labels.map((label, bitIndex) => ({
      id: `di${bitIndex}`,
      label,
      unit: '',
      primary: true,
      sourceId: 'di_state[0]',
      bitIndex,
    })) as unknown as TestMeasurementDescriptor[]
    const sources = [
      {
        id: 'di_state[0]', label: 'DI0..DI31 回读位图', unit: 'bitmask',
        primary: false, taskVisible: false,
      },
      {
        id: 'di_state[1]', label: 'DI32..DI63 诊断位图', unit: 'bitmask',
        primary: false, taskVisible: false,
      },
    ] as unknown as TestMeasurementDescriptor[]
    const sample: ApplicationSample = {
      taskId: 'di-task',
      stepId: 'DI_READ',
      channelId: 'DI_READ',
      timestampUs: 1_785_000_000_000_000,
      cycleIndex: 1,
      values: {
        'di_state[0]': 0x8105,
        'di_state[1]': 0xffff_ffff,
      },
      tags: {},
    }
    sessionHooks.useSession.mockReturnValue({
      connectionState: 'connected',
      resetDigitalStimulus: vi.fn(),
      setDigitalStimulus: vi.fn(),
      snapshot: {
        ...EMPTY_SNAPSHOT,
        algorithmId: 'mbddf.di_read',
        descriptor: {
          ...EMPTY_TEST_DESCRIPTOR,
          algorithmId: 'mbddf.di_read',
          title: '数字量输入',
          measurements: sources,
          taskMeasurements: derived,
        },
      },
    })
    sessionHooks.useTelemetry.mockReturnValue({ latestSample: sample })

    const markup = renderToStaticMarkup(<OverviewPage />)
    const cards = markup.match(/<article class="metric-card">.*?<\/article>/g) ?? []

    expect(cards).toHaveLength(16)
    labels.forEach((label, bitIndex) => {
      expect(cards[bitIndex]).toContain(label)
      expect(cards[bitIndex]).toContain(`metric-card__value">${[0, 2, 8, 15].includes(bitIndex) ? 1 : 0}`)
    })
    expect(markup).not.toContain('回读位图')
    expect(markup).not.toContain('诊断位图')
    expect(markup).not.toContain('bitmask')
  })
})

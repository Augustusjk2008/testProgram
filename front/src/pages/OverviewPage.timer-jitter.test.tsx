import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'

import {
  EMPTY_SNAPSHOT,
  EMPTY_TEST_DESCRIPTOR,
  type ApplicationSnapshot,
  type ApplicationSample,
} from '../shared/protocol'

const sessionHooks = vi.hoisted(() => ({
  useSession: vi.fn(),
  useTelemetry: vi.fn(),
}))

vi.mock('../features/session/SessionProvider', () => sessionHooks)

import { OverviewPage } from './OverviewPage'

function buckets(first: number, second: number): Record<string, number> {
  return {
    'buckets[0]': first,
    'buckets[1]': second,
    'buckets[2]': 0,
    'buckets[3]': 0,
    'buckets[4]': 0,
    'buckets[5]': 0,
    'buckets[6]': 0,
    'buckets[7]': 0,
  }
}

function configure(
  sample: ApplicationSample | null,
  responseValues: Record<string, unknown> = {},
  phase: ApplicationSnapshot['phase'] = sample === null ? 'finished' : 'running',
) {
  sessionHooks.useSession.mockReturnValue({
    connectionState: 'connected',
    resetDigitalStimulus: vi.fn(),
    setDigitalStimulus: vi.fn(),
    snapshot: {
      ...EMPTY_SNAPSHOT,
      phase,
      algorithmId: 'mbddf.timer_jitter',
      verdict: 'pass',
      rawData: { responseValues },
      descriptor: {
        ...EMPTY_TEST_DESCRIPTOR,
        algorithmId: 'mbddf.timer_jitter',
        title: '定时器抖动',
        measurements: [
          { id: 'avg_jitter', label: '平均抖动', unit: 'us', primary: true },
          { id: 'max_jitter', label: '最大抖动', unit: 'us', primary: true },
        ],
      },
    },
  })
  sessionHooks.useTelemetry.mockReturnValue({ latestSample: sample })
}

function sample(values: Record<string, unknown>): ApplicationSample {
  return {
    taskId: 'timer-task',
    stepId: 'TIMER_JITTER',
    channelId: 'TIMER_JITTER_START',
    timestampUs: 1_785_000_000_000_000,
    cycleIndex: 1,
    values,
    tags: {},
  }
}

describe('OverviewPage timer jitter distribution', () => {
  it('prefers the latest sample and renders eight precise histogram intervals', () => {
    configure(
      sample({ ...buckets(17, 233), avg_jitter: 1.25, max_jitter: 4.5 }),
      buckets(99, 151),
    )

    const markup = renderToStaticMarkup(<OverviewPage />)

    expect(markup).toContain('定时器抖动分布')
    expect(markup).toContain('[0, 2) µs')
    expect(markup).toContain('[64, 100) µs')
    expect(markup).toContain('≥100 µs')
    expect(markup).toContain('合计 250')
    expect(markup).toContain('>17<')
    expect(markup).not.toContain('>99<')
    expect(markup).not.toContain('统计不完整')
  })

  it('falls back to terminal snapshot response values after reconnect', () => {
    configure(null, buckets(250, 0))

    const markup = renderToStaticMarkup(<OverviewPage />)

    expect(markup).toContain('定时器抖动分布')
    expect(markup).toContain('合计 250')
    expect(markup).toContain('>250<')
  })

  it('prefers complete snapshot buckets when the latest sample only has status fields', () => {
    configure(
      sample({ status: 0, err_code: 0 }),
      { ...buckets(250, 0), status: 0, err_code: 0 },
      'finished',
    )

    const markup = renderToStaticMarkup(<OverviewPage />)

    expect(markup).toContain('合计 250')
    expect(markup).toContain('>250<')
    expect(markup).not.toContain('统计不完整')
  })

  it('does not mix terminal snapshot buckets into an active incomplete sample', () => {
    configure(
      sample({ status: 0, err_code: 0 }),
      { ...buckets(250, 0), status: 0, err_code: 0 },
      'running',
    )

    const markup = renderToStaticMarkup(<OverviewPage />)

    expect(markup).toContain('统计不完整')
    expect(markup).not.toContain('合计 250')
  })

  it('warns without changing the verdict when a successful histogram is incomplete', () => {
    configure(sample({ ...buckets(249, 0), avg_jitter: 1.25, max_jitter: 4.5 }))

    const markup = renderToStaticMarkup(<OverviewPage />)

    expect(markup).toContain('统计不完整')
    expect(markup).toContain('通过')
  })

  it('warns when a received timer response omits every bucket field', () => {
    configure(sample({ status: 0, err_code: 0, avg_jitter: 1.25, max_jitter: 4.5 }))

    const markup = renderToStaticMarkup(<OverviewPage />)

    expect(markup).toContain('定时器抖动分布')
    expect(markup).toContain('统计不完整')
  })

  it('does not draw a non-zero bar for a zero-count bucket', () => {
    configure(sample({ ...buckets(250, 0), status: 0, err_code: 0 }))

    const markup = renderToStaticMarkup(<OverviewPage />)

    expect(markup).toContain('height:0%')
  })
})

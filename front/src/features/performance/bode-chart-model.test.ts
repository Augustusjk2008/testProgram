import { describe, expect, it } from 'vitest'

import type { AnalysisChannel } from '../../shared/protocol'

import { createBodePlotModel, type BodeCurve } from './bode-chart-model'

function curve(channel: AnalysisChannel, overrides: Partial<BodeCurve> = {}): BodeCurve {
  return {
    channel,
    label: `舵 ${channel + 1}`,
    bode: {
      frequencyHz: [1, 10, 100],
      magnitudeDb: [0, null, -3],
      phaseDeg: [0, null, -45],
      pointStatus: ['valid', 'not_covered', 'valid'],
    },
    ...overrides,
  }
}

describe('Bode chart model', () => {
  it('uses a logarithmic frequency axis and preserves null holes for uPlot', () => {
    const model = createBodePlotModel([curve(0)])

    expect(model.xScale).toEqual({ distr: 3 })
    expect(model.frequency).toEqual([1, 10, 100])
    expect(model.magnitude.data).toEqual([[1, 10, 100], [0, null, -3]])
    expect(model.phase.data).toEqual([[1, 10, 100], [0, null, -45]])
    expect(model.magnitude.series[0]).toMatchObject({ channel: 0, spanGaps: false })
    expect(model.phase.series[0]).toMatchObject({ channel: 0, spanGaps: false })
  })

  it('aligns overlays without interpolating across another channel’s gap', () => {
    const model = createBodePlotModel([
      curve(0),
      curve(1, {
        bode: {
          frequencyHz: [1, 100],
          magnitudeDb: [2, -1],
          phaseDeg: [10, -30],
          pointStatus: ['valid', 'valid'],
        },
      }),
    ])

    expect(model.frequency).toEqual([1, 10, 100])
    expect(model.magnitude.data[2]).toEqual([2, null, -1])
    expect(model.phase.data[2]).toEqual([10, null, -30])
  })

  it('only converts the displayed frequency unit and leaves backend Hz data untouched', () => {
    const input = curve(0)
    const hertz = createBodePlotModel([input], 'hz')
    const radians = createBodePlotModel([input], 'rad/s')

    expect(hertz.frequency).toEqual([1, 10, 100])
    expect(radians.frequency).toEqual([2 * Math.PI, 20 * Math.PI, 200 * Math.PI])
    expect(radians.frequencyUnit).toBe('rad/s')
    expect(input.bode.frequencyHz).toEqual([1, 10, 100])
  })
})

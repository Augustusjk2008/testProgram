import type {
  AnalysisChannel,
  AnalysisMetric,
  BodeProjection,
} from '../../shared/protocol'

export type FrequencyUnit = 'hz' | 'rad/s'

export interface BodeCurve {
  channel: AnalysisChannel
  label: string
  bode: BodeProjection
  metrics?: readonly AnalysisMetric[]
}

export interface BodeSeries {
  channel: AnalysisChannel
  label: string
  spanGaps: false
}

export interface BodeMarker {
  frequency: number
  label: string
}

export interface BodePlotModel {
  frequencyUnit: FrequencyUnit
  frequency: number[]
  xScale: { distr: 3 }
  magnitude: {
    data: [number[], ...Array<Array<number | null>>]
    series: BodeSeries[]
  }
  phase: {
    data: [number[], ...Array<Array<number | null>>]
    series: BodeSeries[]
  }
  markers: BodeMarker[]
}

function displayFrequency(frequencyHz: number, unit: FrequencyUnit): number {
  return unit === 'rad/s' ? frequencyHz * 2 * Math.PI : frequencyHz
}

function markerFromMetric(metric: AnalysisMetric, channel: AnalysisChannel): BodeMarker | null {
  if (metric.value === null || !Number.isFinite(metric.value) || metric.value <= 0) return null
  const key = metric.key.toLowerCase()
  const channelLabel = `舵 ${channel + 1}`
  if (key.includes('bandwidth') && key.includes('1') && key.includes('db')) {
    return { frequency: metric.value, label: `${channelLabel} −1 dB` }
  }
  if (key.includes('bandwidth') && key.includes('3') && key.includes('db')) {
    return { frequency: metric.value, label: `${channelLabel} −3 dB` }
  }
  if (key.includes('resonance') && (key.includes('frequency') || key.includes('freq'))) {
    return { frequency: metric.value, label: `${channelLabel} 共振峰` }
  }
  const phaseFrequency = /phase.*(?:_|)(5|10|20)(?:_|)hz/.exec(key)
  if (phaseFrequency) {
    return { frequency: Number(phaseFrequency[1]), label: `${channelLabel} ${phaseFrequency[1]} Hz 相位` }
  }
  return null
}

function valuesAtFrequencies(
  sourceFrequencies: readonly number[],
  sourceValues: ReadonlyArray<number | null>,
  allFrequencies: readonly number[],
): Array<number | null> {
  const byFrequency = new Map<number, number | null>()
  sourceFrequencies.forEach((frequency, index) => {
    byFrequency.set(frequency, sourceValues[index] ?? null)
  })
  return allFrequencies.map((frequency) => byFrequency.get(frequency) ?? null)
}

export function createBodePlotModel(
  curves: readonly BodeCurve[],
  frequencyUnit: FrequencyUnit = 'hz',
): BodePlotModel {
  const allFrequencies = [...new Set(curves.flatMap(({ bode }) => bode.frequencyHz))]
    .sort((left, right) => left - right)
  const frequency = allFrequencies.map((value) => displayFrequency(value, frequencyUnit))
  const series = curves.map(({ channel, label }) => ({ channel, label, spanGaps: false as const }))
  const magnitudeSeries = curves.map(({ bode }) => (
    valuesAtFrequencies(bode.frequencyHz, bode.magnitudeDb, allFrequencies)
  ))
  const phaseSeries = curves.map(({ bode }) => (
    valuesAtFrequencies(bode.frequencyHz, bode.phaseDeg, allFrequencies)
  ))
  const markers = curves.flatMap(({ channel, metrics = [] }) => metrics
    .map((metric) => markerFromMetric(metric, channel))
    .filter((marker): marker is BodeMarker => marker !== null)
    .map((marker) => ({ ...marker, frequency: displayFrequency(marker.frequency, frequencyUnit) })))

  return {
    frequencyUnit,
    frequency,
    xScale: { distr: 3 },
    magnitude: { data: [frequency, ...magnitudeSeries], series },
    phase: { data: [frequency, ...phaseSeries], series },
    markers,
  }
}

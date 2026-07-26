import type { ApplicationSample } from '../../shared/protocol'

export type AlignedData = Array<Array<number | null>>

export interface QueryOptions {
  channelId?: string
  windowSeconds?: number
  pixelWidth?: number
}

function flattenFiniteNumbers(
  value: unknown,
  prefix = '',
  target: Record<string, number> = {},
): Record<string, number> {
  if (typeof value === 'number' && Number.isFinite(value) && prefix) {
    target[prefix] = value
    return target
  }
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    return target
  }
  Object.entries(value).forEach(([key, child]) => {
    flattenFiniteNumbers(child, prefix ? `${prefix}.${key}` : key, target)
  })
  return target
}

export class SampleBuffer {
  private readonly timestamps: Float64Array
  private readonly channels: string[]
  private readonly series = new Map<string, Float64Array>()
  private writeIndex = 0
  private pointCount = 0
  private latestSample: ApplicationSample | null = null

  constructor(readonly capacity: number) {
    if (!Number.isInteger(capacity) || capacity < 1) {
      throw new Error('Sample capacity must be a positive integer')
    }
    this.timestamps = new Float64Array(capacity)
    this.channels = Array.from({ length: capacity }, () => '')
  }

  get size(): number {
    return this.pointCount
  }

  append(sample: ApplicationSample): void {
    const index = this.writeIndex
    this.series.forEach((values) => { values[index] = Number.NaN })

    const flattened = flattenFiniteNumbers(sample.values)
    Object.entries(flattened).forEach(([field, value]) => {
      let values = this.series.get(field)
      if (!values) {
        values = new Float64Array(this.capacity)
        values.fill(Number.NaN)
        this.series.set(field, values)
      }
      values[index] = value
    })

    this.timestamps[index] = sample.timestampUs / 1_000_000
    this.channels[index] = sample.channelId
    this.latestSample = sample
    this.writeIndex = (index + 1) % this.capacity
    this.pointCount = Math.min(this.pointCount + 1, this.capacity)
  }

  clear(): void {
    this.writeIndex = 0
    this.pointCount = 0
    this.latestSample = null
    this.series.clear()
    this.channels.fill('')
  }

  latest(): ApplicationSample | null {
    return this.latestSample
  }

  fields(): string[] {
    return [...this.series.keys()].sort((left, right) => left.localeCompare(right))
  }

  query(fields: string[], options: QueryOptions = {}): AlignedData {
    const result: AlignedData = [[], ...fields.map(() => [])]
    if (this.pointCount === 0) return result

    const newestIndex = (this.writeIndex - 1 + this.capacity) % this.capacity
    const newestTime = this.timestamps[newestIndex]
    const cutoff = options.windowSeconds && options.windowSeconds > 0
      ? newestTime - options.windowSeconds
      : Number.NEGATIVE_INFINITY
    const first = this.pointCount === this.capacity ? this.writeIndex : 0
    const fieldSeries = fields.map((field) => this.series.get(field))

    for (let offset = 0; offset < this.pointCount; offset += 1) {
      const index = (first + offset) % this.capacity
      const timestamp = this.timestamps[index]
      if (timestamp < cutoff ||
          (options.channelId && this.channels[index] !== options.channelId)) {
        continue
      }
      result[0].push(timestamp)
      fields.forEach((field, fieldIndex) => {
        const value = fieldSeries[fieldIndex]?.[index]
        result[fieldIndex + 1].push(value === undefined || Number.isNaN(value) ? null : value)
      })
    }

    return options.pixelWidth
      ? downsampleAlignedMinMax(result, options.pixelWidth)
      : result
  }
}

export function downsampleAlignedMinMax(
  data: AlignedData,
  pixelWidth: number,
): AlignedData {
  const pointCount = data[0]?.length ?? 0
  const width = Math.max(1, Math.floor(pixelWidth))
  if (pointCount <= width * 2 || data.length < 2) return data

  const valueSeries = data.slice(1)
  const ranges = valueSeries.map((series) => {
    let min = Number.POSITIVE_INFINITY
    let max = Number.NEGATIVE_INFINITY
    series.forEach((value) => {
      if (value === null) return
      min = Math.min(min, value)
      max = Math.max(max, value)
    })
    return { min, range: max > min ? max - min : 1 }
  })
  const bucketSize = Math.max(1, Math.ceil((pointCount - 2) / width))
  const selected = new Set<number>([0, pointCount - 1])

  for (let start = 1; start < pointCount - 1; start += bucketSize) {
    const end = Math.min(pointCount - 1, start + bucketSize)
    let lowIndex = start
    let highIndex = start
    let lowScore = Number.POSITIVE_INFINITY
    let highScore = Number.NEGATIVE_INFINITY

    for (let index = start; index < end; index += 1) {
      valueSeries.forEach((series, seriesIndex) => {
        const value = series[index]
        if (value === null) return
        const range = ranges[seriesIndex]
        const score = (value - range.min) / range.range
        if (score < lowScore) {
          lowScore = score
          lowIndex = index
        }
        if (score > highScore) {
          highScore = score
          highIndex = index
        }
      })
    }
    selected.add(lowIndex)
    selected.add(highIndex)
  }

  const indices = [...selected].sort((left, right) => left - right)
  return data.map((series) => indices.map((index) => series[index]))
}

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
  private evictedPoints = 0
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

  get evictedCount(): number {
    return this.evictedPoints
  }

  append(sample: ApplicationSample): void {
    this.appendMany([sample])
  }

  appendMany(samples: readonly ApplicationSample[]): void {
    samples.forEach((sample) => this.appendOne(sample))
  }

  private appendOne(sample: ApplicationSample): void {
    const index = this.writeIndex
    if (this.pointCount === this.capacity) this.evictedPoints += 1
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
    this.evictedPoints = 0
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
    const includes = (index: number) => {
      const timestamp = this.timestamps[index]
      return timestamp >= cutoff &&
        (!options.channelId || this.channels[index] === options.channelId)
    }
    const appendPoint = (index: number) => {
      result[0].push(this.timestamps[index])
      fields.forEach((_field, fieldIndex) => {
        const value = fieldSeries[fieldIndex]?.[index]
        result[fieldIndex + 1]?.push(value === undefined || Number.isNaN(value) ? null : value)
      })
    }

    if (options.pixelWidth === undefined) {
      for (let offset = 0; offset < this.pointCount; offset += 1) {
        const index = (first + offset) % this.capacity
        if (includes(index)) appendPoint(index)
      }
      return result
    }

    const width = Number.isFinite(options.pixelWidth)
      ? Math.max(1, Math.floor(options.pixelWidth))
      : 1
    let matchingCount = 0
    let firstMatch = -1
    let lastMatch = -1

    for (let offset = 0; offset < this.pointCount; offset += 1) {
      const index = (first + offset) % this.capacity
      if (!includes(index)) continue
      if (firstMatch < 0) firstMatch = index
      lastMatch = index
      matchingCount += 1
    }

    if (matchingCount === 0) return result
    const maximumPointCount = 2 * Math.max(1, fields.length) * width + 2
    if (matchingCount <= maximumPointCount) {
      for (let offset = 0; offset < this.pointCount; offset += 1) {
        const index = (first + offset) % this.capacity
        if (includes(index)) appendPoint(index)
      }
      return result
    }

    const minimumValues = fields.map(() => Array<number>(width).fill(Number.POSITIVE_INFINITY))
    const maximumValues = fields.map(() => Array<number>(width).fill(Number.NEGATIVE_INFINITY))
    const minimumIndices = fields.map(() => Array<number>(width).fill(-1))
    const maximumIndices = fields.map(() => Array<number>(width).fill(-1))
    let ordinal = 0
    const interiorCount = matchingCount - 2
    for (let offset = 0; offset < this.pointCount; offset += 1) {
      const index = (first + offset) % this.capacity
      if (!includes(index)) continue
      if (ordinal > 0 && ordinal < matchingCount - 1) {
        const bucket = Math.min(width - 1, Math.floor((ordinal - 1) * width / interiorCount))
        fields.forEach((_field, fieldIndex) => {
          const value = fieldSeries[fieldIndex]?.[index]
          if (value === undefined || Number.isNaN(value)) return
          if (value < minimumValues[fieldIndex]![bucket]!) {
            minimumValues[fieldIndex]![bucket] = value
            minimumIndices[fieldIndex]![bucket] = index
          }
          if (value > maximumValues[fieldIndex]![bucket]!) {
            maximumValues[fieldIndex]![bucket] = value
            maximumIndices[fieldIndex]![bucket] = index
          }
        })
      }
      ordinal += 1
    }

    const selected = new Set<number>([firstMatch, lastMatch])
    minimumIndices.forEach((indices, fieldIndex) => {
      indices.forEach((index, bucket) => {
        if (index >= 0) selected.add(index)
        const maximumIndex = maximumIndices[fieldIndex]![bucket]!
        if (maximumIndex >= 0) selected.add(maximumIndex)
      })
    })
    for (let offset = 0; offset < this.pointCount; offset += 1) {
      const index = (first + offset) % this.capacity
      if (includes(index) && selected.has(index)) appendPoint(index)
    }

    return result
  }
}

export function downsampleAlignedMinMax(
  data: AlignedData,
  pixelWidth: number,
): AlignedData {
  const pointCount = data[0]?.length ?? 0
  const width = Number.isFinite(pixelWidth) ? Math.max(1, Math.floor(pixelWidth)) : 1
  if (pointCount <= width * 2 || data.length < 2) return data

  const valueSeries = data.slice(1)
  const bucketSize = Math.max(1, Math.ceil((pointCount - 2) / width))
  const selected = new Set<number>([0, pointCount - 1])

  for (let start = 1; start < pointCount - 1; start += bucketSize) {
    const end = Math.min(pointCount - 1, start + bucketSize)
    valueSeries.forEach((series) => {
      let minimum = Number.POSITIVE_INFINITY
      let maximum = Number.NEGATIVE_INFINITY
      let minimumIndex = -1
      let maximumIndex = -1
      for (let index = start; index < end; index += 1) {
        const value = series[index]
        if (value === null || value === undefined) continue
        if (value < minimum) {
          minimum = value
          minimumIndex = index
        }
        if (value > maximum) {
          maximum = value
          maximumIndex = index
        }
      }
      if (minimumIndex >= 0) selected.add(minimumIndex)
      if (maximumIndex >= 0) selected.add(maximumIndex)
    })
  }

  const indices = [...selected].sort((left, right) => left - right)
  return data.map((series) => indices.map((index) => series[index]))
}

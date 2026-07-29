import type { ApplicationSample } from '../../shared/protocol'
import { SampleBuffer } from './sample-buffer'

export type TelemetrySequenceStatus = 'continuous' | 'gap' | 'reconnect_incomplete'

export interface TelemetryBatch {
  firstSeq: number
  lastSeq: number
  samples: readonly ApplicationSample[]
}

export interface TelemetryAppendSummary {
  firstSeq: number
  lastSeq: number
  sampleCount: number
  channelIds: string[]
  cycleIndexRange: [number, number]
  sequenceIssue?: 'gap' | 'duplicate_or_out_of_order'
}

export interface TelemetryStoreSnapshot {
  version: number
  taskId: string
  latestSample: ApplicationSample | null
  fields: string[]
  receivedCount: number
  retainedCount: number
  evictedCount: number
  sequenceComplete: boolean
  sequenceStatus: TelemetrySequenceStatus
}

export interface TelemetryCommitScheduler {
  now(): number
  setTimeout(callback: () => void, delayMs: number): unknown
  clearTimeout(handle: unknown): void
  requestAnimationFrame(callback: () => void): unknown
  cancelAnimationFrame(handle: unknown): void
}

export interface TelemetryStoreOptions {
  commitIntervalMs?: number
  scheduler?: TelemetryCommitScheduler
  onCommit?: () => void
}

function browserScheduler(): TelemetryCommitScheduler {
  const scheduleTimeout = (callback: () => void, delayMs: number) => globalThis.setTimeout(callback, delayMs)
  const cancelTimeout = (handle: unknown) => globalThis.clearTimeout(handle as number)
  return {
    now: () => {
      const value = globalThis.performance?.now()
      return typeof value === 'number' && Number.isFinite(value) ? value : Date.now()
    },
    setTimeout: scheduleTimeout,
    clearTimeout: cancelTimeout,
    requestAnimationFrame: (callback) => {
      if (typeof globalThis.requestAnimationFrame === 'function') {
        return globalThis.requestAnimationFrame(() => callback())
      }
      return scheduleTimeout(callback, 0)
    },
    cancelAnimationFrame: (handle) => {
      if (typeof globalThis.cancelAnimationFrame === 'function') {
        globalThis.cancelAnimationFrame(handle as number)
        return
      }
      cancelTimeout(handle)
    },
  }
}

function validateBatch(batch: TelemetryBatch): void {
  if (!Number.isSafeInteger(batch.firstSeq) || batch.firstSeq < 0 ||
      !Number.isSafeInteger(batch.lastSeq) || batch.lastSeq < 0) {
    throw new Error('遥测批次序号必须是非负 JavaScript 安全整数')
  }
  if (batch.samples.length < 1 || batch.samples.length > 64) {
    throw new Error('遥测批次必须包含 1–64 条样本')
  }
  const expectedLast = batch.firstSeq + batch.samples.length - 1
  if (!Number.isSafeInteger(expectedLast) || batch.lastSeq !== expectedLast) {
    throw new Error('遥测批次首尾序号不连续')
  }
  const taskId = batch.samples[0]?.taskId
  if (!taskId || batch.samples.some((sample) => sample.taskId !== taskId)) {
    throw new Error('遥测批次不得跨任务')
  }
}

export class TelemetryStore {
  readonly buffer: SampleBuffer

  private readonly scheduler: TelemetryCommitScheduler
  private readonly commitIntervalMs: number
  private readonly onCommit?: () => void
  private readonly listeners = new Set<() => void>()
  private timeoutHandle: unknown | null = null
  private frameHandle: unknown | null = null
  private lastCommitAt = 0
  private version = 0
  private receivedCount = 0
  private taskId = ''
  private expectedSequence: number | null = null
  private sequenceExhausted = false
  private sequenceStatus: TelemetrySequenceStatus = 'continuous'
  private disposed = false
  private snapshot: TelemetryStoreSnapshot

  constructor(capacity: number, options: TelemetryStoreOptions = {}) {
    this.buffer = new SampleBuffer(capacity)
    this.scheduler = options.scheduler ?? browserScheduler()
    this.commitIntervalMs = options.commitIntervalMs ?? 100
    this.onCommit = options.onCommit
    this.snapshot = this.createSnapshot()
  }

  subscribe = (listener: () => void): (() => void) => {
    this.listeners.add(listener)
    return () => this.listeners.delete(listener)
  }

  getSnapshot = (): TelemetryStoreSnapshot => this.snapshot

  currentStats(): TelemetryStoreSnapshot {
    return this.createSnapshot()
  }

  appendMany(batch: TelemetryBatch): TelemetryAppendSummary {
    this.assertActive()
    validateBatch(batch)
    const taskId = batch.samples[0]!.taskId
    if (this.taskId && this.taskId !== taskId) this.resetForTask(taskId)
    if (!this.taskId) this.taskId = taskId

    let sequenceIssue: TelemetryAppendSummary['sequenceIssue']
    if (this.sequenceExhausted) {
      sequenceIssue = 'duplicate_or_out_of_order'
      if (this.sequenceStatus !== 'reconnect_incomplete') this.sequenceStatus = 'gap'
    } else if (this.expectedSequence !== null && batch.firstSeq !== this.expectedSequence) {
      sequenceIssue = batch.firstSeq > this.expectedSequence
        ? 'gap'
        : 'duplicate_or_out_of_order'
      if (this.sequenceStatus !== 'reconnect_incomplete') this.sequenceStatus = 'gap'
    }
    this.sequenceExhausted = batch.lastSeq === Number.MAX_SAFE_INTEGER
    this.expectedSequence = batch.lastSeq === Number.MAX_SAFE_INTEGER
      ? null
      : batch.lastSeq + 1
    this.buffer.appendMany(batch.samples)
    this.receivedCount += batch.samples.length
    this.scheduleCommit()

    const channelIds = [...new Set(batch.samples.map((sample) => sample.channelId))]
      .sort((left, right) => left.localeCompare(right))
    const cycleIndices = batch.samples.map((sample) => sample.cycleIndex)
    return {
      firstSeq: batch.firstSeq,
      lastSeq: batch.lastSeq,
      sampleCount: batch.samples.length,
      channelIds,
      cycleIndexRange: [Math.min(...cycleIndices), Math.max(...cycleIndices)],
      ...(sequenceIssue === undefined ? {} : { sequenceIssue }),
    }
  }

  clear(): void {
    if (this.disposed) return
    this.resetState('')
    this.publish()
  }

  markReconnected(): void {
    if (this.disposed) return
    this.cancelScheduledCommit()
    this.expectedSequence = null
    this.sequenceExhausted = false
    this.sequenceStatus = 'reconnect_incomplete'
    this.publish()
  }

  dispose(): void {
    if (this.disposed) return
    this.disposed = true
    this.cancelScheduledCommit()
    this.listeners.clear()
  }

  private resetForTask(taskId: string): void {
    this.resetState(taskId)
  }

  private resetState(taskId: string): void {
    this.cancelScheduledCommit()
    this.buffer.clear()
    this.receivedCount = 0
    this.taskId = taskId
    this.expectedSequence = null
    this.sequenceExhausted = false
    this.sequenceStatus = 'continuous'
  }

  private scheduleCommit(): void {
    if (this.timeoutHandle !== null || this.frameHandle !== null) return
    const elapsed = this.scheduler.now() - this.lastCommitAt
    const delay = Math.max(0, this.commitIntervalMs - elapsed)
    this.timeoutHandle = this.scheduler.setTimeout(() => {
      this.timeoutHandle = null
      this.frameHandle = this.scheduler.requestAnimationFrame(() => {
        this.frameHandle = null
        this.publish()
      })
    }, delay)
  }

  private cancelScheduledCommit(): void {
    if (this.timeoutHandle !== null) {
      this.scheduler.clearTimeout(this.timeoutHandle)
      this.timeoutHandle = null
    }
    if (this.frameHandle !== null) {
      this.scheduler.cancelAnimationFrame(this.frameHandle)
      this.frameHandle = null
    }
  }

  private publish(): void {
    if (this.disposed) return
    this.version += 1
    this.snapshot = this.createSnapshot()
    this.lastCommitAt = this.scheduler.now()
    this.onCommit?.()
    this.listeners.forEach((listener) => listener())
  }

  private createSnapshot(): TelemetryStoreSnapshot {
    return {
      version: this.version,
      taskId: this.taskId,
      latestSample: this.buffer.latest(),
      fields: this.buffer.fields(),
      receivedCount: this.receivedCount,
      retainedCount: this.buffer.size,
      evictedCount: this.buffer.evictedCount,
      sequenceComplete: this.sequenceStatus === 'continuous',
      sequenceStatus: this.sequenceStatus,
    }
  }

  private assertActive(): void {
    if (this.disposed) throw new Error('TelemetryStore 已释放')
  }
}

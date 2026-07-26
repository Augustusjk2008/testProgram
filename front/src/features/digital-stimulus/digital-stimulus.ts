import type {
  ApplicationSample,
  DigitalStimulusSnapshot,
  DigitalSwitchDescriptor,
} from '../../shared/protocol'

export type DigitalReadbackStatus = 'match' | 'mismatch' | 'settling' | 'unknown'

export interface DigitalStimulusTransport {
  set: (switchId: string, active: boolean, expectedRevision: number) => Promise<DigitalStimulusSnapshot>
  reset: () => Promise<DigitalStimulusSnapshot>
}

const COALESCE_MS = 32

function copySnapshot(state: DigitalStimulusSnapshot): DigitalStimulusSnapshot {
  return { ...state, switches: [...state.switches] }
}

export function appliedBit(mask: number, bit: number): boolean {
  if (!Number.isSafeInteger(mask) || !Number.isInteger(bit) || bit < 0 || bit > 52) return false
  return (BigInt(mask) & (1n << BigInt(bit))) !== 0n
}

function setMaskBit(mask: number, bit: number, active: boolean): number {
  const native = BigInt(Number.isSafeInteger(mask) ? mask : 0)
  const flag = 1n << BigInt(bit)
  return Number(active ? native | flag : native & ~flag)
}

export class DigitalStimulusCommandQueue {
  private confirmed: DigitalStimulusSnapshot
  private optimistic: DigitalStimulusSnapshot
  private pending = new Map<string, boolean>()
  private resetPending = false
  private timer: ReturnType<typeof setTimeout> | null = null
  private activeFlush: Promise<void> | null = null
  private disposed = false

  constructor(
    private readonly transport: DigitalStimulusTransport,
    initial: DigitalStimulusSnapshot,
    private readonly onState: (state: DigitalStimulusSnapshot) => void,
    private readonly onError: (message: string) => void,
  ) {
    this.confirmed = copySnapshot(initial)
    this.optimistic = copySnapshot(initial)
  }

  toggle(switchId: string, active: boolean): void {
    if (this.disposed) return
    const descriptor = this.confirmed.switches.find((item) => item.switchId === switchId)
    if (!descriptor) {
      this.onError(`未知数字开关：${switchId}`)
      return
    }
    this.pending.set(switchId, active)
    this.recomputeOptimistic()
    this.onState(copySnapshot(this.optimistic))
    this.schedule()
  }

  requestReset(): void {
    if (this.disposed) return
    this.resetPending = true
    this.pending.clear()
    this.recomputeOptimistic()
    this.onState(copySnapshot(this.optimistic))
    this.schedule()
  }

  sync(state: DigitalStimulusSnapshot): void {
    if (this.disposed) return
    this.adoptConfirmed(state)
    this.recomputeOptimistic()
    this.onState(copySnapshot(this.optimistic))
  }

  async flushNow(): Promise<void> {
    if (this.disposed) return
    if (this.timer !== null) {
      clearTimeout(this.timer)
      this.timer = null
    }
    if (this.activeFlush) return this.activeFlush
    this.activeFlush = this.drain().finally(() => {
      this.activeFlush = null
      if (!this.disposed && (this.resetPending || this.pending.size > 0)) this.schedule()
    })
    return this.activeFlush
  }

  state(): DigitalStimulusSnapshot {
    return copySnapshot(this.optimistic)
  }

  dispose(): void {
    this.disposed = true
    this.pending.clear()
    this.resetPending = false
    if (this.timer !== null) clearTimeout(this.timer)
    this.timer = null
  }

  private schedule(): void {
    if (this.timer !== null || this.activeFlush || this.disposed) return
    this.timer = setTimeout(() => {
      this.timer = null
      void this.flushNow()
    }, COALESCE_MS)
  }

  private recomputeOptimistic(): void {
    let mask = this.resetPending ? 0 : this.confirmed.appliedMask
    for (const [switchId, active] of this.pending) {
      const descriptor = this.confirmed.switches.find((item) => item.switchId === switchId)
      if (descriptor) mask = setMaskBit(mask, descriptor.dutBit, active)
    }
    this.optimistic = { ...this.confirmed, appliedMask: mask }
  }

  private async drain(): Promise<void> {
    while (!this.disposed && (this.resetPending || this.pending.size > 0)) {
      try {
        if (this.resetPending) {
          this.resetPending = false
          this.adoptConfirmed(await this.transport.reset())
        } else {
          const command = this.pending.entries().next().value as [string, boolean] | undefined
          if (!command) break
          this.pending.delete(command[0])
          this.adoptConfirmed(await this.transport.set(
            command[0],
            command[1],
            this.confirmed.revision,
          ))
        }
        this.recomputeOptimistic()
        this.onState(copySnapshot(this.optimistic))
      } catch (error) {
        this.pending.clear()
        this.resetPending = false
        this.optimistic = copySnapshot(this.confirmed)
        this.onState(copySnapshot(this.optimistic))
        this.onError(error instanceof Error ? error.message : String(error))
        return
      }
    }
  }

  private adoptConfirmed(state: DigitalStimulusSnapshot): void {
    if (state.revision < this.confirmed.revision) return
    this.confirmed = copySnapshot(state)
  }
}

function unsignedInteger(value: unknown): bigint | null {
  if (typeof value === 'number' && Number.isSafeInteger(value) &&
      value >= 0 && value <= 0xffff_ffff) {
    return BigInt(value)
  }
  if (typeof value === 'string' && /^(?:0x[0-9a-f]+|\d+)$/i.test(value.trim())) {
    try {
      const parsed = BigInt(value.trim())
      return parsed >= 0n && parsed <= 0xffff_ffffn ? parsed : null
    } catch {
      return null
    }
  }
  return null
}

export function readbackGroup(sample: ApplicationSample | null, group: number): number | null {
  if (!sample || !Number.isInteger(group) || group < 0 || group > 1) return null
  const value = unsignedInteger(sample.values[`di_state[${group}]`])
  if (value === null) return null
  return Number(value)
}

export function readbackBit(sample: ApplicationSample | null, dutBit: number): boolean | null {
  if (!Number.isInteger(dutBit) || dutBit < 0 || dutBit > 63) return null
  const group = Math.floor(dutBit / 32)
  const value = readbackGroup(sample, group)
  if (value === null) return null
  return (BigInt(value) & (1n << BigInt(dutBit % 32))) !== 0n
}

export function physicalLevel(
  descriptor: DigitalSwitchDescriptor,
  active: boolean,
): 'High' | 'Low' {
  if (active) return descriptor.activeLevel
  return descriptor.activeLevel === 'High' ? 'Low' : 'High'
}

export function readbackStatus(
  snapshot: DigitalStimulusSnapshot,
  descriptor: DigitalSwitchDescriptor,
  sample: ApplicationSample | null,
  nowUs: number,
): DigitalReadbackStatus {
  const readback = readbackBit(sample, descriptor.dutBit)
  if (readback === null) return 'unknown'
  if (snapshot.lastWriteTimestampUs > 0 && sample !== null &&
      sample.timestampUs <= snapshot.lastWriteTimestampUs) {
    return 'settling'
  }
  if (snapshot.lastWriteTimestampUs > 0 &&
      nowUs - snapshot.lastWriteTimestampUs < snapshot.settlingMs * 1000) {
    return 'settling'
  }
  const expectedHigh = physicalLevel(
    descriptor,
    appliedBit(snapshot.appliedMask, descriptor.dutBit),
  ) === 'High'
  return readback === expectedHigh
    ? 'match'
    : 'mismatch'
}

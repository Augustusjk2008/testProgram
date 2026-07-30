import type {
  TestDescriptor,
  TestRunParameterDescriptor,
} from '../../shared/protocol'

export type RunParameterValues = Record<string, unknown>

const DO_WRITE_ALGORITHM_ID = 'mbddf.do_write'
const DH_IGNITE_ALGORITHM_ID = 'mbddf.dh_ignite_stream'
const GUARDED_DO_CHANNELS = new Set([5, 6])

function doChannelIndex(parameterId: string): number | null {
  const match = /^channel_enabled\[(\d+)\]$/.exec(parameterId)
  if (!match) return null
  const index = Number(match[1])
  return Number.isSafeInteger(index) ? index : null
}

/** Keeps checkbox-only and display-disabled values canonical after browser restore. */
export function normalizeGuardedRunParameterValues(
  descriptor: TestDescriptor,
  values: RunParameterValues,
): RunParameterValues {
  const normalized = { ...values }
  if (descriptor.algorithmId === DH_IGNITE_ALGORITHM_ID) {
    for (const id of ['power_enable', 'return_enable']) {
      normalized[id] = normalized[id] === 1 || normalized[id] === true ? 1 : 0
    }
  }
  if (descriptor.algorithmId !== DO_WRITE_ALGORITHM_ID) return normalized
  for (const parameter of descriptor.runParameters) {
    const channel = doChannelIndex(parameter.id)
    if (channel === null || !GUARDED_DO_CHANNELS.has(channel)) continue
    normalized[parameter.id] = parameter.kind === 'integer' ? 0 : false
  }
  return normalized
}

function isPersistableValue(value: unknown): value is string | number | boolean {
  return typeof value === 'string' || typeof value === 'boolean' ||
    (typeof value === 'number' && Number.isFinite(value))
}

export function runParameterStorageKey(descriptor: TestDescriptor): string {
  return `hwtest.run-parameters.v1:${encodeURIComponent(descriptor.configId)}:${encodeURIComponent(descriptor.runParameterSchemaVersion)}`
}

export function loadRunParameterValues(
  descriptor: TestDescriptor,
  storedJson: string | null,
): RunParameterValues {
  const values: RunParameterValues = { ...descriptor.runParameterDefaults }
  if (storedJson) {
    try {
      const parsed: unknown = JSON.parse(storedJson)
      if (typeof parsed === 'object' && parsed !== null && !Array.isArray(parsed)) {
        const known = new Set(descriptor.runParameters
          .filter((parameter) => parameter.persistValues !== false)
          .map((parameter) => parameter.id))
        for (const [key, value] of Object.entries(parsed)) {
          if (known.has(key) && isPersistableValue(value)) values[key] = value
        }
      }
    } catch {
      // Invalid or unavailable browser state falls back to configuration defaults.
    }
  }
  return normalizeGuardedRunParameterValues(descriptor, values)
}

/** Returns the browser-persistable subset; absent persistValues remains compatible as true. */
export function persistableRunParameterValues(
  descriptor: TestDescriptor,
  values: RunParameterValues,
): RunParameterValues {
  const persisted: RunParameterValues = {}
  const normalized = normalizeGuardedRunParameterValues(descriptor, values)
  for (const parameter of descriptor.runParameters) {
    if (parameter.persistValues === false) continue
    const value = normalized[parameter.id]
    if (isPersistableValue(value)) persisted[parameter.id] = value
  }
  return persisted
}

export function visibleRunParameters(
  descriptor: TestDescriptor,
  values: RunParameterValues,
): TestRunParameterDescriptor[] {
  return descriptor.runParameters.filter((parameter) => (
    parameter.visibleWhen === undefined ||
    values[parameter.visibleWhen.parameter] === parameter.visibleWhen.equals
  ))
}

function rangeError(
  parameter: TestRunParameterDescriptor,
  numericValue: number,
): string {
  if (parameter.minimum !== undefined) {
    const tooSmall = parameter.minimumExclusive
      ? numericValue <= parameter.minimum
      : numericValue < parameter.minimum
    if (tooSmall) return `${parameter.label}低于允许范围`
  }
  if (parameter.maximum !== undefined) {
    const tooLarge = parameter.maximumExclusive
      ? numericValue >= parameter.maximum
      : numericValue > parameter.maximum
    if (tooLarge) return `${parameter.label}超出允许范围`
  }
  return ''
}

export function validateRunParameterValues(
  descriptor: TestDescriptor,
  values: RunParameterValues,
): string {
  for (const parameter of visibleRunParameters(descriptor, values)) {
    const value = values[parameter.id]
    if (value === undefined || value === null || value === '') {
      if (parameter.required) return `${parameter.label}为必填项`
      continue
    }
    if (parameter.kind === 'boolean') {
      if (typeof value !== 'boolean') return `${parameter.label}必须为布尔值`
      continue
    }
    if (parameter.kind === 'choice') {
      if (!parameter.choices.some((choice) => choice.value === value)) {
        return `${parameter.label}不是可选值`
      }
      continue
    }
    if (typeof value !== 'number' || !Number.isFinite(value)) {
      return `${parameter.label}必须为有限数值`
    }
    if (parameter.kind === 'integer' && !Number.isInteger(value)) {
      return `${parameter.label}必须为整数`
    }
    const error = rangeError(parameter, value)
    if (error) return error
  }
  return ''
}

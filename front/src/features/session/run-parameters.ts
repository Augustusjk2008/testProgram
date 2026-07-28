import type {
  TestDescriptor,
  TestRunParameterDescriptor,
} from '../../shared/protocol'

export type RunParameterValues = Record<string, unknown>

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
  if (!storedJson) return values
  try {
    const parsed: unknown = JSON.parse(storedJson)
    if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) return values
    const known = new Set(descriptor.runParameters.map((parameter) => parameter.id))
    for (const [key, value] of Object.entries(parsed)) {
      if (known.has(key) && isPersistableValue(value)) values[key] = value
    }
  } catch {
    // Invalid or unavailable browser state falls back to configuration defaults.
  }
  return values
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
      // if (parameter.required) return `${parameter.label}为必填项`
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

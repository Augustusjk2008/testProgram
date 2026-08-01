import type { ConfigValue } from './config-draft'
import { configValueAtPath } from './config-form-model'
import type { ConfigFormField, ConfigFormOption, ConfigFormSchema } from './config-form-schema'

export interface ConfigFormValidationError {
  path: string
  message: string
}

function optionsFor(
  field: ConfigFormField,
  optionSources: Record<string, ConfigFormOption[]>,
): ConfigFormOption[] {
  return [...field.options, ...(optionSources[field.optionsSource] ?? [])]
}

function rangeLabel(value: number, unit: string): string {
  return unit ? `${value} ${unit}` : String(value)
}

function fieldErrors(
  field: ConfigFormField,
  path: string,
  current: unknown,
  optionSources: Record<string, ConfigFormOption[]>,
): ConfigFormValidationError[] {
  if (field.readOnly) return []
  const result: ConfigFormValidationError[] = []
  const missing = current === undefined || current === null || current === ''
  if (missing) {
    return field.required ? [{ path, message: `${field.label}不能为空` }] : []
  }

  if (field.kind === 'text' || field.kind === 'multiline' || field.kind === 'choiceOrText') {
    if (typeof current !== 'string') result.push({ path, message: `${field.label}必须是文本` })
    return result
  }
  if (field.kind === 'scalar') {
    if (typeof current !== 'string' && typeof current !== 'boolean' &&
        (typeof current !== 'number' || !Number.isFinite(current))) {
      result.push({ path, message: `${field.label}必须是文本、数字或开关值` })
    }
    return result
  }
  if (field.kind === 'boolean') {
    if (typeof current !== 'boolean') result.push({ path, message: `${field.label}必须是开关值` })
    return result
  }
  if (field.kind === 'choice') {
    const options = optionsFor(field, optionSources)
    const legacyIndex = field.acceptOptionIndex && typeof current === 'number' &&
      Number.isSafeInteger(current) && current >= 0 && current < options.length
    if (!legacyIndex && !options.some((option) => Object.is(option.value, current))) {
      result.push({ path, message: `${field.label}不是允许的选项` })
    }
    return result
  }
  if (typeof current !== 'number' || !Number.isFinite(current)) {
    return [{ path, message: `${field.label}必须是数字` }]
  }
  if (field.kind === 'integer' && !Number.isSafeInteger(current)) {
    result.push({ path, message: `${field.label}必须是整数` })
  }
  if (field.minimum !== undefined && current < field.minimum) {
    result.push({
      path,
      message: `${field.label}不能小于 ${rangeLabel(field.minimum, field.unit)}`,
    })
  }
  if (field.maximum !== undefined && current > field.maximum) {
    result.push({
      path,
      message: `${field.label}不能大于 ${rangeLabel(field.maximum, field.unit)}`,
    })
  }
  return result
}

export function validateConfigForm(
  schema: ConfigFormSchema,
  value: ConfigValue,
  optionSources: Record<string, ConfigFormOption[]> = {},
): ConfigFormValidationError[] {
  const result: ConfigFormValidationError[] = []
  const defaults = new Map<string, ConfigFormField['defaultValue']>()
  schema.sections.forEach((section) => section.fields.forEach((field) => {
    if (field.defaultValue !== undefined) defaults.set(field.path, field.defaultValue)
  }))
  const effectiveValueAtPath = (path: string, defaultValue?: ConfigFormField['defaultValue']) => {
    const current = configValueAtPath(value, path)
    return current === undefined ? defaultValue : current
  }
  for (const section of schema.sections) {
    for (const field of section.fields) {
      if (field.visibleWhen && !Object.is(
        effectiveValueAtPath(field.visibleWhen.path, defaults.get(field.visibleWhen.path)),
        field.visibleWhen.equals,
      )) continue
      result.push(...fieldErrors(
        field,
        field.path,
        effectiveValueAtPath(field.path, field.defaultValue),
        optionSources,
      ))
    }
    for (const list of section.lists) {
      const items = configValueAtPath(value, list.path)
      if (!Array.isArray(items)) continue
      items.forEach((_, index) => {
        list.columns.forEach((field) => {
          const path = `${list.path}/${index}${field.path}`
          result.push(...fieldErrors(
            field,
            path,
            effectiveValueAtPath(path, field.defaultValue),
            optionSources,
          ))
        })
      })
    }
  }
  return result
}

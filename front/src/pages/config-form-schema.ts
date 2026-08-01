export interface ConfigFormSchema {
  contractVersion: 1
  mode: 'form'
  readOnlyPaths: string[]
  sections: ConfigFormSection[]
}

export interface ConfigFormSection {
  id: string
  title: string
  description: string
  fields: ConfigFormField[]
  lists: ConfigFormList[]
}

export type ConfigFormScalar = string | number | boolean

export interface ConfigFormOption {
  value: ConfigFormScalar
  label: string
}

export type ConfigFormFieldKind =
  | 'text'
  | 'multiline'
  | 'integer'
  | 'number'
  | 'boolean'
  | 'choice'
  | 'choiceOrText'
  | 'scalar'

export interface ConfigFormField {
  path: string
  label: string
  description: string
  kind: ConfigFormFieldKind
  defaultValue?: ConfigFormScalar
  unit: string
  required: boolean
  minimum?: number
  maximum?: number
  options: ConfigFormOption[]
  optionsSource: string
  allowManualEntry: boolean
  acceptOptionIndex?: boolean
  visibleWhen?: { path: string; equals: ConfigFormScalar }
  wide: boolean
  readOnly: boolean
}

export interface ConfigFormList {
  path: string
  label: string
  description: string
  addLabel: string
  allowAdd: boolean
  allowRemove: boolean
  itemDefaults: Record<string, unknown>
  columns: ConfigFormField[]
}

const FIELD_KINDS = new Set<ConfigFormFieldKind>([
  'text', 'multiline', 'integer', 'number', 'boolean', 'choice', 'choiceOrText', 'scalar',
])

function record(value: unknown): Record<string, unknown> | null {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null
}

function text(value: unknown, fallback = ''): string | null {
  return value === undefined ? fallback : typeof value === 'string' ? value : null
}

function flag(value: unknown, fallback = false): boolean | null {
  return value === undefined ? fallback : typeof value === 'boolean' ? value : null
}

function finiteNumber(value: unknown): number | undefined | null {
  if (value === undefined) return undefined
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

function scalar(value: unknown): ConfigFormScalar | null {
  return typeof value === 'string' || typeof value === 'boolean' ||
    (typeof value === 'number' && Number.isFinite(value)) ? value : null
}

function parseOptions(value: unknown): ConfigFormOption[] | null {
  if (value === undefined) return []
  if (!Array.isArray(value)) return null
  const options: ConfigFormOption[] = []
  for (const item of value) {
    const source = record(item)
    const optionValue = source ? scalar(source.value) : null
    const label = source ? text(source.label) : null
    if (!source || optionValue === null || label === null || !label) return null
    options.push({ value: optionValue, label })
  }
  return options
}

function parseField(value: unknown, relativePath = false): ConfigFormField | null {
  const source = record(value)
  if (!source) return null
  const sourcePath = text(source.path)
  const path = sourcePath && relativePath && !sourcePath.startsWith('/')
    ? `/${sourcePath}`
    : sourcePath
  const label = text(source.label)
  const description = text(source.description)
  const kind = text(source.kind) as ConfigFormFieldKind | null
  const defaultValue = source.defaultValue === undefined
    ? undefined
    : scalar(source.defaultValue)
  const unit = text(source.unit)
  const required = flag(source.required)
  const minimum = finiteNumber(source.minimum)
  const maximum = finiteNumber(source.maximum)
  const options = parseOptions(source.options)
  const optionsSource = text(source.optionsSource)
  const allowManualEntry = flag(source.allowManualEntry)
  const acceptOptionIndex = flag(source.acceptOptionIndex)
  const wide = flag(source.wide)
  const readOnly = flag(source.readOnly)
  if (!path?.startsWith('/') || !label || !kind || !FIELD_KINDS.has(kind) ||
      description === null || defaultValue === null || unit === null || required === null || minimum === null ||
      maximum === null || options === null || optionsSource === null ||
      allowManualEntry === null || acceptOptionIndex === null || wide === null || readOnly === null) {
    return null
  }

  let visibleWhen: ConfigFormField['visibleWhen']
  if (source.visibleWhen !== undefined) {
    const visibility = record(source.visibleWhen)
    const visibilityPath = visibility ? text(visibility.path) : null
    const equals = visibility ? scalar(visibility.equals) : null
    if (!visibilityPath?.startsWith('/') || equals === null) return null
    visibleWhen = { path: visibilityPath, equals }
  }

  return {
    path,
    label,
    description,
    kind,
    ...(defaultValue === undefined ? {} : { defaultValue }),
    unit,
    required,
    ...(minimum === undefined ? {} : { minimum }),
    ...(maximum === undefined ? {} : { maximum }),
    options,
    optionsSource,
    allowManualEntry,
    acceptOptionIndex,
    ...(visibleWhen ? { visibleWhen } : {}),
    wide,
    readOnly,
  }
}

function parseList(value: unknown): ConfigFormList | null {
  const source = record(value)
  if (!source) return null
  const path = text(source.path)
  const label = text(source.label)
  const description = text(source.description)
  const addLabel = text(source.addLabel, '增加一项')
  const allowAdd = flag(source.allowAdd)
  const allowRemove = flag(source.allowRemove)
  const itemDefaults = source.itemDefaults === undefined ? {} : record(source.itemDefaults)
  const columnsSource = source.columns === undefined ? [] : source.columns
  if (!path?.startsWith('/') || !label || description === null || addLabel === null ||
      allowAdd === null || allowRemove === null || !itemDefaults || !Array.isArray(columnsSource)) {
    return null
  }
  const columns = columnsSource.map((column) => parseField(column, true))
  if (columns.some((column) => column === null)) return null
  return {
    path,
    label,
    description,
    addLabel,
    allowAdd,
    allowRemove,
    itemDefaults,
    columns: columns as ConfigFormField[],
  }
}

function stringList(value: unknown): string[] | null {
  if (value === undefined) return []
  if (!Array.isArray(value) || value.some((item) => typeof item !== 'string')) return null
  return value as string[]
}

export function parseConfigFormSchema(value: unknown): ConfigFormSchema | null {
  const source = record(value)
  if (!source || source.contractVersion !== 1 || source.mode !== 'form' ||
      !Array.isArray(source.sections)) return null
  const readOnlyPaths = stringList(source.readOnlyPaths)
  if (!readOnlyPaths) return null

  const sections: ConfigFormSection[] = []
  for (const item of source.sections) {
    const section = record(item)
    if (!section) return null
    const id = text(section.id)
    const title = text(section.title)
    const description = text(section.description)
    const fieldsSource = section.fields === undefined ? [] : section.fields
    const listsSource = section.lists === undefined ? [] : section.lists
    if (!id || !title || description === null || !Array.isArray(fieldsSource) ||
        !Array.isArray(listsSource)) return null
    const fields = fieldsSource.map((field) => parseField(field))
    const lists = listsSource.map(parseList)
    if (fields.some((field) => field === null) || lists.some((list) => list === null)) return null
    sections.push({
      id,
      title,
      description,
      fields: fields as ConfigFormField[],
      lists: lists as ConfigFormList[],
    })
  }
  return { contractVersion: 1, mode: 'form', readOnlyPaths, sections }
}

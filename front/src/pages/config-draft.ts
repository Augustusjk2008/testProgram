import type { ConfigCatalogItem } from '../shared/protocol'

export type ConfigValue = Record<string, unknown>
export type EditableCatalogItem = ConfigCatalogItem & Record<string, unknown>

export interface ConfigurationWorkspaceNavigationState {
  dirty: boolean
  saving: boolean
}

export function shouldPreserveConfigDraftOnReconnect(
  wasDisconnected: boolean,
  dirty: boolean,
): boolean {
  return wasDisconnected && dirty
}

export function canLeaveConfigurationWorkspace(
  state: ConfigurationWorkspaceNavigationState,
  confirmDiscard: () => boolean,
): boolean {
  if (state.saving) return false
  return !state.dirty || confirmDiscard()
}

export type TestConfigField =
  | 'title'
  | 'description'
  | 'stepId'
  | 'testItemId'
  | 'parameters'
  | 'step'
  | 'executionConfig'

export interface TestConfigFormFields {
  title: string
  description: string
  stepId: string
  testItemId: string
  parameters: ConfigValue
  step: ConfigValue
  executionConfig: ConfigValue
}

export function isConfigDocumentNavigationBlocked(
  _dirty: boolean,
  loading: boolean,
  saving: boolean,
): boolean {
  return loading || saving
}

function asRecord(value: unknown): ConfigValue | null {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
    ? value as ConfigValue
    : null
}

function stringValue(value: unknown): string {
  return typeof value === 'string' ? value : ''
}

function persistedCatalogEntry(value: unknown): {
  documentId: string
  enabled: boolean
  order: number
} | null {
  const entry = asRecord(value)
  if (!entry || typeof entry.documentId !== 'string' || !entry.documentId ||
      typeof entry.enabled !== 'boolean' || typeof entry.order !== 'number' ||
      !Number.isSafeInteger(entry.order)) {
    return null
  }
  return {
    documentId: entry.documentId,
    enabled: entry.enabled,
    order: entry.order,
  }
}

export function catalogEditorItems(
  value: ConfigValue,
  fallback: readonly ConfigCatalogItem[],
): EditableCatalogItem[] {
  const persisted = new Map(
    (Array.isArray(value.entries) ? value.entries : [])
      .map(persistedCatalogEntry)
      .filter((entry): entry is NonNullable<typeof entry> => entry !== null)
      .map((entry) => [entry.documentId, entry]),
  )
  return fallback
    .map((item) => {
      const entry = persisted.get(item.documentId)
      return entry ? { ...item, enabled: entry.enabled, order: entry.order } : { ...item }
    })
    .sort((left, right) => left.order - right.order || left.documentId.localeCompare(right.documentId))
}

export function updateCatalogDocument(
  value: ConfigValue,
  items: readonly EditableCatalogItem[],
): ConfigValue {
  const next = { ...value }
  delete next.items
  next.entries = items.map(({ documentId, enabled, order }) => ({
    documentId,
    enabled,
    order,
  }))
  return next
}

export function testConfigFormFields(value: ConfigValue): TestConfigFormFields {
  const reportFields = asRecord(value.reportFields) ?? {}
  const steps = Array.isArray(value.steps) ? value.steps : []
  const step = asRecord(steps[0]) ?? {}
  return {
    title: stringValue(reportFields.title),
    description: stringValue(reportFields.description),
    stepId: stringValue(step.stepId),
    testItemId: stringValue(step.testItemId),
    parameters: asRecord(step.parameters) ?? {},
    step,
    executionConfig: asRecord(value.executionConfig) ?? {},
  }
}

export function updateTestConfigDocument(
  value: ConfigValue,
  field: TestConfigField,
  nextValue: unknown,
): ConfigValue {
  if (field === 'title' || field === 'description') {
    const reportFields = asRecord(value.reportFields) ?? {}
    return { ...value, reportFields: { ...reportFields, [field]: nextValue } }
  }
  if (field === 'executionConfig') {
    return { ...value, executionConfig: nextValue }
  }

  const steps = Array.isArray(value.steps) ? [...value.steps] : []
  const currentStep = asRecord(steps[0]) ?? {}
  let nextStep: ConfigValue
  if (field === 'step') {
    nextStep = { ...(asRecord(nextValue) ?? {}) }
    if (Object.prototype.hasOwnProperty.call(currentStep, 'algorithmId')) {
      nextStep.algorithmId = currentStep.algorithmId
    }
  } else if (field === 'parameters') {
    nextStep = { ...currentStep, parameters: nextValue }
  } else {
    nextStep = { ...currentStep, [field]: nextValue }
  }
  steps[0] = nextStep
  return { ...value, steps }
}

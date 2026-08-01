import { describe, expect, it } from 'vitest'

import type { ConfigCatalogItem } from '../shared/protocol'
import {
  canLeaveConfigurationWorkspace,
  catalogEditorItems,
  isConfigDocumentNavigationBlocked,
  shouldPreserveConfigDraftOnReconnect,
  testConfigFormFields,
  updateCatalogDocument,
  updateTestConfigDocument,
} from './config-draft'

describe('configuration document draft mapping', () => {
  it('preserves dirty drafts across reconnects and confirms before leaving the workspace', () => {
    expect(shouldPreserveConfigDraftOnReconnect(true, true)).toBe(true)
    expect(shouldPreserveConfigDraftOnReconnect(true, false)).toBe(false)
    expect(canLeaveConfigurationWorkspace({ dirty: false, saving: false }, () => false)).toBe(true)
    expect(canLeaveConfigurationWorkspace({ dirty: true, saving: false }, () => true)).toBe(true)
    expect(canLeaveConfigurationWorkspace({ dirty: true, saving: false }, () => false)).toBe(false)
    expect(canLeaveConfigurationWorkspace({ dirty: false, saving: true }, () => true)).toBe(false)
  })

  it('only hard-blocks document switching while a load or save is in flight', () => {
    expect(isConfigDocumentNavigationBlocked(false, false, false)).toBe(false)
    expect(isConfigDocumentNavigationBlocked(true, false, false)).toBe(false)
    expect(isConfigDocumentNavigationBlocked(false, true, false)).toBe(true)
    expect(isConfigDocumentNavigationBlocked(false, false, true)).toBe(true)
  })

  it('edits catalog entries while retaining display metadata outside the file', () => {
    const fallback: ConfigCatalogItem[] = [{
      documentId: 'sample.testcfg.json',
      configId: 'sample',
      title: '样例测试',
      enabled: true,
      order: 10,
      valid: true,
      message: '',
    }]
    const value = {
      schemaVersion: '1',
      entries: [{ documentId: 'sample.testcfg.json', enabled: false, order: 3 }],
    }

    const items = catalogEditorItems(value, fallback)
    expect(items).toEqual([{ ...fallback[0], enabled: false, order: 3 }])

    const saved = updateCatalogDocument(value, [{ ...items[0]!, enabled: true, order: 0 }])
    expect(saved).toEqual({
      schemaVersion: '1',
      entries: [{ documentId: 'sample.testcfg.json', enabled: true, order: 0 }],
    })
    expect(saved).not.toHaveProperty('items')
  })

  it('projects and edits the real nested testcfg shape without changing algorithm identity', () => {
    const value = {
      schemaVersion: '1.0',
      configId: 'sample',
      reportFields: { title: '原标题', description: '原说明' },
      steps: [{
        stepId: 'STEP_1',
        testItemId: 'item_1',
        algorithmId: 'mbddf.system_status',
        parameters: { protocol: { requestValues: {} } },
      }],
      executionConfig: { transport: { openTimeoutMs: 1000 } },
    }

    expect(testConfigFormFields(value)).toMatchObject({
      title: '原标题',
      description: '原说明',
      stepId: 'STEP_1',
      testItemId: 'item_1',
      parameters: { protocol: { requestValues: {} } },
    })

    const titled = updateTestConfigDocument(value, 'title', '新标题')
    const updated = updateTestConfigDocument(titled, 'stepId', 'STEP_NEW')
    expect(updated.reportFields).toEqual({ title: '新标题', description: '原说明' })
    expect(updated.steps).toEqual([{ ...value.steps[0], stepId: 'STEP_NEW' }])
    expect((updated.steps as Array<Record<string, unknown>>)[0]?.algorithmId)
      .toBe('mbddf.system_status')
    expect(updated).not.toHaveProperty('title')
    expect(updated).not.toHaveProperty('stepId')
  })
})

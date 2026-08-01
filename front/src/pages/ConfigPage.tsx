import { ArrowClockwise, ArrowCounterClockwise, ArrowDown, ArrowUp, FloppyDisk } from '@phosphor-icons/react'
import { useEffect, useMemo, useRef, useState } from 'react'

import { useSession } from '../features/session/SessionProvider'
import type { ConfigCatalogItem, ConfigDocument } from '../shared/protocol'
import { ConfigRequestError } from '../shared/ws/HwtestClient'
import {
  catalogEditorItems,
  isConfigDocumentNavigationBlocked,
  shouldPreserveConfigDraftOnReconnect,
  updateCatalogDocument,
  type ConfigValue,
  type ConfigurationWorkspaceNavigationState,
  type EditableCatalogItem,
} from './config-draft'
import { ConfigForm } from './ConfigForm'
import { parseConfigFormSchema } from './config-form-schema'
import { validateConfigForm } from './config-form-validation'

const FIXED_DOCUMENTS = [
  { documentId: 'test-config-catalog', title: '测试项目管理', kind: 'catalog' },
  { documentId: 'mbddf-station', title: '工位硬件', kind: 'station' },
] as const

function cloneValue(value: ConfigValue): ConfigValue {
  return JSON.parse(JSON.stringify(value)) as ConfigValue
}

function isWriteBlocked(phase: string): boolean {
  return phase !== 'empty' && phase !== 'configured'
}

function CatalogEditor({
  value,
  fallbackItems,
  disabled,
  onChange,
}: {
  value: ConfigValue
  fallbackItems: readonly ConfigCatalogItem[]
  disabled: boolean
  onChange: (value: ConfigValue) => void
}) {
  const items = catalogEditorItems(value, fallbackItems)
  const updateItems = (nextItems: EditableCatalogItem[]) => {
    onChange(updateCatalogDocument(value, nextItems))
  }
  const updateItem = (documentId: string, patch: Partial<EditableCatalogItem>) => {
    updateItems(items.map((item) => item.documentId === documentId ? { ...item, ...patch } : item))
  }
  const move = (documentId: string, offset: -1 | 1) => {
    const index = items.findIndex((item) => item.documentId === documentId)
    const nextIndex = index + offset
    if (index < 0 || nextIndex < 0 || nextIndex >= items.length) return
    const next = [...items]
    ;[next[index], next[nextIndex]] = [next[nextIndex]!, next[index]!]
    updateItems(next.map((item, order) => ({ ...item, order })))
  }

  return (
    <div className="config-catalog-editor">
      <p className="config-editor__hint">保存后，停用的测试项目将不会出现在测试工作台中。</p>
      <div className="config-catalog-editor__items">
        {items.map((item, index) => (
          <article className="config-catalog-editor__item" key={item.documentId}>
            <label>
              <input
                aria-label={`${item.title || item.documentId} 启用`}
                checked={item.enabled}
                disabled={disabled}
                onChange={(event) => updateItem(item.documentId, { enabled: event.target.checked })}
                type="checkbox"
              />
              <span>{item.enabled ? '启用' : '停用'}</span>
            </label>
            <div>
              <strong>{item.title || item.documentId}</strong>
              {!item.valid && <small>{item.message || '配置无效'}</small>}
            </div>
            <span className="config-catalog-editor__order">#{index + 1}</span>
            <div className="config-catalog-editor__move">
              <button aria-label={`${item.title || item.documentId} 上移`} disabled={disabled || index === 0} onClick={() => move(item.documentId, -1)} title="上移" type="button"><ArrowUp aria-hidden="true" size={14} /></button>
              <button aria-label={`${item.title || item.documentId} 下移`} disabled={disabled || index === items.length - 1} onClick={() => move(item.documentId, 1)} title="下移" type="button"><ArrowDown aria-hidden="true" size={14} /></button>
            </div>
          </article>
        ))}
      </div>
    </div>
  )
}

export function ConfigPage({
  onNavigationStateChange,
}: {
  onNavigationStateChange?: (state: ConfigurationWorkspaceNavigationState) => void
} = {}) {
  const {
    configCatalog,
    configCatalogError,
    configCatalogReady,
    connectionState,
    getConfigDocument,
    hardwareOptions,
    refreshConfigCatalog,
    saveConfig,
    serialPorts,
    snapshot,
  } = useSession()
  const [selectedDocumentId, setSelectedDocumentId] = useState('test-config-catalog')
  const [document, setDocument] = useState<ConfigDocument | null>(null)
  const [draft, setDraft] = useState<ConfigValue | null>(null)
  const [loading, setLoading] = useState(false)
  const [saving, setSaving] = useState(false)
  const [dirty, setDirty] = useState(false)
  const [loadError, setLoadError] = useState('')
  const [saveError, setSaveError] = useState<{ code: string; message: string } | null>(null)
  const [reloadGeneration, setReloadGeneration] = useState(0)
  const dirtyRef = useRef(false)
  const wasDisconnected = useRef(false)

  const catalogItems = useMemo(() => [...(configCatalog?.items ?? [])]
    .sort((left, right) => left.order - right.order || left.documentId.localeCompare(right.documentId)), [configCatalog])
  const documents = useMemo(() => [
    ...FIXED_DOCUMENTS,
    ...catalogItems
      .filter((item) => !FIXED_DOCUMENTS.some((fixed) => fixed.documentId === item.documentId))
      .map((item) => ({ documentId: item.documentId, title: item.title || item.documentId, kind: 'testcfg' })),
  ], [catalogItems])
  const writeBlocked = isWriteBlocked(snapshot.phase)
  const selectedDocumentTitle = documents.find((entry) =>
    entry.documentId === selectedDocumentId)?.title ?? '配置项'
  const navigationBlocked = isConfigDocumentNavigationBlocked(dirty, loading, saving)
  const formSchema = useMemo(() => parseConfigFormSchema(document?.schema), [document?.schema])
  const optionSources = useMemo(() => ({
    serialPorts: (serialPorts ?? []).map((port) => ({
      value: port.portName,
      label: [port.portName, port.description, port.manufacturer].filter(Boolean).join(' · '),
    })),
    niDevices: (hardwareOptions?.devices ?? []).map((device) => ({
      value: device.deviceName,
      label: [device.deviceName, device.model, device.serialNumber].filter(Boolean).join(' · '),
    })),
    ni6259Devices: (hardwareOptions?.devices ?? [])
      .filter((device) => device.model.toUpperCase().includes('6259'))
      .map((device) => ({
        value: device.deviceName,
        label: [device.deviceName, device.model, device.serialNumber].filter(Boolean).join(' · '),
      })),
    ni6733Devices: (hardwareOptions?.devices ?? [])
      .filter((device) => device.model.toUpperCase().includes('6733'))
      .map((device) => ({
        value: device.deviceName,
        label: [device.deviceName, device.model, device.serialNumber].filter(Boolean).join(' · '),
      })),
    ni6259SerialNumbers: (hardwareOptions?.devices ?? [])
      .filter((device) => device.model.toUpperCase().includes('6259') && device.serialNumber)
      .map((device) => ({ value: device.serialNumber, label: `${device.serialNumber} · ${device.deviceName}` })),
    ni6733SerialNumbers: (hardwareOptions?.devices ?? [])
      .filter((device) => device.model.toUpperCase().includes('6733') && device.serialNumber)
      .map((device) => ({ value: device.serialNumber, label: `${device.serialNumber} · ${device.deviceName}` })),
  }), [hardwareOptions, serialPorts])
  const formErrors = useMemo(() => formSchema && draft
    ? validateConfigForm(formSchema, draft, optionSources)
    : [], [draft, formSchema, optionSources])
  const saveDisabled = connectionState !== 'connected' || !document || !draft || !dirty ||
    loading || saving || writeBlocked || formErrors.length > 0

  useEffect(() => {
    dirtyRef.current = dirty
    onNavigationStateChange?.({ dirty, saving })
  }, [dirty, onNavigationStateChange, saving])

  useEffect(() => () => {
    onNavigationStateChange?.({ dirty: false, saving: false })
  }, [onNavigationStateChange])

  useEffect(() => {
    if (!dirty && !saving) return
    const preventAccidentalClose = (event: BeforeUnloadEvent) => {
      event.preventDefault()
      event.returnValue = ''
    }
    window.addEventListener('beforeunload', preventAccidentalClose)
    return () => window.removeEventListener('beforeunload', preventAccidentalClose)
  }, [dirty, saving])

  useEffect(() => {
    if (connectionState !== 'connected') {
      wasDisconnected.current = true
      setLoading(false)
      setLoadError('等待 WebSocket 连接后读取配置。')
      return
    }
    const preserveDraft = shouldPreserveConfigDraftOnReconnect(
      wasDisconnected.current,
      dirtyRef.current,
    )
    wasDisconnected.current = false
    if (preserveDraft) {
      setLoading(false)
      setLoadError('')
      return
    }
    let disposed = false
    setLoading(true)
    setLoadError('')
    setSaveError(null)
    void getConfigDocument(selectedDocumentId)
      .then((next) => {
        if (disposed) return
        setDocument(next)
        setDraft(cloneValue(next.value))
        setDirty(false)
      })
      .catch((error) => {
        if (disposed) return
        setDocument(null)
        setDraft(null)
        setLoadError(error instanceof Error ? error.message : String(error))
      })
      .finally(() => {
        if (!disposed) setLoading(false)
      })
    return () => { disposed = true }
  }, [connectionState, getConfigDocument, reloadGeneration, selectedDocumentId])

  function updateDraft(next: ConfigValue) {
    setDraft(next)
    setDirty(true)
  }

  function discardDraft() {
    if (!document) return
    setDraft(cloneValue(document.value))
    setDirty(false)
    setSaveError(null)
  }

  function selectDocument(nextDocumentId: string) {
    if (nextDocumentId === selectedDocumentId || navigationBlocked) return
    if (dirty && !window.confirm('当前配置尚未保存。是否放弃修改并切换配置项？')) return
    setDirty(false)
    dirtyRef.current = false
    setSelectedDocumentId(nextDocumentId)
  }

  async function save() {
    if (!document || !draft || saveDisabled) return
    setSaving(true)
    setSaveError(null)
    try {
      const saved = await saveConfig({
        documentId: document.documentId,
        expectedRevision: document.revision,
        value: draft,
      })
      setDocument(saved)
      setDraft(cloneValue(saved.value))
      setDirty(false)
    } catch (error) {
      if (error instanceof ConfigRequestError) {
        setSaveError({ code: error.code, message: error.message })
      } else {
        setSaveError({ code: 'save_failed', message: error instanceof Error ? error.message : String(error) })
      }
    } finally {
      setSaving(false)
    }
  }

  const saveErrorTitle = saveError?.code.includes('conflict')
    ? '保存冲突'
    : saveError?.code.includes('validation') || saveError?.code.includes('invalid')
      ? '验证错误'
      : '保存失败'

  return (
    <div className="config-page">
      <aside className="config-page__catalog panel">
        <header className="panel__header">
          <div><h3>配置项目</h3><small>{configCatalogReady ? `${catalogItems.length} 个测试项目` : '正在读取…'}</small></div>
          <button className="button button--quiet button--compact" disabled={loading || saving} onClick={() => void refreshConfigCatalog().catch(() => undefined)} type="button"><ArrowClockwise aria-hidden="true" size={14} />刷新</button>
        </header>
        {configCatalogError && <p className="config-page__catalog-error">读取失败：{configCatalogError}</p>}
        <div className="config-page__document-list" role="list">
          {documents.map((entry) => {
            const item = catalogItems.find((candidate) => candidate.documentId === entry.documentId)
            const selected = entry.documentId === selectedDocumentId
            return (
              <button
                aria-current={selected ? 'true' : undefined}
                className={selected ? 'config-page__document is-active' : 'config-page__document'}
                disabled={!selected && navigationBlocked}
                key={entry.documentId}
                onClick={() => selectDocument(entry.documentId)}
                role="listitem"
                type="button"
              >
                <strong>{entry.title}</strong>
                {item && <small>{item.enabled ? '启用' : '已停用'} · #{item.order + 1}{item.valid ? '' : ' · 无效'}</small>}
              </button>
            )
          })}
        </div>
      </aside>

      <main className="config-page__editor panel">
        <header className="panel__header">
          <div>
            <h3>{selectedDocumentTitle}</h3>
            <small>{document ? `版本 ${document.revision.slice(0, 12)}` : '等待配置内容'}</small>
          </div>
          <div className="config-page__actions">
            <button className="button button--quiet" disabled={!dirty || loading || saving} onClick={discardDraft} type="button"><ArrowCounterClockwise aria-hidden="true" size={15} />放弃修改</button>
            <button aria-label="保存配置" className="button button--primary" disabled={saveDisabled} onClick={() => void save()} type="button">
              <FloppyDisk aria-hidden="true" size={15} />{saving ? '保存中…' : '保存配置'}
            </button>
          </div>
        </header>

        {writeBlocked && <p className="config-editor__lock" role="status">测试正在运行，系统配置暂时只读；请返回测试工作台停止测试后再修改。</p>}
        {document?.documentId === 'mbddf-station' && hardwareOptions && (
          <p className={hardwareOptions.state === 'available' ? 'config-editor__hint' : 'config-editor__lock'} role="status">
            {hardwareOptions.state === 'available'
              ? `已检测到 ${hardwareOptions.devices.length} 台 NI 设备；也可以手工填写设备名和序列号。`
              : `NI 设备未自动检测：${hardwareOptions.message || '当前不可用'}。仍可手工填写。`}
          </p>
        )}
        {saveError && (
          <p className="config-editor__error" role="alert">
            <strong>{saveErrorTitle}（{saveError.code}）</strong>{saveError.message}
            {saveError.code === 'config_conflict' && (
              <button className="button button--quiet button--compact" onClick={() => setReloadGeneration((value) => value + 1)} type="button">
                <ArrowClockwise aria-hidden="true" size={14} />重新读取并放弃草稿
              </button>
            )}
          </p>
        )}

        {loading ? (
          <div className="compact-empty">正在读取配置…</div>
        ) : loadError ? (
          <div className="config-editor__error" role="alert">读取失败：{loadError}</div>
        ) : document && draft ? (
          <div className="config-editor">
            {document.documentId === 'test-config-catalog' ? (
              <CatalogEditor disabled={writeBlocked || saving} fallbackItems={catalogItems} onChange={updateDraft} value={draft} />
            ) : formSchema ? (
              <ConfigForm
                disabled={writeBlocked || saving}
                onChange={updateDraft}
                optionSources={optionSources}
                schema={formSchema}
                validationErrors={formErrors}
                value={draft}
              />
            ) : (
              <div className="compact-empty">该配置尚未提供产品工程师表单，当前不可编辑。</div>
            )}
          </div>
        ) : null}
      </main>
    </div>
  )
}

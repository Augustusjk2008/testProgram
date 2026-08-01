import { ArrowClockwise, ArrowCounterClockwise, ArrowDown, ArrowUp, FloppyDisk } from '@phosphor-icons/react'
import { useEffect, useMemo, useState } from 'react'

import type { ConfigCatalogItem, ConfigDocument } from '../shared/protocol'
import { ConfigRequestError } from '../shared/ws/HwtestClient'
import { useSession } from '../features/session/SessionProvider'
import {
  catalogEditorItems,
  isConfigDocumentNavigationBlocked,
  testConfigFormFields,
  updateCatalogDocument,
  updateTestConfigDocument,
  type ConfigValue,
  type EditableCatalogItem,
  type TestConfigField,
} from './config-draft'

const FIXED_DOCUMENTS = [
  { documentId: 'test-config-catalog', title: '测试配置目录', kind: 'catalog' },
  { documentId: 'mbddf-station', title: 'MB_DDF 工位', kind: 'station' },
] as const

function cloneValue(value: ConfigValue): ConfigValue {
  return JSON.parse(JSON.stringify(value)) as ConfigValue
}

function jsonText(value: unknown): string {
  return JSON.stringify(value ?? {}, null, 2)
}

function asRecord(value: unknown): ConfigValue | null {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
    ? value as ConfigValue
    : null
}

function isWriteBlocked(phase: string): boolean {
  return phase !== 'empty' && phase !== 'configured'
}

function isTestConfig(document: ConfigDocument): boolean {
  return document.kind === 'testcfg' || document.documentId.endsWith('.testcfg.json')
}

function JsonEditor({
  label,
  value,
  disabled,
  onValueChange,
  onError,
}: {
  label: string
  value: ConfigValue
  disabled: boolean
  onValueChange: (value: ConfigValue) => void
  onError: (message: string) => void
}) {
  const [text, setText] = useState(() => jsonText(value))

  useEffect(() => {
    setText(jsonText(value))
  }, [value])

  return (
    <label className="config-field config-field--json">
      <span>{label}</span>
      <textarea
        aria-label={label}
        disabled={disabled}
        onChange={(event) => {
          const next = event.target.value
          setText(next)
          try {
            const parsed = asRecord(JSON.parse(next))
            if (!parsed) throw new Error('根节点必须是 JSON 对象')
            onError('')
            onValueChange(parsed)
          } catch (error) {
            const detail = error instanceof Error ? error.message : 'JSON 格式无效'
            onError(`${label}：${detail}`)
          }
        }}
        spellCheck={false}
        value={text}
      />
    </label>
  )
}

function TestConfigEditor({
  value,
  disabled,
  onChange,
  onJsonError,
}: {
  value: ConfigValue
  disabled: boolean
  onChange: (value: ConfigValue) => void
  onJsonError: (field: string, message: string) => void
}) {
  const fields = testConfigFormFields(value)
  const update = (field: TestConfigField, next: unknown) => {
    onChange(updateTestConfigDocument(value, field, next))
  }

  return (
    <div className="config-editor__fields">
      <label className="config-field">
        <span>标题</span>
        <input
          aria-label="标题"
          disabled={disabled}
          onChange={(event) => update('title', event.target.value)}
          value={fields.title}
        />
      </label>
      <label className="config-field config-field--wide">
        <span>说明</span>
        <textarea
          aria-label="说明"
          disabled={disabled}
          onChange={(event) => update('description', event.target.value)}
          value={fields.description}
        />
      </label>
      <label className="config-field">
        <span>步骤 ID</span>
        <input
          aria-label="步骤 ID"
          disabled={disabled}
          onChange={(event) => update('stepId', event.target.value)}
          value={fields.stepId}
        />
      </label>
      <label className="config-field">
        <span>测试项 ID</span>
        <input
          aria-label="测试项 ID"
          disabled={disabled}
          onChange={(event) => update('testItemId', event.target.value)}
          value={fields.testItemId}
        />
      </label>
      <JsonEditor
        disabled={disabled}
        label="步骤参数 JSON"
        onError={(message) => onJsonError('parameters', message)}
        onValueChange={(next) => update('parameters', next)}
        value={fields.parameters}
      />
      <JsonEditor
        disabled={disabled}
        label="步骤 JSON"
        onError={(message) => onJsonError('step', message)}
        onValueChange={(next) => update('step', next)}
        value={fields.step}
      />
      <JsonEditor
        disabled={disabled}
        label="执行配置 JSON"
        onError={(message) => onJsonError('executionConfig', message)}
        onValueChange={(next) => update('executionConfig', next)}
        value={fields.executionConfig}
      />
    </div>
  )
}

function CatalogEditor({
  value,
  fallbackItems,
  disabled,
  onChange,
  onJsonError,
}: {
  value: ConfigValue
  fallbackItems: readonly ConfigCatalogItem[]
  disabled: boolean
  onChange: (value: ConfigValue) => void
  onJsonError: (field: string, message: string) => void
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
      <p className="config-editor__hint">启停和排序仅在保存“测试配置目录”后由后端持久化并刷新运行目录。</p>
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
              <small>{item.documentId}{item.valid ? '' : ` · ${item.message || '无效配置'}`}</small>
            </div>
            <span className="config-catalog-editor__order">#{item.order + 1}</span>
            <div className="config-catalog-editor__move">
              <button aria-label={`${item.title || item.documentId} 上移`} disabled={disabled || index === 0} onClick={() => move(item.documentId, -1)} title="上移" type="button"><ArrowUp aria-hidden="true" size={14} /></button>
              <button aria-label={`${item.title || item.documentId} 下移`} disabled={disabled || index === items.length - 1} onClick={() => move(item.documentId, 1)} title="下移" type="button"><ArrowDown aria-hidden="true" size={14} /></button>
            </div>
          </article>
        ))}
      </div>
      <details className="config-editor__advanced">
        <summary>高级目录 JSON</summary>
        <JsonEditor disabled={disabled} label="目录 JSON" onError={(message) => onJsonError('catalog', message)} onValueChange={onChange} value={value} />
      </details>
    </div>
  )
}

function StationEditor({
  value,
  disabled,
  onChange,
  onJsonError,
}: {
  value: ConfigValue
  disabled: boolean
  onChange: (value: ConfigValue) => void
  onJsonError: (field: string, message: string) => void
}) {
  return (
    <div className="config-editor__fields">
      <p className="config-editor__hint">使用 JSON 编辑串口、波特率、PXI-6259 / PXI-6733 identity，以及资源端口和通道映射；未知或受保护字段会由后端拒绝。</p>
      <JsonEditor
        disabled={disabled}
        label="工位配置 JSON"
        onError={(message) => onJsonError('station', message)}
        onValueChange={onChange}
        value={value}
      />
    </div>
  )
}

function GenericEditor({
  value,
  disabled,
  onChange,
  onJsonError,
}: {
  value: ConfigValue
  disabled: boolean
  onChange: (value: ConfigValue) => void
  onJsonError: (field: string, message: string) => void
}) {
  return (
    <div className="config-editor__fields">
      <JsonEditor
        disabled={disabled}
        label="配置 JSON"
        onError={(message) => onJsonError('document', message)}
        onValueChange={onChange}
        value={value}
      />
    </div>
  )
}

export function ConfigPage() {
  const {
    configCatalog,
    configCatalogError,
    configCatalogReady,
    connectionState,
    getConfigDocument,
    refreshConfigCatalog,
    saveConfig,
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
  const [jsonErrors, setJsonErrors] = useState<Record<string, string>>({})
  const [reloadGeneration, setReloadGeneration] = useState(0)

  const catalogItems = useMemo(() => [...(configCatalog?.items ?? [])]
    .sort((left, right) => left.order - right.order || left.documentId.localeCompare(right.documentId)), [configCatalog])
  const documents = useMemo(() => [
    ...FIXED_DOCUMENTS,
    ...catalogItems
      .filter((item) => !FIXED_DOCUMENTS.some((fixed) => fixed.documentId === item.documentId))
      .map((item) => ({ documentId: item.documentId, title: item.title || item.documentId, kind: 'testcfg' })),
  ], [catalogItems])
  const writeBlocked = isWriteBlocked(snapshot.phase)
  const navigationBlocked = isConfigDocumentNavigationBlocked(dirty, loading, saving)
  const hasJsonError = Object.values(jsonErrors).some(Boolean)
  const saveDisabled = !document || !draft || !dirty || loading || saving || writeBlocked || hasJsonError

  useEffect(() => {
    if (connectionState !== 'connected') {
      setLoading(false)
      setLoadError('等待 WebSocket 连接后读取配置文档。')
      return
    }
    let disposed = false
    setLoading(true)
    setLoadError('')
    setSaveError(null)
    setJsonErrors({})
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

  function setJsonError(field: string, message: string) {
    setJsonErrors((current) => ({ ...current, [field]: message }))
  }

  function discardDraft() {
    if (!document) return
    setDraft(cloneValue(document.value))
    setDirty(false)
    setJsonErrors({})
    setSaveError(null)
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
      setJsonErrors({})
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
          <div><h3>配置目录</h3><small>{configCatalogReady ? `${catalogItems.length} 个 testcfg` : '正在读取目录…'}</small></div>
          <button className="button button--quiet button--compact" disabled={loading || saving} onClick={() => void refreshConfigCatalog().catch(() => undefined)} type="button"><ArrowClockwise aria-hidden="true" size={14} />刷新</button>
        </header>
        {configCatalogError && <p className="config-page__catalog-error">目录读取失败：{configCatalogError}</p>}
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
                onClick={() => setSelectedDocumentId(entry.documentId)}
                role="listitem"
                type="button"
              >
                <strong>{entry.title}</strong>
                <span>{entry.documentId}</span>
                {item && <small>{item.enabled ? '启用' : '已停用'} · #{item.order + 1}{item.valid ? '' : ' · 无效'}</small>}
              </button>
            )
          })}
        </div>
      </aside>

      <main className="config-page__editor panel">
        <header className="panel__header">
          <div>
            <h3>{document?.documentId ?? selectedDocumentId}</h3>
            <small>{document ? `${document.kind} · revision ${document.revision}` : '等待配置文档'}</small>
          </div>
          <div className="config-page__actions">
            <button className="button button--quiet" disabled={!dirty || loading || saving} onClick={discardDraft} type="button"><ArrowCounterClockwise aria-hidden="true" size={15} />放弃修改</button>
            <button aria-label="保存配置" className="button button--primary" disabled={saveDisabled} onClick={() => void save()} type="button">
              <FloppyDisk aria-hidden="true" size={15} />{saving ? '保存中…' : '保存配置'}
            </button>
          </div>
        </header>

        {writeBlocked && <p className="config-editor__lock" role="status">运行态禁止保存配置；请等待测试停止后再保存。</p>}
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
        {Object.values(jsonErrors).filter(Boolean).map((error) => <p className="config-editor__error" key={error} role="alert">{error}</p>)}

        {loading ? (
          <div className="compact-empty">正在读取配置文档…</div>
        ) : loadError ? (
          <div className="config-editor__error" role="alert">读取失败：{loadError}</div>
        ) : document && draft ? (
          <div className="config-editor">
            {document.documentId === 'test-config-catalog' ? (
              <CatalogEditor disabled={writeBlocked || saving} fallbackItems={catalogItems} onChange={updateDraft} onJsonError={setJsonError} value={draft} />
            ) : document.documentId === 'mbddf-station' ? (
              <StationEditor disabled={writeBlocked || saving} onChange={updateDraft} onJsonError={setJsonError} value={draft} />
            ) : isTestConfig(document) ? (
              <TestConfigEditor disabled={writeBlocked || saving} onChange={updateDraft} onJsonError={setJsonError} value={draft} />
            ) : (
              <GenericEditor disabled={writeBlocked || saving} onChange={updateDraft} onJsonError={setJsonError} value={draft} />
            )}
          </div>
        ) : null}
      </main>
    </div>
  )
}

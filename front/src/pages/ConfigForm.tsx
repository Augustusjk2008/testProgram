import type { ConfigValue } from './config-draft'
import {
  appendConfigArrayItem,
  configValueAtPath,
  removeConfigArrayItem,
  updateConfigValueAtPath,
} from './config-form-model'
import type {
  ConfigFormField,
  ConfigFormList,
  ConfigFormOption,
  ConfigFormScalar,
  ConfigFormSchema,
} from './config-form-schema'
import type { ConfigFormValidationError } from './config-form-validation'

interface ConfigFormProps {
  schema: ConfigFormSchema
  value: ConfigValue
  disabled: boolean
  optionSources?: Record<string, ConfigFormOption[]>
  validationErrors?: ConfigFormValidationError[]
  onChange: (value: ConfigValue) => void
}

function optionKey(value: ConfigFormScalar): string {
  return `${typeof value}:${String(value)}`
}

function resolvedOptions(
  field: ConfigFormField,
  optionSources: Record<string, ConfigFormOption[]>,
): ConfigFormOption[] {
  const seen = new Set<string>()
  return [...field.options, ...(optionSources[field.optionsSource] ?? [])]
    .filter((option) => {
      const key = optionKey(option.value)
      if (seen.has(key)) return false
      seen.add(key)
      return true
    })
}

function numberValue(value: unknown): number | '' {
  return typeof value === 'number' && Number.isFinite(value) ? value : ''
}

function textValue(value: unknown): string {
  return typeof value === 'string' ? value : ''
}

function datalistId(path: string, instance: string): string {
  return `config-options-${instance}-${path.replace(/[^a-zA-Z0-9_-]+/g, '-')}`
}

function FieldControl({
  field,
  fieldPath,
  instance,
  value,
  disabled,
  optionSources,
  validationErrors,
  onChange,
}: {
  field: ConfigFormField
  fieldPath: string
  instance: string
  value: ConfigValue
  disabled: boolean
  optionSources: Record<string, ConfigFormOption[]>
  validationErrors: ConfigFormValidationError[]
  onChange: (value: ConfigValue) => void
}) {
  const configured = configValueAtPath(value, fieldPath)
  const inheritsDefault = configured === undefined && field.defaultValue !== undefined
  const current = inheritsDefault ? field.defaultValue : configured
  const update = (next: unknown) => onChange(updateConfigValueAtPath(value, fieldPath, next))
  const options = resolvedOptions(field, optionSources)
  const fieldDisabled = disabled || field.readOnly
  const errors = validationErrors.filter((error) => error.path === fieldPath)
  let control

  if (field.kind === 'choice') {
    let selectedIndex = options.findIndex((option) => Object.is(option.value, current))
    if (selectedIndex < 0 && field.acceptOptionIndex && typeof current === 'number' &&
        Number.isSafeInteger(current) && current >= 0 && current < options.length) {
      selectedIndex = current
    }
    control = (
      <select
        aria-label={field.label}
        disabled={fieldDisabled}
        onChange={(event) => {
          const selected = options[Number(event.target.value)]
          if (selected) update(selected.value)
        }}
        required={field.required}
        value={selectedIndex >= 0 ? selectedIndex : ''}
      >
        {selectedIndex < 0 && <option value="">请选择</option>}
        {options.map((option, index) => (
          <option key={optionKey(option.value)} value={index}>{option.label}</option>
        ))}
      </select>
    )
  } else if (field.kind === 'choiceOrText') {
    const listId = datalistId(fieldPath, instance)
    control = (
      <>
        <input
          aria-label={field.label}
          disabled={fieldDisabled}
          list={listId}
          onChange={(event) => update(event.target.value)}
          required={field.required}
          type="text"
          value={textValue(current)}
        />
        <datalist id={listId}>
          {options.map((option) => (
            <option key={optionKey(option.value)} value={String(option.value)}>{option.label}</option>
          ))}
        </datalist>
      </>
    )
  } else if (field.kind === 'boolean') {
    control = (
      <input
        aria-label={field.label}
        checked={current === true}
        disabled={fieldDisabled}
        onChange={(event) => update(event.target.checked)}
        type="checkbox"
      />
    )
  } else if (field.kind === 'scalar' && typeof current === 'boolean') {
    control = (
      <select
        aria-label={field.label}
        disabled={fieldDisabled}
        onChange={(event) => update(event.target.value === 'true')}
        value={String(current)}
      >
        <option value="true">是</option>
        <option value="false">否</option>
      </select>
    )
  } else if (field.kind === 'scalar' && typeof current === 'number') {
    control = (
      <input
        aria-label={field.label}
        disabled={fieldDisabled}
        onChange={(event) => update(event.target.value === '' ? '' : Number(event.target.value))}
        type="number"
        value={numberValue(current)}
      />
    )
  } else if (field.kind === 'multiline') {
    control = (
      <textarea
        aria-label={field.label}
        disabled={fieldDisabled}
        onChange={(event) => update(event.target.value)}
        required={field.required}
        value={textValue(current)}
      />
    )
  } else if (field.kind === 'integer' || field.kind === 'number') {
    control = (
      <input
        aria-label={field.label}
        disabled={fieldDisabled}
        max={field.maximum}
        min={field.minimum}
        onChange={(event) => update(event.target.value === '' ? '' : Number(event.target.value))}
        required={field.required}
        step={field.kind === 'integer' ? 1 : 'any'}
        type="number"
        value={numberValue(current)}
      />
    )
  } else {
    control = (
      <input
        aria-label={field.label}
        disabled={fieldDisabled}
        onChange={(event) => update(event.target.value)}
        required={field.required}
        type="text"
        value={textValue(current)}
      />
    )
  }

  return (
    <label
      className={field.wide ? 'config-form-field config-form-field--wide' : 'config-form-field'}
      title={field.description || field.label}
    >
      <span>{field.label}</span>
      <span className="config-form-field__control">{control}{field.unit && <b>{field.unit}</b>}</span>
      {field.description && <small>{field.description}</small>}
      {inheritsDefault && <small className="config-form-field__default">当前使用默认值</small>}
      {errors.map((error) => <small className="config-form-field__error" key={error.message}>{error.message}</small>)}
    </label>
  )
}

function fieldVisible(
  field: ConfigFormField,
  value: ConfigValue,
  defaults: Map<string, ConfigFormScalar>,
): boolean {
  if (!field.visibleWhen) return true
  const configured = configValueAtPath(value, field.visibleWhen.path)
  const current = configured === undefined ? defaults.get(field.visibleWhen.path) : configured
  return !field.visibleWhen || Object.is(
    current,
    field.visibleWhen.equals,
  )
}

function listItemPath(listPath: string, index: number, relativePath: string): string {
  return `${listPath}/${index}${relativePath}`
}

function ListEditor({
  list,
  value,
  disabled,
  optionSources,
  validationErrors,
  onChange,
}: {
  list: ConfigFormList
  value: ConfigValue
  disabled: boolean
  optionSources: Record<string, ConfigFormOption[]>
  validationErrors: ConfigFormValidationError[]
  onChange: (value: ConfigValue) => void
}) {
  const current = configValueAtPath(value, list.path)
  const items = Array.isArray(current) ? current : []
  return (
    <section className="config-form-list">
      <header>
        <div><h5>{list.label}</h5>{list.description && <p>{list.description}</p>}</div>
        {list.allowAdd && (
          <button
            className="button button--quiet button--compact"
            disabled={disabled}
            onClick={() => onChange(appendConfigArrayItem(
              value,
              list.path,
              JSON.parse(JSON.stringify(list.itemDefaults)) as Record<string, unknown>,
            ))}
            type="button"
          >
            {list.addLabel}
          </button>
        )}
      </header>
      {items.length === 0 ? <p className="config-form-list__empty">尚未配置{list.label}</p> : (
        <div className="config-form-list__items">
          {items.map((_, index) => (
            <article className="config-form-list__item" key={`${list.path}-${index}`}>
              <div className="config-form-list__fields">
                {list.columns.map((field) => (
                  <FieldControl
                    disabled={disabled}
                    field={field}
                    fieldPath={listItemPath(list.path, index, field.path)}
                    instance={`row-${index}`}
                    key={field.path}
                    onChange={onChange}
                    optionSources={optionSources}
                    validationErrors={validationErrors}
                    value={value}
                  />
                ))}
              </div>
              {list.allowRemove && (
                <button
                  aria-label={`删除第 ${index + 1} 项${list.label}`}
                  className="button button--danger button--compact"
                  disabled={disabled}
                  onClick={() => onChange(removeConfigArrayItem(value, list.path, index))}
                  type="button"
                >
                  删除
                </button>
              )}
            </article>
          ))}
        </div>
      )}
    </section>
  )
}

export function ConfigForm({
  schema,
  value,
  disabled,
  optionSources = {},
  validationErrors = [],
  onChange,
}: ConfigFormProps) {
  const defaults = new Map<string, ConfigFormScalar>()
  schema.sections.forEach((section) => section.fields.forEach((field) => {
    if (field.defaultValue !== undefined) defaults.set(field.path, field.defaultValue)
  }))
  return (
    <div className="config-form">
      {schema.sections.map((section) => (
        <section className="config-form-section" key={section.id}>
          <header className="config-form-section__header">
            <h4>{section.title}</h4>
            {section.description && <p>{section.description}</p>}
          </header>
          {section.fields.length > 0 && (
            <div className="config-form-section__fields">
              {section.fields.filter((field) => fieldVisible(field, value, defaults)).map((field, index) => (
                <FieldControl
                  disabled={disabled}
                  field={field}
                  fieldPath={field.path}
                  instance={section.id}
                  key={`${field.path}-${index}`}
                  onChange={onChange}
                  optionSources={optionSources}
                  validationErrors={validationErrors}
                  value={value}
                />
              ))}
            </div>
          )}
          {section.lists.map((list) => (
            <ListEditor
              disabled={disabled}
              key={list.path}
              list={list}
              onChange={onChange}
              optionSources={optionSources}
              validationErrors={validationErrors}
              value={value}
            />
          ))}
        </section>
      ))}
    </div>
  )
}

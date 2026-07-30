import type {
  TestDescriptor,
  TestRunParameterDescriptor,
} from '../../shared/protocol'
import { visibleRunParameters, type RunParameterValues } from './run-parameters'

interface RunParameterEditorProps {
  descriptor: TestDescriptor
  values: RunParameterValues
  disabled: boolean
  effective: boolean
  onChange: (values: RunParameterValues) => void
  onReset: () => void
}

const DH_IGNITE_ALGORITHM_ID = 'mbddf.dh_ignite_stream'
const DO_WRITE_ALGORITHM_ID = 'mbddf.do_write'

function channelIndex(parameterId: string): number | null {
  const match = /^channel_enabled\[(\d+)\]$/.exec(parameterId)
  if (!match) return null
  const index = Number(match[1])
  return Number.isSafeInteger(index) ? index : null
}

function checkboxValue(parameter: TestRunParameterDescriptor, checked: boolean): boolean | number {
  return parameter.kind === 'integer' ? (checked ? 1 : 0) : checked
}

function checkedValue(value: unknown): boolean {
  return value === true || value === 1
}

function isDhIntegerToggle(
  descriptor: TestDescriptor,
  parameter: TestRunParameterDescriptor,
): boolean {
  return descriptor.algorithmId === DH_IGNITE_ALGORITHM_ID &&
    (parameter.id === 'power_enable' || parameter.id === 'return_enable')
}

function numberValue(value: unknown): string | number {
  return typeof value === 'number' && Number.isFinite(value) ? value : ''
}

function field(
  descriptor: TestDescriptor,
  parameter: TestRunParameterDescriptor,
  values: RunParameterValues,
  disabled: boolean,
  onChange: (values: RunParameterValues) => void,
) {
  const value = values[parameter.id]
  const update = (next: unknown) => onChange({ ...values, [parameter.id]: next })

  if (isDhIntegerToggle(descriptor, parameter)) {
    return (
      <input
        aria-label={parameter.label}
        checked={value === 1}
        disabled={disabled}
        onChange={(event) => update(event.target.checked ? 1 : 0)}
        type="checkbox"
      />
    )
  }

  if (parameter.kind === 'choice') {
    const selectedIndex = parameter.choices.findIndex((choice) => choice.value === value)
    return (
      <select
        aria-label={parameter.label}
        disabled={disabled}
        onChange={(event) => {
          const choice = parameter.choices[Number(event.target.value)]
          if (choice) update(choice.value)
        }}
        value={selectedIndex >= 0 ? selectedIndex : ''}
      >
        {parameter.choices.map((choice, index) => (
          <option key={`${parameter.id}-${String(choice.value)}`} value={index}>
            {choice.label}
          </option>
        ))}
      </select>
    )
  }

  if (parameter.kind === 'boolean') {
    return (
      <input
        aria-label={parameter.label}
        checked={value === true}
        disabled={disabled}
        onChange={(event) => update(event.target.checked)}
        type="checkbox"
      />
    )
  }

  return (
    <span className="algorithm-parameter__number">
      <input
        aria-label={parameter.label}
        disabled={disabled}
        max={parameter.maximum}
        min={parameter.minimum}
        onChange={(event) => update(event.target.value === '' ? '' : Number(event.target.value))}
        step={parameter.kind === 'integer' ? 1 : 'any'}
        type="number"
        value={numberValue(value)}
      />
      {parameter.unit && <b>{parameter.unit}</b>}
    </span>
  )
}

function ChannelGrid({
  kind,
  parameters,
  values,
  disabled,
  onChange,
}: {
  kind: 'dh' | 'do'
  parameters: TestRunParameterDescriptor[]
  values: RunParameterValues
  disabled: boolean
  onChange: (values: RunParameterValues) => void
}) {
  const isDh = kind === 'dh'
  const update = (parameter: TestRunParameterDescriptor, checked: boolean) => {
    onChange({ ...values, [parameter.id]: checkboxValue(parameter, checked) })
  }
  const setDhChannels = (checked: boolean) => {
    const next = { ...values }
    parameters.forEach((parameter) => {
      next[parameter.id] = checkboxValue(parameter, checked)
    })
    onChange(next)
  }

  return (
    <fieldset className={isDh ? 'algorithm-channel-grid algorithm-channel-grid--dh' : 'algorithm-channel-grid algorithm-channel-grid--do'}>
      <legend>{isDh ? '23 路点火通道' : '16 路数字输出'}</legend>
      {isDh && (
        <div className="algorithm-channel-grid__toolbar">
          <button disabled={disabled} onClick={() => setDhChannels(true)} type="button">全选</button>
          <button disabled={disabled} onClick={() => setDhChannels(false)} type="button">全不选</button>
        </div>
      )}
      <div className="algorithm-channel-grid__items">
        {parameters.map((parameter) => {
          const index = channelIndex(parameter.id)
          const guarded = !isDh && (index === 5 || index === 6)
          const note = !isDh && index !== null && (index === 3 || index === 4)
            ? `DO${index}：低有效，默认安全位为 1（关闭）`
            : ''
          return (
            <label className={guarded ? 'algorithm-channel is-guarded' : 'algorithm-channel'} key={parameter.id} title={note || parameter.description || parameter.label}>
              <input
                aria-label={parameter.label}
                checked={guarded ? false : checkedValue(values[parameter.id])}
                disabled={disabled || guarded}
                onChange={(event) => update(parameter, event.target.checked)}
                type="checkbox"
              />
              <span>{parameter.label}</span>
              {note && <small>{note}</small>}
              {guarded && <small>固定关闭</small>}
            </label>
          )
        })}
      </div>
    </fieldset>
  )
}

export function RunParameterEditor({
  descriptor,
  values,
  disabled,
  effective,
  onChange,
  onReset,
}: RunParameterEditorProps) {
  if (descriptor.runParameters.length === 0) return null
  const parameters = visibleRunParameters(descriptor, values)
  const dhChannels = descriptor.algorithmId === DH_IGNITE_ALGORITHM_ID
    ? parameters.filter((parameter) => {
      const index = channelIndex(parameter.id)
      return index !== null && index >= 0 && index < 23
    }).sort((left, right) => (channelIndex(left.id) ?? 0) - (channelIndex(right.id) ?? 0))
    : []
  const doChannels = descriptor.algorithmId === DO_WRITE_ALGORITHM_ID
    ? parameters.filter((parameter) => {
      const index = channelIndex(parameter.id)
      return index !== null && index >= 0 && index < 16
    }).sort((left, right) => (channelIndex(left.id) ?? 0) - (channelIndex(right.id) ?? 0))
    : []
  const groupedChannels = new Set([...dhChannels, ...doChannels].map((parameter) => parameter.id))
  const regularParameters = parameters.filter((parameter) => !groupedChannels.has(parameter.id))

  return (
    <details className="algorithm-parameters" open>
      <summary>
        <span>测试参数</span>
        <b>{parameters.length} 项{effective ? ' · 本次已生效' : ''}</b>
        <button
          className="button button--quiet button--compact"
          disabled={disabled}
          onClick={onReset}
          type="button"
        >
          设为默认
        </button>
      </summary>
      {regularParameters.length > 0 && (
        <div className="algorithm-parameters__grid">
          {regularParameters.map((parameter) => (
            <label
              className="algorithm-parameter"
              key={parameter.id}
              title={parameter.description || parameter.label}
            >
              <span>{parameter.label}</span>
              {field(descriptor, parameter, values, disabled, onChange)}
            </label>
          ))}
        </div>
      )}
        {dhChannels.length > 0 && (
          <ChannelGrid
            disabled={disabled}
            kind="dh"
            onChange={onChange}
            parameters={dhChannels}
            values={values}
          />
        )}
        {doChannels.length > 0 && (
          <ChannelGrid
            disabled={disabled}
            kind="do"
            onChange={onChange}
            parameters={doChannels}
            values={values}
          />
        )}
    </details>
  )
}

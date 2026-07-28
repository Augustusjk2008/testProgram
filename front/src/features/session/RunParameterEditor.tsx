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

function numberValue(value: unknown): string | number {
  return typeof value === 'number' && Number.isFinite(value) ? value : ''
}

function field(
  parameter: TestRunParameterDescriptor,
  values: RunParameterValues,
  disabled: boolean,
  onChange: (values: RunParameterValues) => void,
) {
  const value = values[parameter.id]
  const update = (next: unknown) => onChange({ ...values, [parameter.id]: next })

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
      <div className="algorithm-parameters__grid">
        {parameters.map((parameter) => (
          <label
            className="algorithm-parameter"
            key={parameter.id}
            title={parameter.description || parameter.label}
          >
            <span>{parameter.label}</span>
            {field(parameter, values, disabled, onChange)}
          </label>
        ))}
      </div>
    </details>
  )
}

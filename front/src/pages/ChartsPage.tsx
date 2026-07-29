import { ChartLine } from '@phosphor-icons/react'
import { useEffect, useMemo, useRef, useState } from 'react'

import { ChartConfigurator } from '../features/telemetry/ChartConfigurator'
import { TelemetryChart } from '../features/telemetry/TelemetryChart'
import {
  buildChartGroups,
  type ChartLayout,
  createAssignments,
  type SeriesAssignment,
} from '../features/telemetry/series-config'
import { chartWorkspaceStorageKey } from '../features/telemetry/chart-workspace-storage'
import { useSession, useTelemetry } from '../features/session/SessionProvider'
import { DEFAULT_TIME_WINDOW_SECONDS } from '../shared/config'
import { fieldLabel } from '../shared/format'
import { setLocalStorageValue } from '../shared/storage'

interface StoredConfig {
  layout: ChartLayout
  windowSeconds: number
  assignments: SeriesAssignment[]
}

function loadConfig(storageKey: string): StoredConfig {
  try {
    const value = window.localStorage.getItem(storageKey)
    if (value) return JSON.parse(value) as StoredConfig
  } catch {
    // Fall back to the initial workspace when storage is unavailable.
  }
  return { layout: 'combined', windowSeconds: DEFAULT_TIME_WINDOW_SECONDS, assignments: [] }
}

export function ChartsPage() {
  const { snapshot } = useSession()
  const { clearTelemetry, dataVersion, fields, telemetry } = useTelemetry()
  const descriptor = snapshot.descriptor
  const storageKey = useMemo(
    () => chartWorkspaceStorageKey(descriptor.configId, descriptor.algorithmId),
    [descriptor.algorithmId, descriptor.configId],
  )
  const initial = useMemo(() => loadConfig(storageKey), [])
  const [layout, setLayout] = useState<ChartLayout>(initial.layout)
  const [windowSeconds, setWindowSeconds] = useState(initial.windowSeconds)
  const [assignments, setAssignments] = useState<SeriesAssignment[]>(initial.assignments)
  const skipNextSave = useRef(false)

  useEffect(() => {
    const stored = loadConfig(storageKey)
    skipNextSave.current = true
    setLayout(stored.layout)
    setWindowSeconds(stored.windowSeconds)
    setAssignments(stored.assignments)
  }, [storageKey])

  useEffect(() => {
    const descriptorFields = descriptor.measurements.map(({ id }) => id)
    const availableFields = fields.length > 0 ? fields : descriptorFields
    setAssignments((current) => {
      const scopedCurrent = descriptorFields.length > 0
        ? current.filter(({ path }) => descriptorFields.includes(path))
        : current
      const next = createAssignments(availableFields, scopedCurrent, descriptor.measurements)
      return next
    })
  }, [descriptor.measurements, fields])

  useEffect(() => {
    if (skipNextSave.current) {
      skipNextSave.current = false
      return
    }
    setLocalStorageValue(storageKey, JSON.stringify({
      layout,
      windowSeconds,
      assignments,
    }))
  }, [assignments, layout, storageKey, windowSeconds])

  const groups = useMemo(
    () => buildChartGroups(assignments, layout),
    [assignments, layout],
  )

  return (
    <div className="charts-workspace">
      <ChartConfigurator
        assignments={assignments}
        layout={layout}
        onAssignmentsChange={setAssignments}
        onClear={clearTelemetry}
        onLayoutChange={setLayout}
        onWindowChange={setWindowSeconds}
        windowSeconds={windowSeconds}
      />

      <main className="charts-stage">
        {groups.length === 0 ? (
          <section className="empty-state panel charts-empty">
            <ChartLine size={30} />
            <h3>{fields.length === 0 ? '等待曲线数据' : '请选择曲线'}</h3>
          </section>
        ) : (
          <div className={layout === 'separate' ? 'chart-grid chart-grid--split' : 'chart-grid'}>
            {groups.map((group) => (
              <TelemetryChart
                buffer={telemetry}
                dataVersion={dataVersion}
                fields={group.fields}
                key={`${group.id}:${group.fields.join(',')}`}
                title={layout === 'custom'
                  ? group.id
                  : group.fields.length === 1
                    ? fieldLabel(group.fields[0])
                  : `${descriptor.title || '测试'} · 合并视图`}
                windowSeconds={windowSeconds}
              />
            ))}
          </div>
        )}
      </main>
    </div>
  )
}

import { ChartLine } from '@phosphor-icons/react'
import { useEffect, useMemo, useState } from 'react'

import { ChartConfigurator } from '../features/telemetry/ChartConfigurator'
import { TelemetryChart } from '../features/telemetry/TelemetryChart'
import {
  buildChartGroups,
  type ChartLayout,
  createAssignments,
  type SeriesAssignment,
} from '../features/telemetry/series-config'
import { useSession } from '../features/session/SessionProvider'
import { DEFAULT_TIME_WINDOW_SECONDS } from '../shared/config'
import { fieldLabel } from '../shared/format'
import { setLocalStorageValue } from '../shared/storage'

const STORAGE_KEY = 'hwtest.chart-workspace.v2'

interface StoredConfig {
  layout: ChartLayout
  windowSeconds: number
  assignments: SeriesAssignment[]
}

function loadConfig(): StoredConfig {
  try {
    const value = window.localStorage.getItem(STORAGE_KEY)
    if (value) return JSON.parse(value) as StoredConfig
  } catch {
    // Fall back to the initial workspace when storage is unavailable.
  }
  return { layout: 'combined', windowSeconds: DEFAULT_TIME_WINDOW_SECONDS, assignments: [] }
}

export function ChartsPage() {
  const { clearTelemetry, dataVersion, fields, snapshot, telemetry } = useSession()
  const descriptor = snapshot.descriptor
  const initial = useMemo(loadConfig, [])
  const [layout, setLayout] = useState<ChartLayout>(initial.layout)
  const [windowSeconds, setWindowSeconds] = useState(initial.windowSeconds)
  const [assignments, setAssignments] = useState<SeriesAssignment[]>(initial.assignments)

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
    setLocalStorageValue(STORAGE_KEY, JSON.stringify({
      layout,
      windowSeconds,
      assignments,
    }))
  }, [assignments, layout, windowSeconds])

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

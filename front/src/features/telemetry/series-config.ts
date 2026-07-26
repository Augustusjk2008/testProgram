export type ChartLayout = 'combined' | 'separate' | 'custom'

export interface SeriesAssignment {
  path: string
  enabled: boolean
  groupId: string
}

export interface ChartGroup {
  id: string
  fields: string[]
}

function defaultGroup(path: string): string {
  const normalized = path.toLowerCase()
  if (normalized.includes('temp') || normalized.includes('thermal')) return 'temperature'
  if (normalized.includes('usage') || normalized.includes('load')) return 'load'
  if (normalized.includes('freq')) return 'frequency'
  if (normalized.includes('pcie') || normalized.includes('width') || normalized.includes('speed')) return 'link'
  if (normalized.includes('time') || normalized.includes('sec')) return 'runtime'
  return 'system'
}

function enabledByDefault(path: string): boolean {
  const normalized = path.toLowerCase()
  return normalized === 'cpu_usage' ||
    normalized === 'mem_usage' ||
    normalized.includes('temp') ||
    normalized.includes('thermal')
}

export function createAssignments(
  fields: string[],
  existing: SeriesAssignment[] = [],
): SeriesAssignment[] {
  const known = new Set(existing.map(({ path }) => path))
  return [
    ...existing,
    ...fields
      .filter((field) => !known.has(field))
      .map((path) => ({
        path,
        enabled: enabledByDefault(path),
        groupId: defaultGroup(path),
      })),
  ]
}

export function buildChartGroups(
  assignments: SeriesAssignment[],
  layout: ChartLayout,
): ChartGroup[] {
  const enabled = assignments.filter((assignment) => assignment.enabled)
  if (enabled.length === 0) return []
  if (layout === 'combined') {
    return [{ id: 'combined', fields: enabled.map(({ path }) => path) }]
  }
  if (layout === 'separate') {
    return enabled.map(({ path }) => ({ id: path, fields: [path] }))
  }

  const groups = new Map<string, string[]>()
  enabled.forEach(({ path, groupId }) => {
    const id = groupId.trim() || 'ungrouped'
    const fields = groups.get(id) ?? []
    fields.push(path)
    groups.set(id, fields)
  })
  return [...groups].map(([id, fields]) => ({ id, fields }))
}

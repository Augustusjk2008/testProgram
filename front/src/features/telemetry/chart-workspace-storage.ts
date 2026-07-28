const CHART_WORKSPACE_PREFIX = 'hwtest.chart-workspace.v2'

export function chartWorkspaceStorageKey(configId: string, algorithmId: string): string {
  const scope = configId.trim() || algorithmId.trim() || 'unselected'
  return `${CHART_WORKSPACE_PREFIX}.${encodeURIComponent(scope)}`
}

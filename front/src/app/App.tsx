import { useEffect, useRef, useState } from 'react'

import { AppShell, type PageId, type WorkspaceId } from './AppShell'
import { ChartsPage } from '../pages/ChartsPage'
import { DiagnosticsPage } from '../pages/DiagnosticsPage'
import { OverviewPage } from '../pages/OverviewPage'
import { PerformancePage } from '../pages/PerformancePage'
import { BoardTestPage } from '../pages/BoardTestPage'
import { ConfigPage } from '../pages/ConfigPage'
import { isBoardTestAlgorithm } from '../features/board-test/board-test-navigation'
import { useSession } from '../features/session/SessionProvider'
import { isPerformanceCapabilityEnabled } from '../features/performance/performance-navigation'
import { analysisIdentityKey } from '../features/performance/analysis-session-state'
import {
  canLeaveConfigurationWorkspace,
  type ConfigurationWorkspaceNavigationState,
} from '../pages/config-draft'

export function App() {
  const [page, setPage] = useState<PageId>('overview')
  const [workspace, setWorkspace] = useState<WorkspaceId>('test')
  const [configNavigationState, setConfigNavigationState] =
    useState<ConfigurationWorkspaceNavigationState>({ dirty: false, saving: false })
  const { performanceNavigationIdentity, snapshot } = useSession()
  const navigatedIdentityKey = useRef('')
  const performanceAvailable = isPerformanceCapabilityEnabled(snapshot.descriptor.postRunAnalysis)
  const boardTestAvailable = isBoardTestAlgorithm(snapshot.algorithmId || snapshot.descriptor.algorithmId)

  useEffect(() => {
    if (workspace !== 'test' || !performanceNavigationIdentity || !performanceAvailable) return
    const identityKey = analysisIdentityKey(performanceNavigationIdentity)
    if (identityKey === navigatedIdentityKey.current) return
    navigatedIdentityKey.current = identityKey
    setPage('performance')
  }, [performanceAvailable, performanceNavigationIdentity, workspace])

  useEffect(() => {
    if (page === 'performance' && !performanceAvailable) setPage('overview')
    if (page === 'board-test' && !boardTestAvailable) setPage('overview')
  }, [boardTestAvailable, page, performanceAvailable])

  return (
    <AppShell
      onPageChange={setPage}
      onWorkspaceChange={(nextWorkspace) => {
        if (workspace === 'configuration' && nextWorkspace === 'test' &&
            !canLeaveConfigurationWorkspace(configNavigationState, () => window.confirm(
              '当前配置尚未保存。是否放弃修改并返回测试工作台？',
            ))) return
        setWorkspace(nextWorkspace)
      }}
      page={page}
      workspace={workspace}
    >
      {workspace === 'configuration' ? (
        <ConfigPage onNavigationStateChange={setConfigNavigationState} />
      ) : (
        <>
          {page === 'overview' && <OverviewPage />}
          {page === 'charts' && <ChartsPage />}
          {page === 'diagnostics' && <DiagnosticsPage />}
          {page === 'performance' && <PerformancePage />}
          {page === 'board-test' && boardTestAvailable && <BoardTestPage />}
        </>
      )}
    </AppShell>
  )
}

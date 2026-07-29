import { useEffect, useRef, useState } from 'react'

import { AppShell, type PageId } from './AppShell'
import { ChartsPage } from '../pages/ChartsPage'
import { DiagnosticsPage } from '../pages/DiagnosticsPage'
import { OverviewPage } from '../pages/OverviewPage'
import { PerformancePage } from '../pages/PerformancePage'
import { useSession } from '../features/session/SessionProvider'
import { isPerformanceCapabilityEnabled } from '../features/performance/performance-navigation'
import { analysisIdentityKey } from '../features/performance/analysis-session-state'

export function App() {
  const [page, setPage] = useState<PageId>('overview')
  const { performanceNavigationIdentity, snapshot } = useSession()
  const navigatedIdentityKey = useRef('')
  const performanceAvailable = isPerformanceCapabilityEnabled(snapshot.descriptor.postRunAnalysis)

  useEffect(() => {
    if (!performanceNavigationIdentity || !performanceAvailable) return
    const identityKey = analysisIdentityKey(performanceNavigationIdentity)
    if (identityKey === navigatedIdentityKey.current) return
    navigatedIdentityKey.current = identityKey
    setPage('performance')
  }, [performanceAvailable, performanceNavigationIdentity])

  useEffect(() => {
    if (page === 'performance' && !performanceAvailable) setPage('overview')
  }, [page, performanceAvailable])

  return (
    <AppShell onPageChange={setPage} page={page}>
      {page === 'overview' && <OverviewPage />}
      {page === 'charts' && <ChartsPage />}
      {page === 'diagnostics' && <DiagnosticsPage />}
      {page === 'performance' && <PerformancePage />}
    </AppShell>
  )
}

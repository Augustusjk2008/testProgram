import { useState } from 'react'

import { AppShell, type PageId } from './AppShell'
import { ChartsPage } from '../pages/ChartsPage'
import { DiagnosticsPage } from '../pages/DiagnosticsPage'
import { OverviewPage } from '../pages/OverviewPage'

export function App() {
  const [page, setPage] = useState<PageId>('overview')
  return (
    <AppShell onPageChange={setPage} page={page}>
      {page === 'overview' && <OverviewPage />}
      {page === 'charts' && <ChartsPage />}
      {page === 'diagnostics' && <DiagnosticsPage />}
    </AppShell>
  )
}

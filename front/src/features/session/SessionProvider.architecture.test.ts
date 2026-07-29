import { describe, expect, it } from 'vitest'

import chartsPage from '../../pages/ChartsPage.tsx?raw'
import diagnosticsPage from '../../pages/DiagnosticsPage.tsx?raw'
import overviewPage from '../../pages/OverviewPage.tsx?raw'
import runControlBar from './RunControlBar.tsx?raw'
import sessionProvider from './SessionProvider.tsx?raw'

describe('SessionProvider telemetry rendering boundary', () => {
  it('keeps high-frequency telemetry out of the global session context', () => {
    const sessionContext = sessionProvider.match(
      /interface SessionContextValue\s*{([\s\S]*?)\n}/,
    )?.[1] ?? ''

    expect(sessionContext).not.toMatch(
      /latestSample|telemetry:|fields:|dataVersion|telemetryStats|diagnostics|clearTelemetry/,
    )
    expect(sessionProvider).toContain('const TelemetryContext')
    expect(sessionProvider).toContain('export function useTelemetry')
    expect(sessionProvider).toContain('useSyncExternalStore(')
  })

  it.each([
    ['RunControlBar.tsx', runControlBar],
    ['ChartsPage.tsx', chartsPage],
    ['OverviewPage.tsx', overviewPage],
    ['DiagnosticsPage.tsx', diagnosticsPage],
  ])('%s subscribes through the telemetry-only hook', (_name, source) => {
    expect(source).toContain('useTelemetry')
  })
})

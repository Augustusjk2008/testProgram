import type {
  AnalysisIdentity,
  AnalysisState,
  PostRunAnalysisCapability,
} from '../../shared/protocol'
import { analysisIdentityKey } from './analysis-session-state'

const POST_STOP_STATES = new Set<AnalysisState>([
  'queued',
  'validating',
  'preprocessing',
  'calculating',
  'persisting',
  'completed',
  'partial',
  'unavailable',
  'failed',
  'cancelled',
])

export function isPerformanceCapabilityEnabled(capability: PostRunAnalysisCapability): boolean {
  return capability.supported
}

export class PerformanceNavigationGate {
  private armedIdentityKey = ''
  private readonly consumedIdentityKeys = new Set<string>()

  recordStopSucceeded(identity: AnalysisIdentity | null): void {
    this.armedIdentityKey = identity ? analysisIdentityKey(identity) : ''
  }

  observe(identity: AnalysisIdentity | null, state: AnalysisState): AnalysisIdentity | null {
    if (!identity || !POST_STOP_STATES.has(state)) return null
    const key = analysisIdentityKey(identity)
    if (key !== this.armedIdentityKey || this.consumedIdentityKeys.has(key)) return null
    this.consumedIdentityKeys.add(key)
    this.armedIdentityKey = ''
    return identity
  }
}

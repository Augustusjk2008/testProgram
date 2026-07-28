import {
  createContext,
  type PropsWithChildren,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
} from 'react'

import {
  CHART_COMMIT_INTERVAL_MS,
  DIAGNOSTIC_CAPACITY,
  HWTEST_WS_URL,
  SAMPLE_CAPACITY,
} from '../../shared/config'
import { connectionStateLabel, phaseLabel } from '../../shared/format'
import {
  EMPTY_SNAPSHOT,
  type ActionName,
  type ApplicationSample,
  type ApplicationSnapshot,
  type DigitalStimulusSnapshot,
  type ReplyMessage,
  type TestConfigOption,
  type TestRunOptions,
} from '../../shared/protocol'
import {
  parseTestConfigCatalog,
  type ConnectionState,
  HwtestClient,
} from '../../shared/ws/HwtestClient'
import { SampleBuffer } from '../telemetry/sample-buffer'
import { selectInitialTestConfig } from './config-selection'

export interface DiagnosticEntry {
  id: number
  timestamp: number
  kind: 'link' | 'command' | 'snapshot' | 'sample' | 'error'
  title: string
  detail: string
  payload?: unknown
}

interface SessionContextValue {
  wsUrl: string
  connectionState: ConnectionState
  connectionDetail: string
  snapshot: ApplicationSnapshot
  testConfigs: TestConfigOption[]
  testConfigsReady: boolean
  selectedConfigId: string
  latestSample: ApplicationSample | null
  telemetry: SampleBuffer
  fields: string[]
  dataVersion: number
  diagnostics: DiagnosticEntry[]
  busyAction: ActionName | null
  actionError: string
  connect: (reconnecting?: boolean) => Promise<void>
  invoke: (action: ActionName, params?: Record<string, unknown>) => Promise<ReplyMessage>
  start: (options: TestRunOptions) => Promise<ReplyMessage>
  setDigitalStimulus: (
    switchId: string,
    active: boolean,
    expectedRevision: number,
  ) => Promise<DigitalStimulusSnapshot>
  resetDigitalStimulus: () => Promise<DigitalStimulusSnapshot>
  clearTelemetry: () => void
}

const SessionContext = createContext<SessionContextValue | null>(null)

export function SessionProvider({ children }: PropsWithChildren) {
  const clientRef = useRef<HwtestClient | null>(null)
  const telemetryRef = useRef(new SampleBuffer(SAMPLE_CAPACITY))
  const diagnosticsRef = useRef<DiagnosticEntry[]>([])
  const diagnosticSequence = useRef(0)
  const commitTimer = useRef<number | null>(null)
  const commitFrame = useRef<number | null>(null)
  const lastCommit = useRef(0)
  const descriptorConfigId = useRef('')
  const snapshotRef = useRef<ApplicationSnapshot>(EMPTY_SNAPSHOT)
  const autoLoadInFlight = useRef(false)

  const [connectionState, setConnectionState] = useState<ConnectionState>('disconnected')
  const [connectionDetail, setConnectionDetail] = useState('')
  const [snapshot, setSnapshot] = useState<ApplicationSnapshot>(EMPTY_SNAPSHOT)
  const [testConfigs, setTestConfigs] = useState<TestConfigOption[]>([])
  const [testConfigsReady, setTestConfigsReady] = useState(false)
  const [selectedConfigId, setSelectedConfigId] = useState('')
  const [latestSample, setLatestSample] = useState<ApplicationSample | null>(null)
  const [fields, setFields] = useState<string[]>([])
  const [dataVersion, setDataVersion] = useState(0)
  const [diagnostics, setDiagnostics] = useState<DiagnosticEntry[]>([])
  const [busyAction, setBusyAction] = useState<ActionName | null>(null)
  const [actionError, setActionError] = useState('')

  const pushDiagnostic = useCallback((
    kind: DiagnosticEntry['kind'],
    title: string,
    detail: string,
    payload?: unknown,
    deferCommit = false,
  ) => {
    const entry: DiagnosticEntry = {
      id: ++diagnosticSequence.current,
      timestamp: Date.now(),
      kind,
      title,
      detail,
      payload,
    }
    diagnosticsRef.current = [entry, ...diagnosticsRef.current]
      .slice(0, DIAGNOSTIC_CAPACITY)
    if (!deferCommit) setDiagnostics(diagnosticsRef.current)
  }, [])

  const commitTelemetry = useCallback(() => {
    lastCommit.current = performance.now()
    setLatestSample(telemetryRef.current.latest())
    setFields(telemetryRef.current.fields())
    setDiagnostics(diagnosticsRef.current)
    setDataVersion((version) => version + 1)
  }, [])

  const scheduleTelemetryCommit = useCallback(() => {
    if (commitTimer.current !== null || commitFrame.current !== null) return
    const remaining = Math.max(
      0,
      CHART_COMMIT_INTERVAL_MS - (performance.now() - lastCommit.current),
    )
    commitTimer.current = window.setTimeout(() => {
      commitTimer.current = null
      commitFrame.current = window.requestAnimationFrame(() => {
        commitFrame.current = null
        commitTelemetry()
      })
    }, remaining)
  }, [commitTelemetry])

  const clearTelemetry = useCallback(() => {
    telemetryRef.current.clear()
    setLatestSample(null)
    setFields([])
    setDataVersion((version) => version + 1)
  }, [])

  const connect = useCallback(async (reconnecting = true) => {
    setActionError('')
    await clientRef.current?.connect(reconnecting)
  }, [])

  const invoke = useCallback(async (
    action: ActionName,
    params: Record<string, unknown> = {},
  ): Promise<ReplyMessage> => {
    const client = clientRef.current
    if (!client) throw new Error('WebSocket client is unavailable')
    const tracksGlobalBusy = action !== 'setDigitalStimulus' &&
      action !== 'resetDigitalStimulus'
    if (tracksGlobalBusy) setBusyAction(action)
    setActionError('')
    pushDiagnostic('command', `发送 · ${action}`, JSON.stringify(params), params)
    try {
      const reply = await client.request(action, params)
      pushDiagnostic(
        reply.ok ? 'command' : 'error',
        `${reply.ok ? '完成' : '拒绝'} · ${action}`,
        reply.message || reply.code || 'ok',
        reply,
      )
      if (!reply.ok) setActionError(reply.message || reply.code)
      return reply
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error)
      setActionError(message)
      pushDiagnostic('error', `失败 · ${action}`, message)
      throw error
    } finally {
      if (tracksGlobalBusy) setBusyAction(null)
    }
  }, [pushDiagnostic])

  useEffect(() => {
    const client = new HwtestClient(HWTEST_WS_URL)
    clientRef.current = client
    const unsubscribe = client.subscribe((event) => {
      if (event.type === 'connection') {
        setConnectionState(event.state)
        setConnectionDetail(event.detail ?? '')
        pushDiagnostic(
          event.state === 'error' ? 'error' : 'link',
          `WebSocket · ${connectionStateLabel(event.state)}`,
          event.detail ?? HWTEST_WS_URL,
        )
        return
      }

      const message = event.message
      if (message.type === 'snapshot') {
        const nextConfigId = message.snapshot.descriptor.configId
        if (nextConfigId && nextConfigId !== descriptorConfigId.current) {
          if (descriptorConfigId.current) clearTelemetry()
          descriptorConfigId.current = nextConfigId
        }
        const nextSnapshot = { ...EMPTY_SNAPSHOT, ...message.snapshot }
        snapshotRef.current = nextSnapshot
        setSnapshot(nextSnapshot)
        pushDiagnostic(
          'snapshot',
          `状态 · ${phaseLabel(message.snapshot.phase)}`,
          message.snapshot.progressStep || `seq ${message.seq}`,
          message,
        )
      } else if (message.type === 'sample') {
        telemetryRef.current.append(message.sample)
        pushDiagnostic(
          'sample',
          `样本 · #${message.sample.cycleIndex}`,
          `${message.sample.channelId} · seq ${message.seq}`,
          message,
          true,
        )
        scheduleTelemetryCommit()
      } else if (message.type === 'hello') {
        setTestConfigsReady(false)
        pushDiagnostic(
          'link',
          `已连接 ${message.server}`,
          `协议 v${message.protocolVersion}`,
          message,
        )
        void client.request('testConfigs').then((reply) => {
          setTestConfigsReady(true)
          if (!reply.ok) {
            setActionError(reply.message || reply.code)
            pushDiagnostic(
              'error',
              '测试配置目录不可用',
              reply.message || reply.code,
              reply,
            )
            return
          }
          try {
            const catalog = parseTestConfigCatalog(reply.data)
            const initialConfig = selectInitialTestConfig(catalog)
            setTestConfigs(catalog.configs)
            setSelectedConfigId(initialConfig?.configId ?? '')
            if (!initialConfig) {
              setActionError('没有可加载的测试配置')
              pushDiagnostic('error', '测试配置目录为空', '没有发现可用测试配置', catalog)
              return
            }
            if (snapshotRef.current.phase !== 'empty' || autoLoadInFlight.current) return

            autoLoadInFlight.current = true
            const action: ActionName = initialConfig.configId === catalog.selectedConfigId
              ? 'load'
              : 'selectTest'
            const params = action === 'selectTest'
              ? { configId: initialConfig.configId }
              : {}
            void invoke(action, params)
              .catch(() => undefined)
              .finally(() => {
                autoLoadInFlight.current = false
              })
          } catch (error) {
            const detail = error instanceof Error ? error.message : String(error)
            setActionError(detail)
            pushDiagnostic('error', '测试配置目录格式错误', detail, reply)
          }
        }).catch((error) => {
          const detail = error instanceof Error ? error.message : String(error)
          setTestConfigsReady(true)
          setActionError(detail)
          pushDiagnostic('error', '读取测试配置目录失败', detail)
        })
      }
    })

    void client.connect().catch(() => undefined)
    return () => {
      unsubscribe()
      client.close()
      if (commitTimer.current !== null) window.clearTimeout(commitTimer.current)
      if (commitFrame.current !== null) window.cancelAnimationFrame(commitFrame.current)
      autoLoadInFlight.current = false
    }
  }, [clearTelemetry, invoke, pushDiagnostic, scheduleTelemetryCommit])

  const start = useCallback(async (options: TestRunOptions) => {
    clearTelemetry()
    return invoke('start', { ...options })
  }, [clearTelemetry, invoke])

  const applyDigitalStimulusReply = useCallback((
    action: 'setDigitalStimulus' | 'resetDigitalStimulus',
    reply: ReplyMessage,
    requestConfigId: string,
  ): DigitalStimulusSnapshot => {
    const digitalStimulus = reply.data.digitalStimulus
    const currentSnapshot = snapshotRef.current
    if (digitalStimulus &&
        currentSnapshot.descriptor.configId === requestConfigId &&
        digitalStimulus.revision >= currentSnapshot.digitalStimulus.revision) {
      const nextSnapshot = { ...currentSnapshot, digitalStimulus }
      snapshotRef.current = nextSnapshot
      setSnapshot(nextSnapshot)
    }

    if (!reply.ok) {
      throw new Error(reply.message || reply.code || `${action} was rejected`)
    }
    if (digitalStimulus) return digitalStimulus

    const message = `${action} reply is missing data.digitalStimulus`
    setActionError(message)
    pushDiagnostic('error', `失败 · ${action}`, message, reply)
    throw new Error(message)
  }, [pushDiagnostic])

  const setDigitalStimulus = useCallback(async (
    switchId: string,
    active: boolean,
    expectedRevision: number,
  ) => {
    const requestConfigId = snapshotRef.current.descriptor.configId
    const reply = await invoke('setDigitalStimulus', {
      switchId,
      active,
      expectedRevision,
    })
    return applyDigitalStimulusReply('setDigitalStimulus', reply, requestConfigId)
  }, [applyDigitalStimulusReply, invoke])

  const resetDigitalStimulus = useCallback(async () => {
    const requestConfigId = snapshotRef.current.descriptor.configId
    const reply = await invoke('resetDigitalStimulus')
    return applyDigitalStimulusReply('resetDigitalStimulus', reply, requestConfigId)
  }, [applyDigitalStimulusReply, invoke])

  const value = useMemo<SessionContextValue>(() => ({
    wsUrl: HWTEST_WS_URL,
    connectionState,
    connectionDetail,
    snapshot,
    testConfigs,
    testConfigsReady,
    selectedConfigId,
    latestSample,
    telemetry: telemetryRef.current,
    fields,
    dataVersion,
    diagnostics,
    busyAction,
    actionError,
    connect,
    invoke,
    start,
    setDigitalStimulus,
    resetDigitalStimulus,
    clearTelemetry,
  }), [
    actionError,
    busyAction,
    clearTelemetry,
    connect,
    connectionDetail,
    connectionState,
    dataVersion,
    diagnostics,
    fields,
    invoke,
    latestSample,
    resetDigitalStimulus,
    selectedConfigId,
    setDigitalStimulus,
    snapshot,
    start,
    testConfigs,
    testConfigsReady,
  ])

  return <SessionContext.Provider value={value}>{children}</SessionContext.Provider>
}

export function useSession(): SessionContextValue {
  const context = useContext(SessionContext)
  if (!context) throw new Error('useSession must be used inside SessionProvider')
  return context
}

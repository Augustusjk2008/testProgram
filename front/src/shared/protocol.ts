export type RunMode = 'single' | 'pc_periodic' | 'device_stream'

export interface TestRunOptions {
  mode: RunMode
  intervalMs: number
  maxCycles: number
  saveData: boolean
  algorithmParameters: Record<string, unknown>
}

export interface TestMeasurementDescriptor {
  id: string
  label: string
  unit: string
  primary: boolean
}

export type RunParameterKind = 'integer' | 'number' | 'boolean' | 'choice'

export interface TestRunParameterChoice {
  value: string | number | boolean
  label: string
}

export interface TestRunParameterVisibility {
  parameter: string
  equals: string | number | boolean
}

export interface TestRunParameterDescriptor {
  id: string
  label: string
  description: string
  kind: RunParameterKind
  unit: string
  required: boolean
  minimum?: number
  maximum?: number
  minimumExclusive: boolean
  maximumExclusive: boolean
  choices: TestRunParameterChoice[]
  visibleWhen?: TestRunParameterVisibility
  /** Defaults to true for descriptors that predate this opt-out. */
  persistValues?: boolean
}

export type BoardTestKind = 'do_write' | 'helm_board_test'

export type BoardTestMode = 'automatic' | 'manual'

/**
 * The backend owns the fields inside each point.  Keeping the point payload
 * opaque here lets the board-test page remain compatible with future fields
 * while still rejecting non-object entries before rendering.
 */
export type BoardTestPoint = Record<string, unknown>

export interface BoardTestResult {
  schema: 'hwtest.mbddf-board-test-result'
  version: 1
  kind: BoardTestKind
  mode: BoardTestMode
  completedPoints: number
  totalPoints: number
  summary: BoardTestPoint
  doSteps: BoardTestPoint[]
  pwmPoints: BoardTestPoint[]
  directionPoints: BoardTestPoint[]
  feedbackPoints: BoardTestPoint[]
  manualResponse?: BoardTestPoint
}

function isBoardTestObject(value: unknown): value is BoardTestPoint {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}

function boardTestPoints(value: unknown): BoardTestPoint[] {
  if (!Array.isArray(value)) return []
  return value.filter(isBoardTestObject).map((point) => ({ ...point }))
}

function boardTestCount(value: unknown): number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= 0
    ? value
    : 0
}

/**
 * Converts the versioned result held in snapshot.rawData into a display-safe
 * view model.  Unknown fields are deliberately discarded at the top level;
 * point records retain their backend-owned fields for forward compatibility.
 */
export function parseBoardTestResult(value: unknown): BoardTestResult | null {
  if (!isBoardTestObject(value) ||
      value.schema !== 'hwtest.mbddf-board-test-result' ||
      value.version !== 1 ||
      (value.kind !== 'do_write' && value.kind !== 'helm_board_test') ||
      (value.mode !== 'automatic' && value.mode !== 'manual')) {
    return null
  }

  const manualResponse = isBoardTestObject(value.manualResponse)
    ? { ...value.manualResponse }
    : undefined
  return {
    schema: 'hwtest.mbddf-board-test-result',
    version: 1,
    kind: value.kind,
    mode: value.mode,
    completedPoints: boardTestCount(value.completedPoints),
    totalPoints: boardTestCount(value.totalPoints),
    summary: isBoardTestObject(value.summary) ? { ...value.summary } : {},
    doSteps: boardTestPoints(value.doSteps),
    pwmPoints: boardTestPoints(value.pwmPoints),
    directionPoints: boardTestPoints(value.directionPoints),
    feedbackPoints: boardTestPoints(value.feedbackPoints),
    ...(manualResponse === undefined ? {} : { manualResponse }),
  }
}

export interface PostRunAnalysisCapability {
  supported: boolean
  analyzerId: string
  schemaVersion: string
}

export type AnalysisState =
  | 'none'
  | 'capturing'
  | 'queued'
  | 'validating'
  | 'preprocessing'
  | 'calculating'
  | 'persisting'
  | 'completed'
  | 'partial'
  | 'unavailable'
  | 'failed'
  | 'cancelled'

export type AnalysisChannelStatus =
  | 'not_applicable'
  | 'completed'
  | 'partial'
  | 'unavailable'

export type AnalysisChannel = 0 | 1 | 2 | 3

export interface AnalysisIdentity {
  taskId: string
  analysisGeneration: number
}

export interface AnalysisMetric {
  key: string
  label: string
  unit: string
  status: string
  value: number | null
  detail: string
}

export interface AnalysisChannelSummary {
  channel: AnalysisChannel
  enabled: boolean
  status: AnalysisChannelStatus
  warnings: string[]
  omittedWarningCount?: number
  commonMetrics: AnalysisMetric[]
  waveformMetrics: AnalysisMetric[]
  bodeAvailable: boolean
  bodePointCount: number
  reasonCode: string
  message: string
}

export interface AnalysisSnapshot extends PostRunAnalysisCapability, AnalysisIdentity {
  state: AnalysisState
  progress: number
  stage: string
  message: string
  reasonCode: string
  resultFilePath: string
  diagnosticInputFilePath: string
  sourceSummary: Record<string, unknown>
  channelSummaries: AnalysisChannelSummary[]
}

export interface BodeProjection {
  frequencyHz: number[]
  magnitudeDb: Array<number | null>
  phaseDeg: Array<number | null>
  pointStatus: string[]
}

export interface AnalysisResult {
  channelSummary: AnalysisChannelSummary
  bode: BodeProjection
}

export interface TestDescriptor {
  configId: string
  productModel: string
  productName: string
  configVersion: string
  stepId: string
  testItemId: string
  algorithmId: string
  title: string
  description: string
  stoppable: boolean
  supportedRunModes: RunMode[]
  measurements: TestMeasurementDescriptor[]
  runParameterSchemaVersion: string
  runParameters: TestRunParameterDescriptor[]
  runParameterDefaults: Record<string, unknown>
  postRunAnalysis: PostRunAnalysisCapability
}

export interface TestConfigOption {
  configId: string
  title: string
  description: string
  algorithmId: string
}

export interface TestConfigCatalog {
  selectedConfigId: string
  configs: TestConfigOption[]
}

/** Editable-document directory exposed by the configuration API. */
export type ConfigRevision = string

export interface ConfigCatalogItem {
  documentId: string
  configId: string
  title: string
  enabled: boolean
  order: number
  valid: boolean
  message: string
}

export interface ConfigCatalog {
  revision: ConfigRevision
  items: ConfigCatalogItem[]
}

export type ConfigCatalogRequest = Record<string, never>

export interface ConfigDocumentRequest {
  documentId: string
}

export interface ConfigDocument {
  documentId: string
  kind: string
  revision: ConfigRevision
  value: Record<string, unknown>
  schema?: Record<string, unknown>
}

export interface SaveConfigRequest {
  documentId: string
  expectedRevision: ConfigRevision
  value: Record<string, unknown>
}

export type HardwareOptionsState = 'available' | 'unavailable' | 'error'

export interface HardwareOptionDevice {
  deviceName: string
  deviceId: string
  model: string
  serialNumber: string
  supportedModules: string[]
}

export interface HardwareOptions {
  state: HardwareOptionsState
  message: string
  allowManualEntry: boolean
  devices: HardwareOptionDevice[]
}

export interface DigitalSwitchDescriptor {
  switchId: string
  dutBit: number
  label: string
  activeLevel: 'High' | 'Low'
}

export interface DigitalStimulusSnapshot {
  available: boolean
  configured: boolean
  switches: DigitalSwitchDescriptor[]
  appliedMask: number
  revision: number
  lastWriteTimestampUs: number
  settlingMs: number
  errorCode: string
  message: string
}

export type ActionName =
  | 'load'
  | 'testConfigs'
  | 'configCatalog'
  | 'hardwareOptions'
  | 'configDocument'
  | 'saveConfig'
  | 'selectTest'
  | 'snapshot'
  | 'controls'
  | 'ports'
  | 'selectControl'
  | 'selectSerialPort'
  | 'selectAuxiliarySerialPort'
  | 'prepare'
  | 'start'
  | 'pause'
  | 'resume'
  | 'setDigitalStimulus'
  | 'resetDigitalStimulus'
  | 'setTelemetryDelivery'
  | 'analysisResult'
  | 'stop'
  | 'disconnect'
  | 'quit'

export interface ApplicationSnapshot {
  phase: string
  testState: string
  controlResourceId: string
  providerId: string
  serialPortName: string
  auxiliarySerialPortName: string
  taskId: string
  stepId: string
  testItemId: string
  algorithmId: string
  progress: number
  progressStep: string
  hasResult: boolean
  verdict: string
  errorCode: string
  message: string
  attempts: number
  rawData: Record<string, unknown>
  effectiveRunParameters: Record<string, unknown>
  runMode: RunMode
  intervalMs: number
  maxCycles: number
  cycleIndex: number
  sampleCount: number
  dataSaveEnabled: boolean
  dataFilePath: string
  dataSaveError: string
  descriptor: TestDescriptor
  digitalStimulus: DigitalStimulusSnapshot
  analysis: AnalysisSnapshot
}

export interface ApplicationSample {
  taskId: string
  stepId: string
  channelId: string
  timestampUs: number
  cycleIndex: number
  values: Record<string, unknown>
  tags: Record<string, unknown>
  streamElapsedUs?: number
}

export interface TelemetryBatchCapability {
  version: 1
  maxSamples: number
  maxBytes: number
  maxLatencyMs: number
  snapshotIntervalMs: number
}

export interface ServerCapabilities {
  telemetryBatch?: TelemetryBatchCapability
}

export interface HelloMessage {
  v: 1
  type: 'hello'
  server: string
  protocolVersion: number
  capabilities?: ServerCapabilities
}

export interface SnapshotMessage {
  v: 1
  type: 'snapshot'
  seq: number
  snapshot: ApplicationSnapshot
}

export interface SampleMessage {
  v: 1
  type: 'sample'
  seq: number
  sample: ApplicationSample
}

export interface SampleBatchMessage {
  v: 1
  type: 'sampleBatch'
  firstSeq: number
  lastSeq: number
  samples: ApplicationSample[]
}

export type ReplyData = Record<string, unknown> & {
  configCatalog?: ConfigCatalog
  configDocument?: ConfigDocument
  saveConfig?: ConfigDocument
  digitalStimulus?: DigitalStimulusSnapshot
  analysisResult?: AnalysisResult
}

export interface ReplyMessage {
  v: 1
  type: 'reply'
  id: string
  ok: boolean
  code: string
  message: string
  data: ReplyData
}

export type ServerMessage = HelloMessage | SnapshotMessage | SampleMessage | SampleBatchMessage | ReplyMessage

export interface ControlResource {
  resourceId: string
  providerId: string
}

export interface SerialPortInfo {
  portName: string
  description: string
  manufacturer: string
  serialNumber: string
  systemLocation: string
}

export const EMPTY_TEST_DESCRIPTOR: TestDescriptor = {
  configId: '',
  productModel: '',
  productName: '',
  configVersion: '',
  stepId: '',
  testItemId: '',
  algorithmId: '',
  title: '',
  description: '',
  stoppable: true,
  supportedRunModes: [],
  measurements: [],
  runParameterSchemaVersion: '',
  runParameters: [],
  runParameterDefaults: {},
  postRunAnalysis: {
    supported: false,
    analyzerId: '',
    schemaVersion: '',
  },
}

export const EMPTY_ANALYSIS: AnalysisSnapshot = {
  supported: false,
  analyzerId: '',
  schemaVersion: '',
  taskId: '',
  analysisGeneration: 0,
  state: 'none',
  progress: 0,
  stage: '',
  message: '',
  reasonCode: '',
  resultFilePath: '',
  diagnosticInputFilePath: '',
  sourceSummary: {},
  channelSummaries: [],
}

export const EMPTY_DIGITAL_STIMULUS: DigitalStimulusSnapshot = {
  available: false,
  configured: false,
  switches: [],
  appliedMask: 0,
  revision: 0,
  lastWriteTimestampUs: 0,
  settlingMs: 0,
  errorCode: '',
  message: '',
}

export const EMPTY_SNAPSHOT: ApplicationSnapshot = {
  phase: 'empty',
  testState: 'Uninitialized',
  controlResourceId: '',
  providerId: '',
  serialPortName: '',
  auxiliarySerialPortName: '',
  taskId: '',
  stepId: '',
  testItemId: '',
  algorithmId: '',
  progress: 0,
  progressStep: '',
  hasResult: false,
  verdict: '',
  errorCode: '',
  message: '',
  attempts: 0,
  rawData: {},
  effectiveRunParameters: {},
  runMode: 'single',
  intervalMs: 1000,
  maxCycles: 1,
  cycleIndex: 0,
  sampleCount: 0,
  dataSaveEnabled: false,
  dataFilePath: '',
  dataSaveError: '',
  descriptor: EMPTY_TEST_DESCRIPTOR,
  digitalStimulus: EMPTY_DIGITAL_STIMULUS,
  analysis: EMPTY_ANALYSIS,
}

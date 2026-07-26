export type RunMode = 'single' | 'pc_periodic' | 'device_stream'

export interface TestRunOptions {
  mode: RunMode
  intervalMs: number
  maxCycles: number
}

export interface TestMeasurementDescriptor {
  id: string
  label: string
  unit: string
  primary: boolean
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
  supportedRunModes: RunMode[]
  measurements: TestMeasurementDescriptor[]
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
  | 'selectTest'
  | 'snapshot'
  | 'controls'
  | 'ports'
  | 'selectControl'
  | 'selectSerialPort'
  | 'prepare'
  | 'start'
  | 'pause'
  | 'resume'
  | 'setDigitalStimulus'
  | 'resetDigitalStimulus'
  | 'stop'
  | 'disconnect'
  | 'quit'

export interface ApplicationSnapshot {
  phase: string
  testState: string
  controlResourceId: string
  providerId: string
  serialPortName: string
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
  runMode: RunMode
  intervalMs: number
  maxCycles: number
  cycleIndex: number
  sampleCount: number
  descriptor: TestDescriptor
  digitalStimulus: DigitalStimulusSnapshot
}

export interface ApplicationSample {
  taskId: string
  stepId: string
  channelId: string
  timestampUs: number
  cycleIndex: number
  values: Record<string, unknown>
  tags: Record<string, unknown>
}

export interface HelloMessage {
  v: 1
  type: 'hello'
  server: string
  protocolVersion: number
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

export type ReplyData = Record<string, unknown> & {
  digitalStimulus?: DigitalStimulusSnapshot
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

export type ServerMessage = HelloMessage | SnapshotMessage | SampleMessage | ReplyMessage

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
  supportedRunModes: [],
  measurements: [],
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
  runMode: 'single',
  intervalMs: 1000,
  maxCycles: 1,
  cycleIndex: 0,
  sampleCount: 0,
  descriptor: EMPTY_TEST_DESCRIPTOR,
  digitalStimulus: EMPTY_DIGITAL_STIMULUS,
}

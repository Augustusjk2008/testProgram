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

export type ActionName =
  | 'load'
  | 'snapshot'
  | 'controls'
  | 'ports'
  | 'selectControl'
  | 'selectSerialPort'
  | 'prepare'
  | 'start'
  | 'pause'
  | 'resume'
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

export interface ReplyMessage {
  v: 1
  type: 'reply'
  id: string
  ok: boolean
  code: string
  message: string
  data: Record<string, unknown>
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
}

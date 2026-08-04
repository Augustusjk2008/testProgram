import type { TestRunOptions } from '../../shared/protocol'

function continuousSaveDestination(options: TestRunOptions): Pick<
  TestRunOptions,
  'dataDirectory' | 'dataFileName'
> {
  if (!options.saveData) return {}
  return {
    ...(options.dataDirectory === undefined ? {} : { dataDirectory: options.dataDirectory }),
    ...(options.dataFileName === undefined ? {} : { dataFileName: options.dataFileName }),
  }
}

export function validatePcPeriodicOptions(intervalMs: number, maxCycles: number): string {
  if (!Number.isInteger(intervalMs) || intervalMs < 0 || intervalMs > 3_600_000) {
    return '周期需为 0–3,600,000 ms 的整数；0 表示上一轮完成后立即开始下一轮'
  }
  if (!Number.isInteger(maxCycles) || maxCycles < 0 || maxCycles > 1_000_000_000) {
    return '轮数需为 0–1,000,000,000；0 表示持续运行'
  }
  return ''
}

export function normalizeRunOptionsForStart(options: TestRunOptions): TestRunOptions {
  const { dataDirectory: _dataDirectory, dataFileName: _dataFileName, ...baseOptions } = options
  if (options.mode === 'single') {
    return {
      mode: 'single', intervalMs: 1000, maxCycles: 1, saveData: false,
      algorithmParameters: options.algorithmParameters,
    }
  }
  if (options.mode === 'pc_periodic') {
    return { ...baseOptions, ...continuousSaveDestination(options) }
  }
  return {
    ...baseOptions,
    mode: 'device_stream',
    intervalMs: 1000,
    maxCycles: 1,
    ...continuousSaveDestination(options),
  }
}

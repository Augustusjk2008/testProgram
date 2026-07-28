import type { TestRunOptions } from '../../shared/protocol'

export function normalizeRunOptionsForStart(options: TestRunOptions): TestRunOptions {
  if (options.mode === 'single') {
    return {
      mode: 'single', intervalMs: 1000, maxCycles: 1, saveData: false,
      algorithmParameters: options.algorithmParameters,
    }
  }
  if (options.mode === 'pc_periodic') return options
  return {
    mode: 'device_stream', intervalMs: 1000, maxCycles: 1, saveData: options.saveData,
    algorithmParameters: options.algorithmParameters,
  }
}

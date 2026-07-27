import type { TestRunOptions } from '../../shared/protocol'

export function normalizeRunOptionsForStart(options: TestRunOptions): TestRunOptions {
  if (options.mode === 'single') {
    return { mode: 'single', intervalMs: 1000, maxCycles: 1, saveData: false }
  }
  if (options.mode === 'pc_periodic') return options
  return { ...options, saveData: false }
}

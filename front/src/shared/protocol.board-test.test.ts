import { describe, expect, it } from 'vitest'

import { parseBoardTestResult } from './protocol'

describe('board-test result projection', () => {
  it('normalizes a complete automatic result without trusting unknown point entries', () => {
    expect(parseBoardTestResult({
      schema: 'hwtest.mbddf-board-test-result',
      version: 1,
      kind: 'helm_board_test',
      mode: 'automatic',
      completedPoints: 36,
      totalPoints: 88,
      summary: { maximumError: 0.8, worstPoint: 'PWM 3 / 90%' },
      pwmPoints: [
        { channel: 3, command: 90, measured: 89.2, error: -0.8, tolerance: 1, pass: true },
        'not a point',
      ],
      feedbackPoints: [{ channel: 1, command: 5, measured: 4.98, error: -0.02, tolerance: 0.05 }],
      directionPoints: [{ channel: 1, direction: 0, measured: 0.1, pass: true }],
      doSteps: [{ step: 1, requestedMask: '0x0018', do1: 0, do2: 0, pass: true }],
      ignored: { should: 'not reach the view model' },
    })).toEqual({
      schema: 'hwtest.mbddf-board-test-result',
      version: 1,
      kind: 'helm_board_test',
      mode: 'automatic',
      completedPoints: 36,
      totalPoints: 88,
      summary: { maximumError: 0.8, worstPoint: 'PWM 3 / 90%' },
      doSteps: [{ step: 1, requestedMask: '0x0018', do1: 0, do2: 0, pass: true }],
      pwmPoints: [{ channel: 3, command: 90, measured: 89.2, error: -0.8, tolerance: 1, pass: true }],
      directionPoints: [{ channel: 1, direction: 0, measured: 0.1, pass: true }],
      feedbackPoints: [{ channel: 1, command: 5, measured: 4.98, error: -0.02, tolerance: 0.05 }],
    })
  })

  it('rejects an unrecognized contract and safely defaults optional collections', () => {
    expect(parseBoardTestResult({ schema: 'other', version: 1 })).toBeNull()
    expect(parseBoardTestResult({
      schema: 'hwtest.mbddf-board-test-result',
      version: 1,
      kind: 'do_write',
      mode: 'manual',
      completedPoints: -1,
      totalPoints: 'five',
      summary: null,
      doSteps: 'not an array',
      manualResponse: { status: 0, applied_state: '0x0018' },
    })).toEqual({
      schema: 'hwtest.mbddf-board-test-result',
      version: 1,
      kind: 'do_write',
      mode: 'manual',
      completedPoints: 0,
      totalPoints: 0,
      summary: {},
      doSteps: [],
      pwmPoints: [],
      directionPoints: [],
      feedbackPoints: [],
      manualResponse: { status: 0, applied_state: '0x0018' },
    })
  })
})

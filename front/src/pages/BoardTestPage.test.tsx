import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'

import { EMPTY_SNAPSHOT } from '../shared/protocol'

const sessionHooks = vi.hoisted(() => ({ useSession: vi.fn() }))

vi.mock('../features/session/SessionProvider', () => sessionHooks)

import { BoardTestPage } from './BoardTestPage'

describe('BoardTestPage', () => {
  it('renders the one-shot manual response table without unrelated explanations', () => {
    sessionHooks.useSession.mockReturnValue({
      snapshot: {
        ...EMPTY_SNAPSHOT,
        algorithmId: 'mbddf.helm_board_test',
        verdict: 'pass',
        rawData: {
          boardTest: {
            schema: 'hwtest.mbddf-board-test-result',
            version: 1,
            kind: 'helm_board_test',
            mode: 'manual',
            completedPoints: 1,
            totalPoints: 1,
            summary: { maximumError: 0 },
            manualResponse: { status: 0, helm_AD_value: 2.5 },
          },
        },
      },
    })

    const markup = renderToStaticMarkup(<BoardTestPage />)

    expect(markup).toContain('一次响应表')
    expect(markup).toContain('helm_AD_value')
    expect(markup).not.toContain('DUT内部自检')
  })

  it('renders automatic DO, direction, PWM, and feedback projections together', () => {
    sessionHooks.useSession.mockReturnValue({
      snapshot: {
        ...EMPTY_SNAPSHOT,
        algorithmId: 'mbddf.helm_board_test',
        verdict: 'fail',
        rawData: {
          boardTest: {
            schema: 'hwtest.mbddf-board-test-result',
            version: 1,
            kind: 'helm_board_test',
            mode: 'automatic',
            completedPoints: 3,
            totalPoints: 88,
            summary: { maximumError: 1.2, worstPoint: 'PWM 2 / 90%' },
            doSteps: [{ step: 1, requestedMask: '0x0018', do1: 0, do2: 0, pass: true }],
            directionPoints: [{ channel: 1, direction: 0, measured: 0.1, pass: true }],
            pwmPoints: [{ channel: 2, command: 90, measured: 88.8, error: -1.2, tolerance: 1, pass: false }],
            feedbackPoints: [{ channel: 1, command: 5, measured: 4.98, error: -0.02, tolerance: 0.05, pass: true }],
          },
        },
      },
    })

    const markup = renderToStaticMarkup(<BoardTestPage />)

    expect(markup).toContain('DO 步骤矩阵')
    expect(markup).toContain('方向 0 / 1 矩阵')
    expect(markup).toContain('PWM 指令、实测、误差与容差')
    expect(markup).toContain('反馈电压指令、读回、误差与容差')
    expect(markup).toContain('最差点')
  })

  it('projects the backend point field names used by the automatic board-test result', () => {
    sessionHooks.useSession.mockReturnValue({
      snapshot: {
        ...EMPTY_SNAPSHOT,
        algorithmId: 'mbddf.helm_board_test',
        verdict: 'pass',
        rawData: {
          boardTest: {
            schema: 'hwtest.mbddf-board-test-result',
            version: 1,
            kind: 'helm_board_test',
            mode: 'automatic',
            completedPoints: 3,
            totalPoints: 85,
            summary: {
              maxDutyErrorPercentagePoints: 0.7,
              maxFeedbackErrorVolts: 0.02,
              worstPoint: 'PWM1 @ 50%',
            },
            doSteps: [{
              index: 0,
              commandMask: 0x0018,
              appliedMask: 0x0018,
              measuredTxEnable: true,
              measuredAttenuator: false,
              passed: true,
            }],
            directionPoints: [
              { expectedMask: 0, measuredMask: 0, measuredVoltages: [0.1, 1.5, 0.1, 0.1], passed: false },
              { expectedMask: 1, measuredMask: 1, measuredVoltages: [4.2, 0.1, 0.1, 0.1], passed: true },
            ],
            pwmPoints: [{
              channel: 1,
              commandPercent: 50,
              measuredPercent: 49.3,
              errorPercentagePoints: 0.7,
              tolerancePercentagePoints: 1,
              passed: true,
            }],
            feedbackPoints: [{
              channel: 1,
              commandV: 5,
              expectedV: 4.99985,
              measuredV: 4.98,
              errorV: 0.02,
              toleranceV: 0.05,
              passed: true,
            }],
          },
        },
      },
    })

    const markup = renderToStaticMarkup(<BoardTestPage />)
    const channel1Row = markup.slice(
      markup.indexOf('<strong>通道 1</strong>'),
      markup.indexOf('<strong>通道 2</strong>'),
    )
    const channel2Row = markup.slice(
      markup.indexOf('<strong>通道 2</strong>'),
      markup.indexOf('<strong>通道 3</strong>'),
    )

    expect(markup).toContain('0x0018')
    expect(markup).toContain('<td>false</td><td>true</td>')
    expect(markup).toContain('PWM 最大误差')
    expect(markup).toContain('0.7 pp')
    expect(markup).toContain('反馈最大误差')
    expect(markup).toContain('0.02 V')
    expect(channel1Row).toContain('<strong>通道 1</strong><div class="board-direction-cell"><span class="board-status board-status--pass">')
    expect(channel2Row).toContain('<strong>通道 2</strong><div class="board-direction-cell"><span class="board-status board-status--fail">')
    expect(markup).toContain('实测 4.2 V')
    expect(markup).toContain('容差 ±1 %')
    expect(markup).toContain('指令 5 V')
    expect(markup).toContain('实测 4.98 V')
    expect(markup).toContain('容差 ±0.05 V')
  })
})

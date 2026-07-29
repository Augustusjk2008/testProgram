import type { ActionName } from '../../shared/protocol'

export function tracksGlobalBusyAction(action: ActionName): boolean {
  return action !== 'setDigitalStimulus' &&
    action !== 'resetDigitalStimulus' &&
    action !== 'analysisResult'
}

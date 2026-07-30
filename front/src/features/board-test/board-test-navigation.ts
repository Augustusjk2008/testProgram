const BOARD_TEST_ALGORITHMS = new Set([
  'mbddf.do_write',
  'mbddf.helm_board_test',
])

export function isBoardTestAlgorithm(algorithmId: string): boolean {
  return BOARD_TEST_ALGORITHMS.has(algorithmId)
}

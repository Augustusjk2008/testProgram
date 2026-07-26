import type { TestConfigCatalog, TestConfigOption } from '../../shared/protocol'

export function selectInitialTestConfig(
  catalog: TestConfigCatalog,
): TestConfigOption | null {
  return catalog.configs.find(({ configId }) => configId === catalog.selectedConfigId)
    ?? catalog.configs[0]
    ?? null
}

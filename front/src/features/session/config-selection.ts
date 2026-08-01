import type { TestConfigCatalog, TestConfigOption } from '../../shared/protocol'

export function selectInitialTestConfig(
  catalog: TestConfigCatalog,
  disabledConfigIds: ReadonlySet<string> = new Set(),
): TestConfigOption | null {
  const enabledConfigs = catalog.configs.filter(({ configId }) => !disabledConfigIds.has(configId))
  return enabledConfigs.find(({ configId }) => configId === catalog.selectedConfigId)
    ?? enabledConfigs[0]
    ?? null
}

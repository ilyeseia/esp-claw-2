import { createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput } from '../components/ui/FormField';
import { Switch } from '../components/ui/Switch';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';

type McpForm = {
  mcp_enabled: string;
  mcp_hostname: string;
  mcp_instance_name: string;
  mcp_endpoint: string;
  mcp_server_port: string;
  mcp_ctrl_port: string;
};

const isTrue = (value: string) => value === 'true' || value === '1';
const boolStr = (value: boolean) => (value ? 'true' : 'false');

export const McpPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<McpForm>({
    tab: 'mcp',
    groups: ['mcp'],
    toForm: (config: Partial<AppConfig>) => ({
      mcp_enabled: config.mcp_enabled ?? 'true',
      mcp_hostname: config.mcp_hostname ?? 'esp-claw',
      mcp_instance_name: config.mcp_instance_name ?? 'ESP-Claw',
      mcp_endpoint: config.mcp_endpoint ?? 'mcp',
      mcp_server_port: config.mcp_server_port ?? '18791',
      mcp_ctrl_port: config.mcp_ctrl_port ?? '18792',
    }),
    fromForm: (form) => ({
      mcp_enabled: boolStr(isTrue(form.mcp_enabled)),
      mcp_hostname: form.mcp_hostname.trim(),
      mcp_instance_name: form.mcp_instance_name.trim(),
      mcp_endpoint: form.mcp_endpoint.trim(),
      mcp_server_port: form.mcp_server_port.trim(),
      mcp_ctrl_port: form.mcp_ctrl_port.trim(),
    }),
  });
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const handleSave = async () => {
    await tab.save();
    setConfirmOpen(true);
  };

  return (
    <TabShell>
      <PageHeader title={t('navMcp') as string} />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionMcpServer') as string}>
          <div class="pt-2">
            <Switch
              label={t('mcpEnabled') as string}
              hint={t('mcpEnabledHint') as string}
              checked={isTrue(tab.form.mcp_enabled)}
              onChange={(value) => tab.setForm('mcp_enabled', boolStr(value))}
            />
          </div>
          <div class="grid gap-3 sm:grid-cols-2 pt-3">
            <TextInput
              label={t('mcpHostname') as string}
              hint={t('mcpHostnameHint') as string}
              placeholder="esp-claw"
              value={tab.form.mcp_hostname}
              onInput={(event) => tab.setForm('mcp_hostname', event.currentTarget.value)}
            />
            <TextInput
              label={t('mcpInstanceName') as string}
              placeholder="ESP-Claw"
              value={tab.form.mcp_instance_name}
              onInput={(event) => tab.setForm('mcp_instance_name', event.currentTarget.value)}
            />
            <TextInput
              label={t('mcpEndpoint') as string}
              placeholder={t('mcpEndpointPlaceholder') as string}
              value={tab.form.mcp_endpoint}
              onInput={(event) => tab.setForm('mcp_endpoint', event.currentTarget.value)}
            />
            <TextInput
              type="number"
              label={t('mcpServerPort') as string}
              value={tab.form.mcp_server_port}
              onInput={(event) => tab.setForm('mcp_server_port', event.currentTarget.value)}
            />
            <TextInput
              type="number"
              label={t('mcpCtrlPort') as string}
              hint={t('mcpCtrlPortHint') as string}
              value={tab.form.mcp_ctrl_port}
              onInput={(event) => tab.setForm('mcp_ctrl_port', event.currentTarget.value)}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">{t('mcpNote')}</p>
        </StaticConfigBlock>
      </div>
      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => handleSave().catch(() => undefined)}
        onDiscard={tab.discard}
        note={t('restartHint') as string}
      />
      <RestartConfirmModal
        open={confirmOpen()}
        onClose={() => setConfirmOpen(false)}
        onConfirm={() => {
          setConfirmOpen(false);
          props.onRestartRequest();
        }}
        subtitle={t('restartHint') as string}
      />
    </TabShell>
  );
};

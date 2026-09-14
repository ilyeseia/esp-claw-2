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

type VpnForm = {
  vpn_enabled: string;
  vpn_gateway: string;
  vpn_test_host: string;
  vpn_test_port: string;
};

const isTrue = (value: string) => value === 'true' || value === '1';
const boolStr = (value: boolean) => (value ? 'true' : 'false');

export const VpnPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<VpnForm>({
    tab: 'vpn',
    groups: ['vpn'],
    toForm: (config: Partial<AppConfig>) => ({
      vpn_enabled: config.vpn_enabled ?? 'false',
      vpn_gateway: config.vpn_gateway ?? '',
      vpn_test_host: config.vpn_test_host ?? '',
      vpn_test_port: config.vpn_test_port ?? '80',
    }),
    fromForm: (form) => ({
      vpn_enabled: boolStr(isTrue(form.vpn_enabled)),
      vpn_gateway: form.vpn_gateway.trim(),
      vpn_test_host: form.vpn_test_host.trim(),
      vpn_test_port: form.vpn_test_port.trim(),
    }),
  });
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const handleSave = async () => {
    await tab.save();
    setConfirmOpen(true);
  };

  return (
    <TabShell>
      <PageHeader title={t('navVpn') as string} />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionVpnGateway') as string}>
          <div class="pt-2">
            <Switch
              label={t('vpnEnabled') as string}
              hint={t('vpnEnabledHint') as string}
              checked={isTrue(tab.form.vpn_enabled)}
              onChange={(value) => tab.setForm('vpn_enabled', boolStr(value))}
            />
          </div>
          <div class="grid gap-3 sm:grid-cols-2 pt-3">
            <TextInput
              label={t('vpnGateway') as string}
              hint={t('vpnGatewayHint') as string}
              placeholder={t('vpnGatewayPlaceholder') as string}
              value={tab.form.vpn_gateway}
              onInput={(event) => tab.setForm('vpn_gateway', event.currentTarget.value)}
            />
            <TextInput
              label={t('vpnTestHost') as string}
              hint={t('vpnTestHostHint') as string}
              placeholder={t('vpnTestHostPlaceholder') as string}
              value={tab.form.vpn_test_host}
              onInput={(event) => tab.setForm('vpn_test_host', event.currentTarget.value)}
            />
            <TextInput
              type="number"
              label={t('vpnTestPort') as string}
              value={tab.form.vpn_test_port}
              onInput={(event) => tab.setForm('vpn_test_port', event.currentTarget.value)}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">{t('vpnNote')}</p>
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

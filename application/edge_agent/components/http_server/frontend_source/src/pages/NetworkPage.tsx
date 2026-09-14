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

type NetworkForm = {
  net_use_static: string;
  net_ip: string;
  net_gateway: string;
  net_netmask: string;
  net_dns: string;
  net_dns2: string;
};

const isTrue = (value: string) => value === 'true' || value === '1';
const boolStr = (value: boolean) => (value ? 'true' : 'false');

export const NetworkPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<NetworkForm>({
    tab: 'network',
    groups: ['network'],
    toForm: (config: Partial<AppConfig>) => ({
      net_use_static: config.net_use_static ?? 'false',
      net_ip: config.net_ip ?? '',
      net_gateway: config.net_gateway ?? '',
      net_netmask: config.net_netmask ?? '255.255.255.0',
      net_dns: config.net_dns ?? '',
      net_dns2: config.net_dns2 ?? '',
    }),
    fromForm: (form) => ({
      net_use_static: boolStr(isTrue(form.net_use_static)),
      net_ip: form.net_ip.trim(),
      net_gateway: form.net_gateway.trim(),
      net_netmask: form.net_netmask.trim(),
      net_dns: form.net_dns.trim(),
      net_dns2: form.net_dns2.trim(),
    }),
  });
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const handleSave = async () => {
    await tab.save();
    setConfirmOpen(true);
  };

  return (
    <TabShell>
      <PageHeader title={t('navNetwork') as string} />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionNetworkIp') as string}>
          <div class="pt-2">
            <Switch
              label={t('netUseStatic') as string}
              hint={t('netUseStaticHint') as string}
              checked={isTrue(tab.form.net_use_static)}
              onChange={(value) => tab.setForm('net_use_static', boolStr(value))}
            />
          </div>
          <div class="grid gap-3 sm:grid-cols-2 pt-3">
            <TextInput
              label={t('netIp') as string}
              placeholder="192.168.1.50"
              value={tab.form.net_ip}
              onInput={(event) => tab.setForm('net_ip', event.currentTarget.value)}
            />
            <TextInput
              label={t('netGateway') as string}
              placeholder="192.168.1.1"
              value={tab.form.net_gateway}
              onInput={(event) => tab.setForm('net_gateway', event.currentTarget.value)}
            />
            <TextInput
              label={t('netNetmask') as string}
              value={tab.form.net_netmask}
              onInput={(event) => tab.setForm('net_netmask', event.currentTarget.value)}
            />
            <TextInput
              label={t('netDns') as string}
              placeholder="8.8.8.8"
              value={tab.form.net_dns}
              onInput={(event) => tab.setForm('net_dns', event.currentTarget.value)}
            />
            <TextInput
              label={t('netDns2') as string}
              value={tab.form.net_dns2}
              onInput={(event) => tab.setForm('net_dns2', event.currentTarget.value)}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">{t('netNote')}</p>
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

import { createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput, TextArea } from '../components/ui/FormField';
import { Switch } from '../components/ui/Switch';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';

type SshForm = {
  ssh_enabled: string;
  ssh_host_private_key_der_b64: string;
  ssh_authorized_public_key: string;
};

const isTrue = (value: string) => value === 'true' || value === '1';
const boolStr = (value: boolean) => (value ? 'true' : 'false');

export const SshPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<SshForm>({
    tab: 'ssh',
    groups: ['ssh'],
    toForm: (config: Partial<AppConfig>) => ({
      ssh_enabled: config.ssh_enabled ?? 'false',
      ssh_host_private_key_der_b64: config.ssh_host_private_key_der_b64 ?? '',
      ssh_authorized_public_key: config.ssh_authorized_public_key ?? '',
    }),
    fromForm: (form) => ({
      ssh_enabled: boolStr(isTrue(form.ssh_enabled)),
      ssh_host_private_key_der_b64: form.ssh_host_private_key_der_b64.trim(),
      ssh_authorized_public_key: form.ssh_authorized_public_key.trim(),
    }),
  });
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const handleSave = async () => {
    await tab.save();
    setConfirmOpen(true);
  };

  return (
    <TabShell>
      <PageHeader title={t('navSsh') as string} />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionSsh') as string}>
          <div class="pt-2">
            <Switch
              label={t('sshEnabled') as string}
              hint={t('sshEnabledHint') as string}
              checked={isTrue(tab.form.ssh_enabled)}
              onChange={(value) => tab.setForm('ssh_enabled', boolStr(value))}
            />
          </div>
          <div class="pt-3">
            <TextArea
              label={t('sshHostPrivateKey') as string}
              hint={t('sshHostPrivateKeyHint') as string}
              rows={4}
              value={tab.form.ssh_host_private_key_der_b64}
              onInput={(event) => tab.setForm('ssh_host_private_key_der_b64', event.currentTarget.value)}
            />
          </div>
          <div class="pt-3">
            <TextInput
              label={t('sshAuthorizedKey') as string}
              hint={t('sshAuthorizedKeyHint') as string}
              placeholder={t('sshAuthorizedKeyPlaceholder') as string}
              value={tab.form.ssh_authorized_public_key}
              onInput={(event) => tab.setForm('ssh_authorized_public_key', event.currentTarget.value)}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">{t('sshNote')}</p>
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

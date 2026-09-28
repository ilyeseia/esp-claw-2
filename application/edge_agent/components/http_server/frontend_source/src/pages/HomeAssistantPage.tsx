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

type HomeAssistantForm = {
  ha_enabled: string;
  ha_base_url: string;
  ha_token: string;
};

const isTrue = (value: string) => value === 'true' || value === '1';
const boolStr = (value: boolean) => (value ? 'true' : 'false');

export const HomeAssistantPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<HomeAssistantForm>({
    tab: 'home_assistant',
    groups: ['home_assistant'],
    toForm: (config: Partial<AppConfig>) => ({
      ha_enabled: config.ha_enabled ?? 'false',
      ha_base_url: config.ha_base_url ?? '',
      ha_token: config.ha_token ?? '',
    }),
    fromForm: (form) => ({
      ha_enabled: boolStr(isTrue(form.ha_enabled)),
      ha_base_url: form.ha_base_url.trim(),
      ha_token: form.ha_token.trim(),
    }),
  });
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const handleSave = async () => {
    await tab.save();
    setConfirmOpen(true);
  };

  return (
    <TabShell>
      <PageHeader title={t('navHomeAssistant') as string} />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionHomeAssistant') as string}>
          <div class="pt-2">
            <Switch
              label={t('haEnabled') as string}
              hint={t('haEnabledHint') as string}
              checked={isTrue(tab.form.ha_enabled)}
              onChange={(value) => tab.setForm('ha_enabled', boolStr(value))}
            />
          </div>
          <div class="grid gap-3 sm:grid-cols-2 pt-3">
            <TextInput
              label={t('haBaseUrl') as string}
              hint={t('haBaseUrlHint') as string}
              placeholder="http://homeassistant.local:8123"
              value={tab.form.ha_base_url}
              onInput={(event) => tab.setForm('ha_base_url', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('haToken') as string}
              hint={t('haTokenHint') as string}
              value={tab.form.ha_token}
              onInput={(event) => tab.setForm('ha_token', event.currentTarget.value)}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">{t('haNote')}</p>
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

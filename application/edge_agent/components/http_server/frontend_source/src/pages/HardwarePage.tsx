import { createMemo, createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextArea } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';

type HardwareForm = {
  hw_pins: string;
};

const HW_PINS_PLACEHOLDER = `[
  {"gpio": 5, "name": "water_pump", "type": "switch", "mode": "output", "allowed": true},
  {"gpio": 34, "name": "soil_moisture", "type": "sensor", "mode": "analog", "allowed": true}
]`;

export const HardwarePage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<HardwareForm>({
    tab: 'hardware',
    groups: ['hardware'],
    toForm: (config: Partial<AppConfig>) => ({
      hw_pins: config.hw_pins ?? '[]',
    }),
    fromForm: (form) => ({
      hw_pins: form.hw_pins.trim() || '[]',
    }),
  });
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const jsonError = createMemo(() => {
    const raw = tab.form.hw_pins.trim();
    if (!raw) return null;
    try {
      const parsed = JSON.parse(raw);
      if (!Array.isArray(parsed)) return t('hwPinsNotArray') as string;
      return null;
    } catch {
      return t('hwPinsInvalidJson') as string;
    }
  });

  const handleSave = async () => {
    if (jsonError()) return;
    await tab.save();
    setConfirmOpen(true);
  };

  return (
    <TabShell>
      <PageHeader title={t('navHardware') as string} />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionHardware') as string}>
          <div class="pt-2">
            <TextArea
              label={t('hwPins') as string}
              hint={t('hwPinsHint') as string}
              placeholder={HW_PINS_PLACEHOLDER}
              rows={10}
              value={tab.form.hw_pins}
              onInput={(event) => tab.setForm('hw_pins', event.currentTarget.value)}
              error={jsonError() ?? undefined}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">{t('hwPinsNote')}</p>
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

import { createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput, SelectInput } from '../components/ui/FormField';
import { Switch } from '../components/ui/Switch';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';

type MqttForm = {
  mqtt_enabled: string;
  mqtt_broker: string;
  mqtt_port: string;
  mqtt_tls_enabled: string;
  mqtt_username: string;
  mqtt_password: string;
  mqtt_client_id: string;
  mqtt_keepalive: string;
  mqtt_qos: string;
  mqtt_base_topic: string;
};

const isTrue = (value: string) => value === 'true' || value === '1';
const boolStr = (value: boolean) => (value ? 'true' : 'false');

export const MqttPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<MqttForm>({
    tab: 'mqtt',
    groups: ['mqtt'],
    toForm: (config: Partial<AppConfig>) => ({
      mqtt_enabled: config.mqtt_enabled ?? 'false',
      mqtt_broker: config.mqtt_broker ?? '',
      mqtt_port: config.mqtt_port ?? '1883',
      mqtt_tls_enabled: config.mqtt_tls_enabled ?? 'false',
      mqtt_username: config.mqtt_username ?? '',
      mqtt_password: config.mqtt_password ?? '',
      mqtt_client_id: config.mqtt_client_id ?? '',
      mqtt_keepalive: config.mqtt_keepalive ?? '60',
      mqtt_qos: config.mqtt_qos ?? '0',
      mqtt_base_topic: config.mqtt_base_topic ?? 'espclaw',
    }),
    fromForm: (form) => ({
      mqtt_enabled: boolStr(isTrue(form.mqtt_enabled)),
      mqtt_broker: form.mqtt_broker.trim(),
      mqtt_port: form.mqtt_port.trim(),
      mqtt_tls_enabled: boolStr(isTrue(form.mqtt_tls_enabled)),
      mqtt_username: form.mqtt_username.trim(),
      mqtt_password: form.mqtt_password,
      mqtt_client_id: form.mqtt_client_id.trim(),
      mqtt_keepalive: form.mqtt_keepalive.trim(),
      mqtt_qos: form.mqtt_qos,
      mqtt_base_topic: form.mqtt_base_topic.trim(),
    }),
  });
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const handleSave = async () => {
    await tab.save();
    setConfirmOpen(true);
  };

  return (
    <TabShell>
      <PageHeader title={t('navMqtt') as string} />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionMqttBroker') as string}>
          <div class="pt-2">
            <Switch
              label={t('mqttEnabled') as string}
              hint={t('mqttEnabledHint') as string}
              checked={isTrue(tab.form.mqtt_enabled)}
              onChange={(value) => tab.setForm('mqtt_enabled', boolStr(value))}
            />
          </div>
          <div class="grid gap-3 sm:grid-cols-2 pt-3">
            <TextInput
              label={t('mqttBroker') as string}
              placeholder={t('mqttBrokerPlaceholder') as string}
              value={tab.form.mqtt_broker}
              onInput={(event) => tab.setForm('mqtt_broker', event.currentTarget.value)}
            />
            <TextInput
              type="number"
              label={t('mqttPort') as string}
              value={tab.form.mqtt_port}
              onInput={(event) => tab.setForm('mqtt_port', event.currentTarget.value)}
            />
            <TextInput
              label={t('mqttUsername') as string}
              value={tab.form.mqtt_username}
              onInput={(event) => tab.setForm('mqtt_username', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('mqttPassword') as string}
              value={tab.form.mqtt_password}
              onInput={(event) => tab.setForm('mqtt_password', event.currentTarget.value)}
            />
          </div>
          <div class="pt-3">
            <Switch
              label={t('mqttTls') as string}
              hint={t('mqttTlsHint') as string}
              checked={isTrue(tab.form.mqtt_tls_enabled)}
              onChange={(value) => tab.setForm('mqtt_tls_enabled', boolStr(value))}
            />
          </div>
        </StaticConfigBlock>
        <StaticConfigBlock title={t('sectionMqttOptions') as string}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              label={t('mqttClientId') as string}
              hint={t('mqttClientIdHint') as string}
              value={tab.form.mqtt_client_id}
              onInput={(event) => tab.setForm('mqtt_client_id', event.currentTarget.value)}
            />
            <TextInput
              type="number"
              label={t('mqttKeepalive') as string}
              value={tab.form.mqtt_keepalive}
              onInput={(event) => tab.setForm('mqtt_keepalive', event.currentTarget.value)}
            />
            <SelectInput
              label={t('mqttQos') as string}
              value={tab.form.mqtt_qos}
              onChange={(event) => tab.setForm('mqtt_qos', event.currentTarget.value)}
            >
              <option value="0">0</option>
              <option value="1">1</option>
            </SelectInput>
            <TextInput
              label={t('mqttBaseTopic') as string}
              hint={t('mqttBaseTopicHint') as string}
              value={tab.form.mqtt_base_topic}
              onInput={(event) => tab.setForm('mqtt_base_topic', event.currentTarget.value)}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">{t('mqttNote')}</p>
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

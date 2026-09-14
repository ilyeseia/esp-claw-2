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

type VpnForm = {
  vpn_mode: string;
  vpn_enabled: string;
  vpn_gateway: string;
  vpn_test_host: string;
  vpn_test_port: string;
  wg_private_key: string;
  wg_address: string;
  wg_peer_public_key: string;
  wg_endpoint: string;
  wg_endpoint_port: string;
  wg_allowed_ips: string;
  wg_keepalive: string;
  wg_preshared_key: string;
  wg_make_default: string;
};

const isTrue = (value: string) => value === 'true' || value === '1';
const boolStr = (value: boolean) => (value ? 'true' : 'false');

export const VpnPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<VpnForm>({
    tab: 'vpn',
    groups: ['vpn'],
    toForm: (config: Partial<AppConfig>) => ({
      vpn_mode: config.vpn_mode ?? 'tailscale-gateway',
      vpn_enabled: config.vpn_enabled ?? 'false',
      vpn_gateway: config.vpn_gateway ?? '',
      vpn_test_host: config.vpn_test_host ?? '',
      vpn_test_port: config.vpn_test_port ?? '80',
      wg_private_key: config.wg_private_key ?? '',
      wg_address: config.wg_address ?? '',
      wg_peer_public_key: config.wg_peer_public_key ?? '',
      wg_endpoint: config.wg_endpoint ?? '',
      wg_endpoint_port: config.wg_endpoint_port ?? '51820',
      wg_allowed_ips: config.wg_allowed_ips ?? '',
      wg_keepalive: config.wg_keepalive ?? '25',
      wg_preshared_key: config.wg_preshared_key ?? '',
      wg_make_default: config.wg_make_default ?? 'false',
    }),
    fromForm: (form) => ({
      vpn_mode: form.vpn_mode,
      vpn_enabled: boolStr(isTrue(form.vpn_enabled)),
      vpn_gateway: form.vpn_gateway.trim(),
      vpn_test_host: form.vpn_test_host.trim(),
      vpn_test_port: form.vpn_test_port.trim(),
      wg_private_key: form.wg_private_key.trim(),
      wg_address: form.wg_address.trim(),
      wg_peer_public_key: form.wg_peer_public_key.trim(),
      wg_endpoint: form.wg_endpoint.trim(),
      wg_endpoint_port: form.wg_endpoint_port.trim(),
      wg_allowed_ips: form.wg_allowed_ips.trim(),
      wg_keepalive: form.wg_keepalive.trim(),
      wg_preshared_key: form.wg_preshared_key.trim(),
      wg_make_default: boolStr(isTrue(form.wg_make_default)),
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
        <StaticConfigBlock title={t('sectionVpnMode') as string}>
          <div class="pt-2">
            <SelectInput
              label={t('vpnMode') as string}
              hint={t('vpnModeHint') as string}
              value={tab.form.vpn_mode}
              onChange={(event) => tab.setForm('vpn_mode', event.currentTarget.value)}
            >
              <option value="off">{t('vpnModeOff') as string}</option>
              <option value="tailscale-gateway">{t('vpnModeGateway') as string}</option>
              <option value="wireguard">{t('vpnModeWireguard') as string}</option>
            </SelectInput>
          </div>
        </StaticConfigBlock>

        <Show when={tab.form.vpn_mode === 'tailscale-gateway'}>
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
        </Show>

        <Show when={tab.form.vpn_mode === 'wireguard'}>
          <StaticConfigBlock title={t('sectionVpnWireguard') as string}>
            <div class="pt-2">
              <Switch
                label={t('vpnEnabled') as string}
                hint={t('wgEnabledHint') as string}
                checked={isTrue(tab.form.vpn_enabled)}
                onChange={(value) => tab.setForm('vpn_enabled', boolStr(value))}
              />
            </div>
            <div class="grid gap-3 sm:grid-cols-2 pt-3">
              <TextInput
                type="password"
                label={t('wgPrivateKey') as string}
                value={tab.form.wg_private_key}
                onInput={(event) => tab.setForm('wg_private_key', event.currentTarget.value)}
              />
              <TextInput
                label={t('wgAddress') as string}
                placeholder="10.2.0.2/32"
                hint={t('wgAddressHint') as string}
                value={tab.form.wg_address}
                onInput={(event) => tab.setForm('wg_address', event.currentTarget.value)}
              />
              <TextInput
                label={t('wgPeerPublicKey') as string}
                value={tab.form.wg_peer_public_key}
                onInput={(event) => tab.setForm('wg_peer_public_key', event.currentTarget.value)}
              />
              <TextInput
                label={t('wgEndpoint') as string}
                placeholder="vpn.example.com"
                value={tab.form.wg_endpoint}
                onInput={(event) => tab.setForm('wg_endpoint', event.currentTarget.value)}
              />
              <TextInput
                type="number"
                label={t('wgEndpointPort') as string}
                value={tab.form.wg_endpoint_port}
                onInput={(event) => tab.setForm('wg_endpoint_port', event.currentTarget.value)}
              />
              <TextInput
                type="number"
                label={t('wgKeepalive') as string}
                value={tab.form.wg_keepalive}
                onInput={(event) => tab.setForm('wg_keepalive', event.currentTarget.value)}
              />
              <TextInput
                label={t('wgAllowedIps') as string}
                placeholder="0.0.0.0/0"
                hint={t('wgAllowedIpsHint') as string}
                value={tab.form.wg_allowed_ips}
                onInput={(event) => tab.setForm('wg_allowed_ips', event.currentTarget.value)}
              />
              <TextInput
                type="password"
                label={t('wgPresharedKey') as string}
                hint={t('wgPresharedKeyHint') as string}
                value={tab.form.wg_preshared_key}
                onInput={(event) => tab.setForm('wg_preshared_key', event.currentTarget.value)}
              />
            </div>
            <div class="pt-3">
              <Switch
                label={t('wgMakeDefault') as string}
                hint={t('wgMakeDefaultHint') as string}
                checked={isTrue(tab.form.wg_make_default)}
                onChange={(value) => tab.setForm('wg_make_default', boolStr(value))}
              />
            </div>
            <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">{t('wgNote')}</p>
          </StaticConfigBlock>
        </Show>
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

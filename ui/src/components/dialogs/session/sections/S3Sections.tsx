/**
 * Session editor for S3 sessions: the storage (service preset, endpoint, region, bucket, start folder) in the Session
 * section, and the S3 section (sign-in, addressing, proxy, transfers).
 */
import { useEffect, useState } from 'react';
import { api } from '@/bridge/api';
import { NumberInput, Select, TextInput } from '@/components/ui';
import { useVault, useVaultLogins } from '@/store/vault';
import { isMac } from '@shared/platform';
import type { S3AuthMethod, S3Options, S3Preset } from '@shared/types';
import { Callout, ComboInput, FieldGrid, FormField, Section, SegmentedControl, type ComboOption, type SegmentOption } from '../../common/controls';
import { useEditor } from '../editorContext';
import { SavedPasswordSection } from './SshSections';

/** What choosing a service fills in. `endpoint` has a part to replace in <angle brackets> where the service needs one. */
export const S3_PRESETS: Record<S3Preset, { label: string; endpoint: string; region: string; hint: string }> = {
  aws: { label: 'Amazon S3', endpoint: '', region: '', hint: 'Leave the endpoint empty: each bucket’s region is found by itself' },
  r2: {
    label: 'Cloudflare R2',
    endpoint: 'https://<account-id>.r2.cloudflarestorage.com',
    region: 'auto',
    hint: 'The account ID is on the R2 page of the Cloudflare dashboard'
  },
  wasabi: { label: 'Wasabi', endpoint: 'https://s3.<region>.wasabisys.com', region: 'us-east-1', hint: 'e.g. s3.eu-central-1.wasabisys.com' },
  b2: { label: 'Backblaze B2', endpoint: 'https://s3.<region>.backblazeb2.com', region: 'us-west-004', hint: 'The endpoint is shown with the bucket in B2' },
  spaces: { label: 'DigitalOcean Spaces', endpoint: 'https://<region>.digitaloceanspaces.com', region: 'nyc3', hint: 'e.g. ams3, fra1, nyc3, sgp1' },
  minio: { label: 'MinIO', endpoint: 'http://localhost:9000', region: 'us-east-1', hint: 'Your MinIO’s API address (not the console’s)' },
  custom: { label: 'Other S3-compatible', endpoint: '', region: '', hint: 'Ceph, Garage, SeaweedFS, …: the address of its S3 API' }
};

const PRESET_OPTIONS = (Object.keys(S3_PRESETS) as S3Preset[]).map((p) => ({ value: p, label: S3_PRESETS[p].label }));

/** Applies a service preset: its endpoint and region, unless the user already changed them. */
export function applyS3Preset(o: S3Options, preset: S3Preset): S3Options {
  const old = S3_PRESETS[o.preset];
  const next = S3_PRESETS[preset];
  const endpointUntouched = !o.endpoint.trim() || o.endpoint === old.endpoint;
  const regionUntouched = !o.region.trim() || o.region === old.region;
  return {
    ...o,
    preset,
    endpoint: endpointUntouched ? next.endpoint : o.endpoint,
    region: regionUntouched ? next.region : o.region
  };
}

/** The storage fields, shown in the Session section for S3. */
export function S3ConnectionFields() {
  const { draft, update, fieldError, warning, isDefault } = useEditor();
  const o = draft.s3;
  const preset = S3_PRESETS[o.preset] ?? S3_PRESETS.custom;
  const placeholderEndpoint = o.endpoint.includes('<');
  return (
    <FieldGrid cols={2}>
      <FormField label="Service" htmlFor="se-s3-preset">
        <Select
          id="se-s3-preset"
          value={o.preset}
          options={PRESET_OPTIONS}
          onChange={(p) => update((d) => void (d.s3 = applyS3Preset(d.s3, p)))}
          aria-label="Service"
        />
      </FormField>
      <FormField label="Region" error={fieldError('s3.region')} hint={o.preset === 'aws' ? 'Empty: us-east-1 (each bucket reports its own)' : undefined} htmlFor="se-s3-region">
        <TextInput
          id="se-s3-region"
          value={o.region}
          placeholder={o.preset === 'aws' ? 'us-east-1' : preset.region || 'us-east-1'}
          invalid={!!fieldError('s3.region')}
          onChange={(v) => update((d) => void (d.s3.region = v.trim()))}
        />
      </FormField>
      <FormField
        label="Endpoint"
        span="all"
        error={fieldError('s3.endpoint') ?? (placeholderEndpoint ? 'Replace the part in <angle brackets>' : undefined)}
        warning={warning('s3.endpoint')}
        hint={warning('s3.endpoint') ? undefined : preset.hint}
        htmlFor="se-s3-endpoint"
      >
        <TextInput
          id="se-s3-endpoint"
          className="mono"
          value={o.endpoint}
          placeholder={o.preset === 'aws' ? '(AWS)' : 'https://s3.example.com'}
          invalid={!!fieldError('s3.endpoint') || placeholderEndpoint}
          onChange={(v) => update((d) => void (d.s3.endpoint = v.trim()))}
        />
      </FormField>
      <FormField
        label="Bucket"
        error={fieldError('s3.bucket')}
        hint={isDefault ? undefined : 'Empty: the storage shows all buckets the key may list'}
        htmlFor="se-s3-bucket"
      >
        <TextInput
          id="se-s3-bucket"
          value={o.bucket}
          placeholder="(all buckets)"
          invalid={!!fieldError('s3.bucket')}
          onChange={(v) => update((d) => void (d.s3.bucket = v.trim()))}
        />
      </FormField>
      <FormField label="Start in folder" hint="A folder inside the bucket" htmlFor="se-s3-start">
        <TextInput
          id="se-s3-start"
          value={o.startPath}
          placeholder="(top)"
          disabled={!o.bucket.trim()}
          onChange={(v) => update((d) => void (d.s3.startPath = v))}
        />
      </FormField>
    </FieldGrid>
  );
}

const METHOD_OPTIONS: readonly SegmentOption<S3AuthMethod>[] = [
  { value: 'keys', label: 'Access key', title: 'An access key ID and its secret access key' },
  { value: 'profile', label: 'AWS profile', title: 'A profile of ~/.aws/config and ~/.aws/credentials (keys, roles, SSO, credential_process)' },
  { value: 'env', label: 'Environment', title: 'AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_SESSION_TOKEN' },
  { value: 'anonymous', label: 'Anonymous', title: 'Public buckets: nothing is signed' }
];

type KeySource = 'own' | 'login';
type ProxyChoice = 'system' | 'none' | 'http';

export function S3Section() {
  const { draft, update, fieldError, isDefault } = useEditor();
  const o = draft.s3;
  const a = o.auth;
  const logins = useVaultLogins();
  const vaultLoaded = useVault((s) => s.loaded);
  const [keySource, setKeySource] = useState<KeySource>(a.loginId ? 'login' : 'own');
  const [profiles, setProfiles] = useState<{ profiles: string[]; files: string[] } | null>(null);
  useEffect(() => {
    if (a.method !== 'profile' || profiles) return;
    api.s3
      .profiles()
      .then(setProfiles)
      .catch(() => setProfiles({ profiles: [], files: [] }));
  }, [a.method, profiles]);
  const chosen = logins.find((l) => l.id === a.loginId);
  const proxy: ProxyChoice = o.systemProxy ? 'system' : draft.proxy.type === 'http' ? 'http' : 'none';
  const profileOptions: ComboOption[] = (profiles?.profiles ?? []).map((p) => ({ value: p }));

  return (
    <>
      <Section title="Sign in">
        <SegmentedControl
          ariaLabel="Sign in with"
          value={a.method}
          options={METHOD_OPTIONS}
          onChange={(m) => update((d) => void (d.s3.auth.method = m))}
        />
        {a.method === 'keys' && (
          <>
            <SegmentedControl
              ariaLabel="Access key from"
              size="sm"
              value={keySource}
              options={[
                { value: 'own', label: 'This session' },
                { value: 'login', label: 'A saved login', title: 'A login from Settings › Credentials: user name = access key ID, password = secret' }
              ]}
              onChange={(v) => {
                setKeySource(v);
                if (v === 'own') update((d) => void (d.s3.auth.loginId = ''));
              }}
            />
            {keySource === 'own' ? (
              <FieldGrid cols={2}>
                <FormField
                  label="Access key ID"
                  span="all"
                  error={fieldError('s3.auth.accessKeyId')}
                  hint="Empty: asked when connecting. The secret is saved below, or asked."
                  htmlFor="se-s3-key"
                >
                  <TextInput
                    id="se-s3-key"
                    className="mono"
                    value={a.accessKeyId}
                    placeholder="AKIA…"
                    autoComplete="off"
                    spellCheck={false}
                    invalid={!!fieldError('s3.auth.accessKeyId')}
                    onChange={(v) => update((d) => void (d.s3.auth.accessKeyId = v.trim()))}
                  />
                </FormField>
              </FieldGrid>
            ) : (
              <FormField label="Saved login" hint="Its user name is the access key ID, its password the secret access key">
                <Select
                  value={a.loginId}
                  aria-label="Saved login"
                  options={[
                    { value: '', label: logins.length ? 'Choose a login…' : 'No logins yet: add one in Settings › Credentials' },
                    ...logins.map((l) => ({ value: l.id, label: `${l.name} (${l.username})` }))
                  ]}
                  onChange={(id) => update((d) => void (d.s3.auth.loginId = id))}
                />
              </FormField>
            )}
            {keySource === 'login' && a.loginId && !chosen && vaultLoaded && (
              <Callout tone="warning">The chosen login is no longer in Credentials, so this session signs in with its own access key. Choose another login.</Callout>
            )}
          </>
        )}
        {a.method === 'profile' && (
          <FormField
            label="Profile"
            hint={
              profiles?.files.length
                ? `From ${profiles.files.join(' and ')}. Empty: AWS_PROFILE, else "default". Roles, MFA, SSO (after aws sso login) and credential_process work.`
                : 'Empty: AWS_PROFILE, else "default"'
            }
            htmlFor="se-s3-profile"
          >
            <ComboInput
              id="se-s3-profile"
              value={a.profile}
              options={profileOptions}
              placeholder="default"
              onChange={(v) => update((d) => void (d.s3.auth.profile = v.trim()))}
            />
          </FormField>
        )}
        {a.method === 'env' && (
          <Callout tone="info">
            Uses AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY and AWS_SESSION_TOKEN; AWS_REGION and AWS_ENDPOINT_URL(_S3) when the session leaves
            region and endpoint empty.
            {isMac && ' An app started from the Finder or the Dock does not see the variables of your shell: start SSHTermaq from a terminal, or use a profile.'}
          </Callout>
        )}
        {a.method === 'anonymous' && (
          <Callout tone="info">Nothing is signed: only public buckets work, and the bucket’s name must be given.</Callout>
        )}
      </Section>

      {a.method === 'keys' && keySource === 'own' && !isDefault && (
        <SavedPasswordSection noun="secret access key" usedWhen="when connecting" />
      )}

      <Section title="Addressing">
        <FormField label="Bucket address" hint="Most self-hosted services (MinIO, Ceph) need path-style">
          <Select
            value={o.addressing}
            aria-label="Bucket address"
            options={[
              { value: 'auto', label: 'Automatic (virtual-hosted for AWS, path-style otherwise)' },
              { value: 'virtual', label: 'Virtual-hosted (bucket.host)' },
              { value: 'path', label: 'Path-style (host/bucket)' }
            ]}
            onChange={(v) => update((d) => void (d.s3.addressing = v))}
          />
        </FormField>
      </Section>

      <Section title="Proxy" description="S3 goes through the system’s proxy settings, no proxy, or an HTTP proxy of its own.">
        <SegmentedControl
          ariaLabel="Proxy"
          size="sm"
          value={proxy}
          options={[
            { value: 'system', label: 'System' },
            { value: 'none', label: 'None' },
            { value: 'http', label: 'HTTP proxy' }
          ]}
          onChange={(v) =>
            update((d) => {
              d.s3.systemProxy = v === 'system';
              if (v !== 'system') d.proxy.type = v === 'http' ? 'http' : 'none';
              if (v === 'http' && (!d.proxy.port || d.proxy.port === 1080)) d.proxy.port = 8080;
            })
          }
        />
        {proxy === 'http' && (
          <FieldGrid cols={4}>
            <FormField label="Proxy host" span={3} error={fieldError('proxy.host')} htmlFor="se-s3-proxyhost">
              <TextInput id="se-s3-proxyhost" value={draft.proxy.host} onChange={(v) => update((d) => void (d.proxy.host = v))} invalid={!!fieldError('proxy.host')} />
            </FormField>
            <FormField label="Port" error={fieldError('proxy.port')} htmlFor="se-s3-proxyport">
              <NumberInput id="se-s3-proxyport" value={draft.proxy.port} min={1} max={65535} onChange={(n) => update((d) => void (d.proxy.port = n))} />
            </FormField>
            <FormField
              label="User name"
              span={2}
              hint={isMac ? 'Empty: no sign-in' : 'Empty: your Windows sign-in where the proxy accepts it'}
              htmlFor="se-s3-proxyuser"
            >
              <TextInput id="se-s3-proxyuser" value={draft.proxy.username} onChange={(v) => update((d) => void (d.proxy.username = v))} />
            </FormField>
            <FormField label="Password" span={2} htmlFor="se-s3-proxypass">
              <TextInput id="se-s3-proxypass" type="password" value={draft.proxy.password} onChange={(v) => update((d) => void (d.proxy.password = v))} />
            </FormField>
          </FieldGrid>
        )}
        {fieldError('proxy.type') && <Callout tone="danger">{fieldError('proxy.type')}</Callout>}
      </Section>

      <Section title="Transfers" description="Big files go in parts, several at once; the parts in flight are held in memory.">
        <FieldGrid cols={2}>
          <FormField label="Part size (MiB)" error={fieldError('s3.partSizeMiB')} hint="8 – 64" htmlFor="se-s3-part">
            <NumberInput id="se-s3-part" value={o.partSizeMiB} min={8} max={64} onChange={(n) => update((d) => void (d.s3.partSizeMiB = n))} />
          </FormField>
          <FormField label="Parts at once" error={fieldError('s3.parallelParts')} hint="1 – 16, per file" htmlFor="se-s3-parallel">
            <NumberInput id="se-s3-parallel" value={o.parallelParts} min={1} max={16} onChange={(n) => update((d) => void (d.s3.parallelParts = n))} />
          </FormField>
        </FieldGrid>
      </Section>
    </>
  );
}

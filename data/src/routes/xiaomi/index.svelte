<script lang="ts">
	import { notifications } from '$lib/stores/notifications.svelte.js';

	type Device = {
		name: string;
		model: string;
		did: string;
		token: string;
		ip: string;
		online: boolean;
	};

	const regions = [
		{ value: 'de', label: 'Europe (de)' },
		{ value: 'cn', label: 'China (cn)' },
		{ value: 'us', label: 'United States (us)' },
		{ value: 'ru', label: 'Russia (ru)' },
		{ value: 'sg', label: 'Singapore (sg)' },
		{ value: 'i2', label: 'India (i2)' },
		{ value: 'tw', label: 'Taiwan (tw)' }
	];

	let username = $state('');
	let password = $state('');
	let region = $state('de');
	let devices = $state<Device[]>([]);
	let twoFactorUrl = $state('');

	let ip = $state('');
	let token = $state('');
	let did = $state('');
	let aiid = $state(4);

	let loggingIn = $state(false);
	let saving = $state(false);
	let testing = $state(false);

	// Locks first: that is what anyone lands on this page for.
	const sorted = $derived(
		[...devices].sort((a, b) => Number(b.model.includes('.lock.')) - Number(a.model.includes('.lock.')))
	);
	const configured = $derived(ip !== '' && token.length === 32 && did !== '');

	async function loadCurrent() {
		try {
			const res = await fetch('/config?type=misc').then((r) => r.json());
			if (!res.success) return;
			ip = res.data.xiaomiLockIp ?? '';
			token = res.data.xiaomiLockToken ?? '';
			did = res.data.xiaomiLockDid ?? '';
			aiid = res.data.xiaomiUnlatchAiid ?? 4;
		} catch {
			// the form still works empty
		}
	}
	loadCurrent();

	async function login(event: Event) {
		event.preventDefault();
		loggingIn = true;
		twoFactorUrl = '';
		devices = [];
		try {
			const res = await fetch('/xiaomi/login', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ username, password, region })
			});
			const body = await res.json();
			if (!body.success) {
				twoFactorUrl = body.twoFactorUrl ?? '';
				notifications.addError(body.error ?? 'Login failed');
				return;
			}
			devices = body.devices as Device[];
			password = '';
			notifications.addSuccess(`Found ${devices.length} device(s)`);
		} catch (error) {
			notifications.addError(error instanceof Error ? error.message : 'Login failed');
		} finally {
			loggingIn = false;
		}
	}

	function pick(device: Device) {
		ip = device.ip;
		token = device.token;
		did = device.did;
	}

	async function save(event: Event) {
		event.preventDefault();
		saving = true;
		try {
			const res = await fetch('/xiaomi/select', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ ip, token, did, aiid })
			});
			const body = await res.json();
			if (!body.success) {
				notifications.addError(body.error ?? 'Could not save');
				return;
			}
			notifications.addSuccess('Xiaomi lock saved');
		} catch (error) {
			notifications.addError(error instanceof Error ? error.message : 'Could not save');
		} finally {
			saving = false;
		}
	}

	async function test() {
		testing = true;
		try {
			const res = await fetch('/xiaomi/test', { method: 'POST' });
			const body = await res.json();
			if (!body.success) {
				notifications.addError(body.error ?? 'The lock did not answer');
				return;
			}
			notifications.addSuccess('Unlatch sent');
		} catch (error) {
			notifications.addError(error instanceof Error ? error.message : 'The lock did not answer');
		} finally {
			testing = false;
		}
	}
</script>

<div class="flex flex-col gap-4 p-4">
	<div class="card bg-base-200 shadow">
		<div class="card-body">
			<h2 class="card-title">Xiaomi account</h2>
			<p class="text-sm opacity-70">
				Used once to read the local token of your lock. The password is not stored and never
				leaves this device except towards Xiaomi's login servers.
			</p>

			<form class="flex flex-col gap-3" onsubmit={login}>
				<label class="form-control w-full">
					<span class="label-text">Server region</span>
					<select class="select select-bordered w-full" bind:value={region}>
						{#each regions as r (r.value)}
							<option value={r.value}>{r.label}</option>
						{/each}
					</select>
				</label>

				<label class="form-control w-full">
					<span class="label-text">Xiaomi account (email, phone or ID)</span>
					<input class="input input-bordered w-full" bind:value={username} autocomplete="username" />
				</label>

				<label class="form-control w-full">
					<span class="label-text">Password</span>
					<input
						type="password"
						class="input input-bordered w-full"
						bind:value={password}
						autocomplete="current-password"
					/>
				</label>

				<button class="btn btn-primary" type="submit" disabled={loggingIn || !username || !password}>
					{#if loggingIn}<span class="loading loading-spinner"></span>{/if}
					Log in and list devices
				</button>
			</form>

			{#if twoFactorUrl}
				<div class="alert alert-warning mt-3">
					<span>
						This account needs two-factor verification. Open
						<a class="link" href={twoFactorUrl} target="_blank" rel="noreferrer">this URL</a>
						in a browser, confirm, then log in again.
					</span>
				</div>
			{/if}
		</div>
	</div>

	{#if sorted.length}
		<div class="card bg-base-200 shadow">
			<div class="card-body">
				<h2 class="card-title">Devices</h2>
				<div class="overflow-x-auto">
					<table class="table table-zebra">
						<thead>
							<tr><th>Name</th><th>Model</th><th>IP</th><th></th></tr>
						</thead>
						<tbody>
							{#each sorted as device (device.did)}
								<tr class={device.model.includes('.lock.') ? 'font-semibold' : ''}>
									<td>{device.name}{#if !device.online}<span class="badge badge-ghost ml-2">offline</span>{/if}</td>
									<td class="font-mono text-xs">{device.model}</td>
									<td class="font-mono text-xs">{device.ip || '-'}</td>
									<td>
										<button class="btn btn-sm" onclick={() => pick(device)} disabled={!device.ip}>
											Use
										</button>
									</td>
								</tr>
							{/each}
						</tbody>
					</table>
				</div>
			</div>
		</div>
	{/if}

	<div class="card bg-base-200 shadow">
		<div class="card-body">
			<h2 class="card-title">Lock used on HomeKey tap</h2>
			<form class="flex flex-col gap-3" onsubmit={save}>
				<label class="form-control w-full">
					<span class="label-text">Lock IP address</span>
					<input class="input input-bordered w-full font-mono" bind:value={ip} placeholder="192.168.1.50" />
				</label>

				<label class="form-control w-full">
					<span class="label-text">Local token (32 hex characters)</span>
					<input class="input input-bordered w-full font-mono" bind:value={token} spellcheck="false" />
				</label>

				<label class="form-control w-full">
					<span class="label-text">Device id (did)</span>
					<input class="input input-bordered w-full font-mono" bind:value={did} />
				</label>

				<label class="form-control w-full">
					<span class="label-text">Unlatch action</span>
					<select class="select select-bordered w-full" bind:value={aiid}>
						<option value={4}>emergency-unlock — pulls the latch (siid 18 / aiid 4)</option>
						<option value={9}>ble-unlock (siid 18 / aiid 9)</option>
					</select>
				</label>

				<div class="flex gap-2">
					<button class="btn btn-primary" type="submit" disabled={saving || !configured}>
						{#if saving}<span class="loading loading-spinner"></span>{/if}
						Save
					</button>
					<button class="btn" type="button" onclick={test} disabled={testing || !configured}>
						{#if testing}<span class="loading loading-spinner"></span>{/if}
						Test unlatch now
					</button>
				</div>
			</form>
		</div>
	</div>
</div>

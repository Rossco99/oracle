import { APIClient, Chains, Session } from '@wharfkit/session';
import { WalletPluginPrivateKey } from '@wharfkit/wallet-plugin-privatekey';

import { Contract as DropContract } from './contracts/drops';
export * as DropContract from './contracts/drops';

import { Contract as EpochContract } from './contracts/epoch.drops';
export * as EpochContract from './contracts/epoch.drops';

if (!process.env.ACCOUNT_NAME) {
	throw new Error('An ACCOUNT_NAME value must be provided in an .env file or on the command line.');
}
if (!process.env.PERMISSION_LEVEL) {
	throw new Error(
		'An PERMISSION_LEVEL value must be provided in an .env file or on the command line.'
	);
}
if (!process.env.PRIVATE_KEY) {
	throw new Error('A PRIVATE_KEY value must be provided in an .env file or on the command line.');
}
const walletPlugin = new WalletPluginPrivateKey(process.env.PRIVATE_KEY);

export const url = process.env.API_ENDPOINT || 'https://jungle4.greymass.com';
export const client = new APIClient({ url });

export const dropsContract: DropContract = new DropContract({ client });
export const epochContract: EpochContract = new EpochContract({ client });

export const session: Session = new Session({
	chain: Chains.Jungle4,
	walletPlugin,
	actor: process.env.ACCOUNT_NAME,
	permission: process.env.PERMISSION_LEVEL
});

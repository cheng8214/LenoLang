import * as path from 'path';
import { workspace, ExtensionContext, window, commands, Uri } from 'vscode';
import {
	LanguageClient,
	LanguageClientOptions,
	ServerOptions,
	TransportKind
} from 'vscode-languageclient/node';
import * as fs from 'fs';

let client: LanguageClient;

export function activate(context: ExtensionContext) {
	// Register command to associate .leno files with Leno language
	context.subscriptions.push(
		commands.registerCommand('leno.associateFiles', async () => {
			// Update settings to associate .leno extension with leno language
			const config = workspace.getConfiguration('files');
			const associations = config.get<{ [key: string]: string }>('associations') || {};
			associations['*.leno'] = 'leno';
			await config.update('associations', associations, true);
			window.showInformationMessage('已关联 .leno 文件到 Leno 语言');
		})
	);

	// Try to find the LSP server executable
	let serverModule: string | undefined;
	
	// First, check user configuration (highest priority)
	const config = workspace.getConfiguration('leno');
	const configuredPath = config.get<string>('languageServer.path');
	
	if (configuredPath) {
		// Expand ~ to home directory if needed
		const expandedPath = configuredPath.replace(/^~/, process.env.HOME || process.env.USERPROFILE || '');
		if (fs.existsSync(expandedPath)) {
			serverModule = expandedPath;
			console.log('[Leno] Using configured LSP server:', serverModule);
		} else {
			console.log('[Leno] Configured path not found:', expandedPath);
		}
	}
	
	// If not found, try development path
	if (!serverModule) {
		const devPath = context.asAbsolutePath(
			path.join('..', '..', '..', 'leno_lsp', 'build', 'leno_lsp.exe')
		);
		if (fs.existsSync(devPath)) {
			serverModule = devPath;
			console.log('[Leno] Using development LSP server:', serverModule);
		}
	}
	
	// If still not found, show error
	if (!serverModule) {
		window.showErrorMessage(
			'Leno Language Server 未找到。请在设置中配置 leno.languageServer.path，指向 leno_lsp.exe 文件。'
		);
		return;
	}

	// Server options
	const serverOptions: ServerOptions = {
		run: { command: serverModule, transport: TransportKind.stdio },
		debug: { command: serverModule, transport: TransportKind.stdio }
	};

	// Client options
	const clientOptions: LanguageClientOptions = {
		documentSelector: [
			{ scheme: 'file', language: 'leno' },
			{ scheme: 'file', pattern: '**/*.leno' }
		],
		synchronize: {
			fileEvents: workspace.createFileSystemWatcher('**/*.leno')
		}
	};

	// Create and start the client
	client = new LanguageClient(
		'lenoLanguageServer',
		'Leno Language Server',
		serverOptions,
		clientOptions
	);

	client.start();
}

export function deactivate(): Thenable<void> | undefined {
	if (!client) {
		return undefined;
	}
	return client.stop();
}

"use strict";
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || function (mod) {
    if (mod && mod.__esModule) return mod;
    var result = {};
    if (mod != null) for (var k in mod) if (k !== "default" && Object.prototype.hasOwnProperty.call(mod, k)) __createBinding(result, mod, k);
    __setModuleDefault(result, mod);
    return result;
};
Object.defineProperty(exports, "__esModule", { value: true });
exports.deactivate = exports.activate = void 0;
const path = __importStar(require("path"));
const vscode_1 = require("vscode");
const node_1 = require("vscode-languageclient/node");
const fs = __importStar(require("fs"));
let client;
function activate(context) {
    // Register command to associate .leno files with Leno language
    context.subscriptions.push(vscode_1.commands.registerCommand('leno.associateFiles', async () => {
        // Update settings to associate .leno extension with leno language
        const config = vscode_1.workspace.getConfiguration('files');
        const associations = config.get('associations') || {};
        associations['*.leno'] = 'leno';
        await config.update('associations', associations, true);
        vscode_1.window.showInformationMessage('已关联 .leno 文件到 Leno 语言');
    }));
    // Try to find the LSP server executable
    let serverModule;
    // First, check user configuration (highest priority)
    const config = vscode_1.workspace.getConfiguration('leno');
    const configuredPath = config.get('languageServer.path');
    if (configuredPath) {
        // Expand ~ to home directory if needed
        const expandedPath = configuredPath.replace(/^~/, process.env.HOME || process.env.USERPROFILE || '');
        if (fs.existsSync(expandedPath)) {
            serverModule = expandedPath;
            console.log('[Leno] Using configured LSP server:', serverModule);
        }
        else {
            console.log('[Leno] Configured path not found:', expandedPath);
        }
    }
    // If not found, try development path
    if (!serverModule) {
        const devPath = context.asAbsolutePath(path.join('..', '..', '..', 'leno_lsp', 'build', 'leno_lsp.exe'));
        if (fs.existsSync(devPath)) {
            serverModule = devPath;
            console.log('[Leno] Using development LSP server:', serverModule);
        }
    }
    // If still not found, show error
    if (!serverModule) {
        vscode_1.window.showErrorMessage('Leno Language Server 未找到。请在设置中配置 leno.languageServer.path，指向 leno_lsp.exe 文件。');
        return;
    }
    // Server options
    const serverOptions = {
        run: { command: serverModule, transport: node_1.TransportKind.stdio },
        debug: { command: serverModule, transport: node_1.TransportKind.stdio }
    };
    // Client options
    const clientOptions = {
        documentSelector: [
            { scheme: 'file', language: 'leno' },
            { scheme: 'file', pattern: '**/*.leno' }
        ],
        synchronize: {
            fileEvents: vscode_1.workspace.createFileSystemWatcher('**/*.leno')
        }
    };
    // Create and start the client
    client = new node_1.LanguageClient('lenoLanguageServer', 'Leno Language Server', serverOptions, clientOptions);
    client.start();
}
exports.activate = activate;
function deactivate() {
    if (!client) {
        return undefined;
    }
    return client.stop();
}
exports.deactivate = deactivate;
//# sourceMappingURL=extension.js.map
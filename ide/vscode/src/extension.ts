import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind
} from 'vscode-languageclient/node';
import { activateDebugger } from './debugger';

let client: LanguageClient;

export function activate(context: vscode.ExtensionContext) {
    console.log('Goo language extension is now active!');

    // Get configuration
    const config = vscode.workspace.getConfiguration('goo');
    const enabled = config.get<boolean>('languageServer.enabled', true);
    
    if (!enabled) {
        console.log('Goo language server is disabled');
        return;
    }

    // Language server executable (prefer enhanced version)
    const serverPath = config.get<string>('languageServer.path', 'goo-lsp-standalone');
    const serverArgs = config.get<string[]>('languageServer.args', []);

    // Server options
    const serverOptions: ServerOptions = {
        run: { 
            command: serverPath, 
            args: serverArgs,
            transport: TransportKind.stdio 
        },
        debug: { 
            command: serverPath, 
            args: [...serverArgs, '--verbose'],
            transport: TransportKind.stdio 
        }
    };

    // Client options
    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: 'file', language: 'goo' }
        ],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.goo')
        },
        outputChannelName: 'Goo Language Server'
    };

    // Create and start the language client
    client = new LanguageClient(
        'gooLanguageServer',
        'Goo Language Server',
        serverOptions,
        clientOptions
    );

    // Register commands
    const restartCommand = vscode.commands.registerCommand('goo.restartLanguageServer', () => {
        if (client) {
            client.restart();
        }
    });

    context.subscriptions.push(restartCommand);

    // Start the client
    client.start().then(() => {
        console.log('Goo language server started successfully');
    }).catch((error) => {
        console.error('Failed to start Goo language server:', error);
        vscode.window.showErrorMessage(`Failed to start Goo language server: ${error.message}`);
    });

    // Activate debugging support
    activateDebugger(context);
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}
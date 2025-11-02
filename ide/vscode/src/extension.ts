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

    // Language server executable (prefer Goo-enhanced version)
    const serverPath = config.get<string>('languageServer.path', 'goo-lsp-goo-enhanced');
    const serverArgs = config.get<string[]>('languageServer.args', ['--debug']);

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
            vscode.window.showInformationMessage('Goo Language Server restarted');
        }
    });

    // Enhanced Goo-specific commands
    const showOwnershipInfo = vscode.commands.registerCommand('goo.showOwnershipInfo', () => {
        const editor = vscode.window.activeTextEditor;
        if (editor && editor.document.languageId === 'goo') {
            vscode.window.showInformationMessage('Ownership tracking information will be displayed in hover tooltips and diagnostics');
        }
    });

    const checkErrorUnions = vscode.commands.registerCommand('goo.checkErrorUnions', () => {
        const editor = vscode.window.activeTextEditor;
        if (editor && editor.document.languageId === 'goo') {
            vscode.languages.getDiagnostics(editor.document.uri).then(diagnostics => {
                const errorUnionDiagnostics = diagnostics.filter(d => 
                    d.message.includes('error union') || d.message.includes('unhandled')
                );
                if (errorUnionDiagnostics.length > 0) {
                    vscode.window.showWarningMessage(`Found ${errorUnionDiagnostics.length} potential error union issues`);
                } else {
                    vscode.window.showInformationMessage('No error union issues found');
                }
            });
        }
    });

    const showNullableInfo = vscode.commands.registerCommand('goo.showNullableInfo', () => {
        const editor = vscode.window.activeTextEditor;
        if (editor && editor.document.languageId === 'goo') {
            vscode.languages.getDiagnostics(editor.document.uri).then(diagnostics => {
                const nullableDiagnostics = diagnostics.filter(d => 
                    d.message.includes('null') || d.message.includes('nullable')
                );
                if (nullableDiagnostics.length > 0) {
                    vscode.window.showWarningMessage(`Found ${nullableDiagnostics.length} potential null safety issues`);
                } else {
                    vscode.window.showInformationMessage('No null safety issues found');
                }
            });
        }
    });

    context.subscriptions.push(restartCommand);
    context.subscriptions.push(showOwnershipInfo);
    context.subscriptions.push(checkErrorUnions);
    context.subscriptions.push(showNullableInfo);

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
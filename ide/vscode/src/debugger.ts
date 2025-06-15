import * as vscode from 'vscode';
import { DebugConfiguration, DebugConfigurationProvider, WorkspaceFolder, CancellationToken, ProviderResult } from 'vscode';

export class GooDebugConfigurationProvider implements DebugConfigurationProvider {
    
    /**
     * Massage a debug configuration just before a debug session is being launched,
     * e.g. add all missing attributes to the debug configuration.
     */
    resolveDebugConfiguration(folder: WorkspaceFolder | undefined, config: DebugConfiguration, token?: CancellationToken): ProviderResult<DebugConfiguration> {
        
        // if launch.json is missing or empty
        if (!config.type && !config.request && !config.name) {
            const editor = vscode.window.activeTextEditor;
            if (editor && editor.document.languageId === 'goo') {
                config.type = 'goo';
                config.name = 'Launch';
                config.request = 'launch';
                config.program = '${file}';
                config.stopOnEntry = true;
            }
        }

        if (!config.program) {
            return vscode.window.showInformationMessage("Cannot find a program to debug").then(_ => {
                return undefined;    // abort launch
            });
        }

        return config;
    }
}

export class GooDebugAdapterDescriptorFactory implements vscode.DebugAdapterDescriptorFactory {
    
    createDebugAdapterDescriptor(session: vscode.DebugSession, executable: vscode.DebugAdapterExecutable | undefined): ProviderResult<vscode.DebugAdapterDescriptor> {
        
        // Get configuration
        const config = vscode.workspace.getConfiguration('goo');
        const debugAdapterPath = config.get<string>('debugAdapter.path', 'goo-debug-adapter');
        const debugAdapterArgs = config.get<string[]>('debugAdapter.args', []);
        
        // Return the debug adapter executable
        return new vscode.DebugAdapterExecutable(debugAdapterPath, debugAdapterArgs);
    }
}

export class GooDebugAdapterTrackerFactory implements vscode.DebugAdapterTrackerFactory {
    
    createDebugAdapterTracker(session: vscode.DebugSession): ProviderResult<vscode.DebugAdapterTracker> {
        return {
            onWillReceiveMessage: m => {
                console.log('DAP Receive:', m);
            },
            onDidSendMessage: m => {
                console.log('DAP Send:', m);
            },
            onWillStartSession: () => {
                console.log('Debug session starting');
            },
            onWillStopSession: () => {
                console.log('Debug session stopping');
            },
            onError: (error) => {
                console.error('Debug adapter error:', error);
            },
            onExit: (code, signal) => {
                console.log(`Debug adapter exited with code ${code}, signal ${signal}`);
            }
        };
    }
}

export function activateDebugger(context: vscode.ExtensionContext) {
    
    // Register debug configuration provider
    const provider = new GooDebugConfigurationProvider();
    context.subscriptions.push(vscode.debug.registerDebugConfigurationProvider('goo', provider));
    
    // Register debug adapter descriptor factory
    const factory = new GooDebugAdapterDescriptorFactory();
    context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory('goo', factory));
    
    // Register debug adapter tracker factory (for logging/debugging)
    const trackerFactory = new GooDebugAdapterTrackerFactory();
    context.subscriptions.push(vscode.debug.registerDebugAdapterTrackerFactory('goo', trackerFactory));
    
    // Register debug commands
    const startDebugging = vscode.commands.registerCommand('goo.startDebugging', () => {
        const editor = vscode.window.activeTextEditor;
        if (editor && editor.document.languageId === 'goo') {
            const config: vscode.DebugConfiguration = {
                type: 'goo',
                name: 'Debug Goo Program',
                request: 'launch',
                program: editor.document.fileName,
                stopOnEntry: true,
                console: 'integratedTerminal'
            };
            
            vscode.debug.startDebugging(undefined, config);
        } else {
            vscode.window.showErrorMessage('Please open a Goo file to debug');
        }
    });
    
    const runDebugging = vscode.commands.registerCommand('goo.runDebugging', () => {
        const editor = vscode.window.activeTextEditor;
        if (editor && editor.document.languageId === 'goo') {
            const config: vscode.DebugConfiguration = {
                type: 'goo',
                name: 'Run Goo Program',
                request: 'launch',
                program: editor.document.fileName,
                stopOnEntry: false,
                console: 'integratedTerminal'
            };
            
            vscode.debug.startDebugging(undefined, config);
        } else {
            vscode.window.showErrorMessage('Please open a Goo file to run');
        }
    });
    
    context.subscriptions.push(startDebugging);
    context.subscriptions.push(runDebugging);
}

// Export debug types for launch.json intellisense
export const debugConfigurationSchema = {
    type: 'object',
    properties: {
        type: {
            type: 'string',
            description: 'Type of debugger',
            enum: ['goo']
        },
        name: {
            type: 'string',
            description: 'Name of the configuration'
        },
        request: {
            type: 'string',
            description: 'Request type',
            enum: ['launch', 'attach']
        },
        program: {
            type: 'string',
            description: 'Path to the Goo program to debug',
            default: '${file}'
        },
        args: {
            type: 'array',
            description: 'Command line arguments passed to the program',
            items: {
                type: 'string'
            },
            default: []
        },
        cwd: {
            type: 'string',
            description: 'Working directory',
            default: '${workspaceFolder}'
        },
        env: {
            type: 'object',
            description: 'Environment variables',
            default: {}
        },
        stopOnEntry: {
            type: 'boolean',
            description: 'Stop at the beginning of the main function',
            default: false
        },
        console: {
            type: 'string',
            description: 'Where to launch the debug target',
            enum: ['internalConsole', 'integratedTerminal', 'externalTerminal'],
            default: 'internalConsole'
        },
        breakpoints: {
            type: 'object',
            description: 'Breakpoint configuration',
            properties: {
                errorUnions: {
                    type: 'boolean',
                    description: 'Break on error union propagation',
                    default: false
                },
                panics: {
                    type: 'boolean',
                    description: 'Break on panic calls',
                    default: true
                },
                memoryErrors: {
                    type: 'boolean',
                    description: 'Break on memory safety violations',
                    default: true
                }
            }
        },
        showDisassembly: {
            type: 'string',
            description: 'When to show disassembly',
            enum: ['always', 'never', 'auto'],
            default: 'auto'
        },
        sourceFileMap: {
            type: 'object',
            description: 'Source file path mappings',
            default: {}
        },
        trace: {
            type: 'string',
            description: 'Enable logging of debug adapter protocol',
            enum: ['off', 'messages', 'verbose'],
            default: 'off'
        }
    },
    required: ['type', 'request', 'name', 'program']
};
export type TabId = 'general' | 'ocr' | 'storage' | 'about';
export type ToastKind = 'ok' | 'warn' | 'err';

export interface HostState {
  type: 'state';
  version?: string;
  config: { hotkey: string; debugShowStats: boolean; configOk: boolean; configError: string };
  runtime: {
    mainRunning: boolean;
    ocrAvailable: boolean;
    ocrLoaded: boolean;
    ocrError: boolean;
    hotkeyRegistered: boolean;
    hotkeyPaused: boolean;
  };
  storage: { portable: boolean; dataDir: string; configPath: string };
  ocr: {
    modelsFound: boolean;
    modelsDir: string;
    manifestOk: boolean;
    packVersion: string;
    manifestError: string;
    cacheBytes: number;
    cacheFiles: number;
  };
}

export type HostMessage =
  | HostState
  | { type: 'hotkeyResult'; ok: boolean; message: string; state?: HostState }
  | { type: 'actionResult'; ok: boolean; message: string; state?: HostState };

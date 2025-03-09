export class FamAPU {
    private static instance: FamAPU;
    private irqCallback = 0;
    private dmcCallback = 0;

    private constructor(private readonly module: any) {
    }
    public static async getAPU(): Promise<FamAPU> {
        if (!this.instance) {
            const wasmModule = await import('./wasm/apu.js');
            const module = await wasmModule.default();
            this.instance = new FamAPU(module);
        }
        return this.instance;
    }

    public setDmcCallback(callback?: (addr: number) => number) {
        if (this.dmcCallback) {
            this.module.removeFunction(this.dmcCallback);
        }
        if (callback) {
            this.dmcCallback = this.module.addFunction(callback, 'ii');
            this.module._setDmcCallback(this.dmcCallback);
        }
    }
    public setIrqCallback(callback?: (flag: number) => void) {
        if (this.irqCallback) {
            this.module.removeFunction(this.irqCallback);
        }
        if (callback) {
            this.irqCallback = this.module.addFunction(callback, 'vi');
            this.module._setIrqCallback(this.irqCallback);
        }
    }
    public writeMem(addr: number, data: number): void {
        this.module._writeMem(addr, data);
    }
    public readMem(addr: number): number {
        return this.module._readMem(addr);
    }
    /**
     * 1step(=240Hz)分の処理を行う
     * @param samples 1stepあたりのサンプル数（44100Hz=183 or 184, 48000Hz=200）
     */
    public step(samples: number): Uint8Array {
        const buf = this.module._step(samples);
        return new Uint8Array(this.module.HEAPU32.buffer, buf, samples);
    }
    /**
     * 0-255のボリュームの大きさを設定する
     * @param volume 0-255の範囲
     */
    public setVolume(volume: number): void {
        this.module._setVolume(volume);
    }
    public reset(): void {
        this.module._reset();
    }
    public powerOff(): void {
        this.module._powerOff();
    }
}
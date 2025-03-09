export class FamCPU {
    private static instance: FamCPU;
    private apuStepCallback = 0;
    private memReadCallback = 0;
    private memWriteCallback = 0;
    private debugCallback = 0;

    private constructor(private readonly module: any) {
    }
    public static async getCPU(): Promise<FamCPU> {
        if (!this.instance) {
            const wasmModule = await import('./wasm/cpu.js');
            const module = await wasmModule.default();
            this.instance = new FamCPU(module);
        }
        return this.instance;
    }
    public nmi(): void {
        this.module._nmi();
    }
    public reset(): void {
        this.module._reset();
    }
    public powerOff(): void {
        this.module._powerOff();
    }
    public irq(flag: number = 1): void {
        this.module._irq(flag);
    }
    public skip(cycle: number): void {
        this.module._skip(cycle);
    }
    public step(cycle: number): number {
        return this.module._step(cycle);
    }
    public setApuStepCallback(callback?: (cycle: number) => void) {
        if (this.apuStepCallback) {
            this.module.removeFunction(this.apuStepCallback);
        }
        if (callback) {
            this.apuStepCallback = this.module.addFunction(callback, 'vi');
            this.module._setApuStepCallback(this.apuStepCallback);
        } else {
            this.apuStepCallback = 0;
        }
    }
    public setMemReadCallback(callback?: (addr: number) => number) {
        if (this.memReadCallback) {
            this.module.removeFunction(this.memReadCallback);
        }
        if (callback) {
            this.memReadCallback = this.module.addFunction(callback, 'ii');
            this.module._setMemReadCallback(this.memReadCallback);
        } else {
            this.memReadCallback = 0;
        }
    }
    public setMemWriteCallback(callback?: (addr: number, data: number) => void) {
        if (this.memWriteCallback) {
            this.module.removeFunction(this.memWriteCallback);
        }
        if (callback) {
            this.memWriteCallback = this.module.addFunction(callback, 'vii');
            this.module._setMemWriteCallback(this.memWriteCallback);
        } else {
            this.memWriteCallback = 0;
        }
    }
    public setDebugCallback(callback?: (a: number, x: number, y: number, s: number, p: number, pc: number, cycle: number) => void) {
        if (this.debugCallback) {
            this.module.removeFunction(this.debugCallback);
        }
        if (callback) {
            this.debugCallback = this.module.addFunction(callback, 'viiiiiii');
            this.module._setDebugCallback(this.debugCallback);
        } else {
            this.debugCallback = 0;
        }
    }
    public getOperandText(addr: number): [string, number] {
        const next = this.module._makeOperandText(addr);
        const buf = this.module._getOperandText();
        const bytes = new Uint8Array(this.module.HEAPU8.buffer, buf);
        let length = 0;
        while (bytes[length] !== 0) length++;
        const text = new TextDecoder().decode(bytes.subarray(0, length));
        return [text, next];
    }
}
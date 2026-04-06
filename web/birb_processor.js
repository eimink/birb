/*
 * birb_processor.js — AudioWorklet processor
 * Drives the WASM synth engine at 128 samples per quantum.
 */
class BirbProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.ready = false;
        this.port.onmessage = (e) => this.handleMessage(e);
    }

    async handleMessage(e) {
        if (e.data.type === 'init') {
            const { wasmBytes, songData } = e.data;

            const module = await WebAssembly.instantiate(wasmBytes, {});
            this.wasm = module.instance.exports;
            this.memory = this.wasm.memory;

            /* upload song data into WASM memory */
            const songBufPtr = this.wasm.getSongBuf();
            const mem = new Uint8Array(this.memory.buffer);
            const song = new Uint8Array(songData);
            mem.set(song, songBufPtr);

            /* init the synth */
            const result = this.wasm.init(song.byteLength);
            if (result === 0) {
                this.outputPtr = this.wasm.getOutputBuf();
                this.ready = true;
                this.port.postMessage({ type: 'ready' });
            } else {
                this.port.postMessage({ type: 'error', message: 'init failed' });
            }
        }
    }

    process(inputs, outputs) {
        if (!this.ready) return true;

        const output = outputs[0][0]; /* mono */
        const len = output.length;    /* typically 128 */

        this.wasm.render(len);

        /* read int16 samples from WASM memory, convert to float */
        const samples = new Int16Array(this.memory.buffer, this.outputPtr, len);
        for (let i = 0; i < len; i++) {
            output[i] = samples[i] / 32768.0;
        }

        /* send sync data back to main thread */
        this.port.postMessage({
            type: 'sync',
            row: this.wasm.getRow(),
            pattern: this.wasm.getPattern()
        });

        return true;
    }
}

registerProcessor('birb-processor', BirbProcessor);

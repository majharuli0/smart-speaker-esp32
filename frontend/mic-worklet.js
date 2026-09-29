// Runs on the browser's audio thread: collects 320 samples (20 ms at 16 kHz)
// of mic audio as 16-bit PCM and hands each chunk to the page to send.
class Mic extends AudioWorkletProcessor {
  constructor() {
    super();
    this.buf = new Int16Array(320);
    this.n = 0;
  }

  process([input]) {
    const ch = input[0] || [];
    for (let i = 0; i < ch.length; i++) {
      this.buf[this.n++] = Math.max(-1, Math.min(1, ch[i])) * 32767;
      if (this.n === 320) {
        this.port.postMessage(this.buf.buffer, [this.buf.buffer]);
        this.buf = new Int16Array(320);
        this.n = 0;
      }
    }
    return true;
  }
}

registerProcessor('mic', Mic);

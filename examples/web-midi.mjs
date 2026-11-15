// This module uses any MIDIInput/MIDIOutput pair obtained with SysEx access.
// Speed is a bridge report. It does not identify the attached DIN instrument.
const prefix = [0xf0, 0x00, 0x20, 0x3c, 0x04, 0x00];
const statusQuery = [0xf0, 0x7d, 0x4d, 0x43, 0x4c, 0x01, 0x01, 0xf7];
const speeds = [null, 1, 2, 10 / 3, 4, 5, 20 / 3, 8, 10, 40 / 3, 16, 20];
const limits = new Map([[1, 1], [2, 2], [4, 4], [8, 7]]);

export function readTurboSpeed(data) {
  if (data.length === 9 && prefix.every((byte, index) => data[index] === byte) &&
      data[6] === 2 && data[8] === 0xf7) {
    return speeds[data[7]] ?? null;
  }
  return null;
}

export function attachTurboPort(input, output, { onMidi, onSpeed } = {}) {
  const receive = ({ data, timeStamp }) => {
    const speed = readTurboSpeed(data);
    if (speed !== null) {
      onSpeed?.(speed, timeStamp);
    } else {
      onMidi?.(data, timeStamp);
    }
  };
  input.addEventListener("midimessage", receive);
  const send = (message, timeStamp) => output.send(message, timeStamp);
  let closed = false;
  const control = (bytes) => {
    if (closed) throw new Error("Turbo port is closed");
    send([...prefix, ...bytes, 0xf7]);
  };
  return {
    querySpeed() {
      if (closed) throw new Error("Turbo port is closed");
      send(statusQuery);
    },
    setLimit(rate) {
      const code = limits.get(rate);
      if (code === undefined) throw new RangeError("Turbo limit must be 1, 2, 4, or 8");
      control([3, code]);
    },
    send(message, timeStamp) {
      if (closed) throw new Error("Turbo port is closed");
      send(message, timeStamp);
    },
    close() {
      closed = true;
      input.removeEventListener("midimessage", receive);
    }
  };
}

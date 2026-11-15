import { readTurboSpeed } from "./web-midi.mjs";

const encoderPrefix = [0xf0, 0x7d, 0x4d, 0x43, 0x4c, 0x02, 0x01];

export function isInstrumentSysEx(data) {
  if (data.length < 2 || data[0] !== 0xf0 || data[data.length - 1] !== 0xf7) return false;
  if (readTurboSpeed(data) !== null) return false;
  // Ignore optional bridge panel reports, which share the first MIDI input.
  return !(data.length === 14 && encoderPrefix.every((byte, index) => data[index] === byte) &&
           data[7] < 128 && data[8] < 16 && data.subarray(9, 13).every(byte => byte > 0 && byte < 128));
}

export function mountCapturePage(root, requestAccess, download) {
  const enable = root.querySelector("#enable");
  const accessStatus = root.querySelector("#access-status");
  const routes = Array.from({ length: 2 }, (_, index) => {
    const panel = root.querySelector("#route").content.firstElementChild.cloneNode(true);
    const number = index + 1;
    panel.querySelector("legend").textContent = `Capture ${number}`;
    root.querySelector("#routes").append(panel);
    return {
      number, select: panel.querySelector("select"), arm: panel.querySelector(".arm"),
      stop: panel.querySelector(".stop"), save: panel.querySelector(".save"),
      status: panel.querySelector(".status"), input: null, frames: [], bytes: 0,
    };
  });

  let midi;
  const selectedInput = route => midi?.inputs.get(route.select.value);

  function refreshButtons() {
    for (const route of routes) {
      const input = selectedInput(route);
      route.select.disabled = route.input !== null;
      route.arm.disabled = route.input !== null || input?.state !== "connected" ||
        routes.some(other => other.input === input);
      route.stop.disabled = route.input === null;
      route.save.disabled = route.input !== null || route.frames.length === 0;
    }
  }

  function showCount(route, message) {
    route.status.textContent = `${message} · ${route.frames.length} frames, ${route.bytes} bytes`;
  }

  function stop(route, message) {
    route.input.removeEventListener("midimessage", route.receive);
    route.input = null;
    showCount(route, message);
    refreshButtons();
  }

  function refreshInputs() {
    for (const route of routes) {
      const selected = route.select.value;
      if (route.input && (route.input.state !== "connected" ||
          midi.inputs.get(route.input.id) !== route.input)) {
        stop(route, "Input disconnected. Capture may be incomplete");
      }
      route.select.options.length = 1;
      for (const input of midi.inputs.values()) {
        if (input.state !== "connected") continue;
        const option = root.createElement("option");
        option.value = input.id;
        option.textContent = `${input.name || "MIDI input"} (${input.id})`;
        route.select.append(option);
      }
      route.select.value = [...route.select.options].some(option => option.value === selected)
        ? selected : "";
    }
    refreshButtons();
  }

  for (const route of routes) {
    route.receive = ({ data }) => {
      if (!isInstrumentSysEx(data)) return;
      route.frames.push(data.slice());
      route.bytes += data.length;
      showCount(route, "Capturing");
    };
    route.select.addEventListener("change", refreshButtons);
    route.arm.addEventListener("click", () => {
      if (route.arm.disabled) return;
      route.input = selectedInput(route);
      route.frames = [];
      route.bytes = 0;
      route.input.addEventListener("midimessage", route.receive);
      showCount(route, "Capturing");
      refreshButtons();
    });
    route.stop.addEventListener("click", () => stop(route, "Stopped"));
    route.save.addEventListener("click", () => {
      const data = new Blob(route.frames, { type: "application/octet-stream" });
      download(data, `midi-capture-${route.number}.syx`);
    });
  }

  enable.addEventListener("click", async () => {
    enable.disabled = true;
    try {
      midi = await requestAccess({ sysex: true });
      midi.addEventListener("statechange", refreshInputs);
      refreshInputs();
      accessStatus.textContent = "MIDI enabled. Select the input for the DIN pair you want to capture.";
    } catch (error) {
      accessStatus.textContent = `MIDI unavailable: ${error.message}`;
      enable.disabled = false;
    }
  });
}

if (typeof document !== "undefined") {
  mountCapturePage(document, options => {
    if (!navigator.requestMIDIAccess) throw new Error("Open this page in a browser with Web MIDI on localhost or HTTPS.");
    return navigator.requestMIDIAccess(options);
  }, (blob, filename) => {
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = filename;
    link.click();
    setTimeout(() => URL.revokeObjectURL(link.href), 1000);
  });
}

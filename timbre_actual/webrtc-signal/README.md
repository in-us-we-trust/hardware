# Timbre final call UI

This folder contains the browser-side waiting page and the signaling server used during the call flow.

The final UX is intentionally simple:

- there is no “Connect” button
- there is no “Call” button
- the page is always waiting for the timbre button to be pressed
- the PC only has one control: “Colgar”
- the page displays the live stream from the timbre camera

## 1) Project path

```bash
~/Escritorio/TPP2/hardware-20260830T075942Z-1-001/hardware/timbre
```

## 2) Start the signaling server

```bash
cd ~/Escritorio/TPP2/hardware-20260830T075942Z-1-001/hardware/timbre/webrtc-signal
npm install
npm start
```

This will serve the browser page at:

```text
http://localhost:8080/
```

## 3) Open the browser page

Open the URL in Chrome or Edge:

```text
http://localhost:8080/
```

The page is designed to stay idle while the timbre waits for the button press.

## 4) Set the ESP32 IP

In the page, edit the field labeled “ESP32” and put the IP of the timbre device, for example:

```text
http://192.168.1.46:8081
```

Then the page will poll the timbre stream at:

```text
http://192.168.1.46:8081/stream
```

## 5) Expected flow

1. The browser page is open and waiting.
2. The user presses the timbre button once.
3. The timbre starts sending the stream on port 8081.
4. The page shows the live image from the timbre camera.
5. The browser only has the “Colgar” button.
6. Pressing “Colgar” calls `/hangup` on the timbre.
7. The timbre returns to rest mode and the stream stops.

## 6) Important behavior

- one button press starts the call
- repeated button bounces are ignored
- extra button presses while in call do nothing
- only the PC hangs up the call
- after hangup, the timbre returns to idle

## 7) Current status

This is the practical final UX for the current project state:

- video stream is served by the timbre on port 8081
- the browser page waits and displays it
- the PC ends the call through “Colgar”
- the final true WebRTC bidirectional audio/ESP32-native stack is a separate engineering step beyond the current demo path

## 8) Validation in this environment

I verified that the Node signaling server starts and serves the page successfully in this environment with:

```text
Signal server running at http://localhost:8080
```

The remaining limitation is the firmware toolchain in the local environment, not the browser-side logic.

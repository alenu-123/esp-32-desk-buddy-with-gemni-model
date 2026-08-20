"""
Gemini Live voice session (mic in / speaker out on your PC)
with live transcripts forwarded to an ESP32 OLED display.

You can now ALSO just type your question into the terminal instead of
speaking - it gets sent to Gemini as a text turn, and shows up on the
ESP32 transcript screen exactly like a spoken question would.

Install dependencies first:
    pip install google-genai sounddevice numpy requests

sounddevice ships prebuilt wheels for Windows/macOS/Linux, so no compiler
or extra build tools are needed (unlike pyaudio).

Set your Gemini API key as an environment variable before running:
    export GEMINI_API_KEY="your-key-here"        # macOS/Linux
    setx GEMINI_API_KEY "your-key-here"           # Windows (new terminal after)

Update ESP32_IP below to match your board's IP (check Serial Monitor after upload).
"""

import asyncio
import os
import queue
import requests
import numpy as np
import sounddevice as sd
from google import genai
from google.genai import types

# ---------------- Config ----------------
ESP32_IP = "10.91.73.100"          # <-- change to your ESP32's IP
ESP32_URL = f"http://{ESP32_IP}/text"
BUTTON_URL = f"http://{ESP32_IP}/button"
BUTTON_POLL_INTERVAL = 0.05  # seconds; how often to check if the button is held

MODEL = "gemini-3.1-flash-live-preview"  # check ai.google.dev for the latest live model name

CHANNELS = 1
SEND_SAMPLE_RATE = 16000     # mic -> Gemini expects 16kHz PCM
RECEIVE_SAMPLE_RATE = 24000  # Gemini -> speaker audio comes back at 24kHz
CHUNK_SIZE = 512             # smaller = lower latency (was 1024)

client = genai.Client(api_key=os.environ.get("GEMINI_API_KEY"))

CONFIG = types.LiveConnectConfig(
    response_modalities=["AUDIO"],
    input_audio_transcription=types.AudioTranscriptionConfig(),
    output_audio_transcription=types.AudioTranscriptionConfig(),
)

# thread-safe queue bridging sounddevice's callback thread -> asyncio
_mic_queue: "queue.Queue[bytes]" = queue.Queue()


def _mic_callback(indata, frames, time_info, status):
    if status:
        print(f"[mic status] {status}")
    _mic_queue.put(bytes(indata))


def _get_button_state_blocking() -> bool:
    """Blocking network call - only ever run via asyncio.to_thread. Returns
    True while the ESP32 boot button is being held (push-to-talk active)."""
    try:
        resp = requests.get(BUTTON_URL, timeout=0.3)
        return resp.text.strip() == "1"
    except requests.exceptions.RequestException:
        return False  # if the board is unreachable, fail safe: not recording


def _post_to_esp32_blocking(text: str):
    """Blocking network call - only ever run via asyncio.to_thread, never awaited directly."""
    try:
        requests.post(
            ESP32_URL,
            data=text.encode("utf-8"),
            headers={"Content-Type": "text/plain"},
            timeout=0.3,
        )
    except requests.exceptions.RequestException as e:
        print(f"[esp32 send failed] {e}")


def send_to_esp32(text: str):
    """Fire-and-forget from async code: schedules the blocking POST on a worker
    thread so it never stalls the asyncio event loop (which is also handling
    live mic/speaker audio). Safe to call from sync or async contexts."""
    try:
        loop = asyncio.get_running_loop()
        loop.create_task(asyncio.to_thread(_post_to_esp32_blocking, text))
    except RuntimeError:
        # no running loop (e.g. called before the event loop starts) - just do it directly
        _post_to_esp32_blocking(text)


class LiveSession:
    def __init__(self):
        self.audio_in_queue = asyncio.Queue()
        self.out_queue = asyncio.Queue(maxsize=20)
        self.session = None

    async def listen_mic(self):
        stream = sd.RawInputStream(
            samplerate=SEND_SAMPLE_RATE,
            channels=CHANNELS,
            dtype="int16",
            blocksize=CHUNK_SIZE,
            latency="low",
            callback=_mic_callback,
        )
        stream.start()
        try:
            while True:
                data = await asyncio.to_thread(_mic_queue.get)
                await self.out_queue.put({"data": data, "mime_type": "audio/pcm"})
        finally:
            stream.stop()
            stream.close()

    async def send_audio(self):
        while True:
            msg = await self.out_queue.get()
            await self.session.send_realtime_input(
                audio=types.Blob(data=msg["data"], mime_type=f"audio/pcm;rate={SEND_SAMPLE_RATE}")
            )

    async def read_text_input(self):
        """Reads lines typed in the terminal and sends each as a text turn
        to Gemini, in parallel with whatever the mic is doing. Type a
        question and hit Enter any time; type 'q' or 'quit' to exit cleanly."""
        print("Type a question and press Enter to send it as text (or just talk).")
        print("Type 'q' to quit.\n")
        while True:
            text = await asyncio.to_thread(input, "You (typed): ")
            text = text.strip()
            if not text:
                continue
            if text.lower() in ("q", "quit", "exit"):
                print("Exiting...")
                os._exit(0)  # blunt but reliable way to tear down all tasks + streams

            # Echo immediately so it's visible locally and on the ESP32,
            # since typed input doesn't come back through input_transcription.
            print(f"You: {text}")
            send_to_esp32(f"You: {text}")

            await self.session.send_client_content(
                turns=types.Content(role="user", parts=[types.Part(text=text)]),
                turn_complete=True,
            )

    async def receive_responses(self):
        while True:
            turn = self.session.receive()
            async for response in turn:
                server_content = response.server_content
                if server_content is None:
                    continue

                # Audio playback data
                if server_content.model_turn:
                    for part in server_content.model_turn.parts:
                        if part.inline_data:
                            self.audio_in_queue.put_nowait(part.inline_data.data)

                # Transcripts (spoken input only - typed input is echoed separately)
                if server_content.input_transcription and server_content.input_transcription.text:
                    text = server_content.input_transcription.text
                    print(f"You: {text}")
                    send_to_esp32(f"You: {text}")

                if server_content.output_transcription and server_content.output_transcription.text:
                    text = server_content.output_transcription.text
                    print(f"Gemini: {text}")
                    send_to_esp32(f"Gemini: {text}")

            # drain any leftover queued audio when turn completes (barge-in handling)
            while not self.audio_in_queue.empty():
                self.audio_in_queue.get_nowait()

    async def play_audio(self):
        stream = sd.RawOutputStream(
            samplerate=RECEIVE_SAMPLE_RATE,
            channels=CHANNELS,
            dtype="int16",
            blocksize=CHUNK_SIZE,
            latency="low",
        )
        stream.start()
        try:
            while True:
                data = await self.audio_in_queue.get()
                await asyncio.to_thread(stream.write, data)
        finally:
            stream.stop()
            stream.close()

    async def run(self):
        async with client.aio.live.connect(model=MODEL, config=CONFIG) as session:
            self.session = session
            print("Connected to Gemini Live. Start talking, or type a question...")
            send_to_esp32("Connected. Talk or type.")

            async with asyncio.TaskGroup() as tg:
                tg.create_task(self.listen_mic())
                tg.create_task(self.send_audio())
                tg.create_task(self.receive_responses())
                tg.create_task(self.play_audio())
                tg.create_task(self.read_text_input())


if __name__ == "__main__":
    if not os.environ.get("GEMINI_API_KEY"):
        raise SystemExit("Set GEMINI_API_KEY environment variable first.")

    try:
        asyncio.run(LiveSession().run())
    except KeyboardInterrupt:
        print("\nStopped.")

import argparse
import glob
import serial
import struct
import sys
import time

try:
    import sounddevice as sd
except ImportError:
    print("Error: sounddevice module is required. Install it with 'python3 -m pip install sounddevice'.")
    sys.exit(1)

import numpy as np

DEFAULT_SAMPLE_RATE = 16000
DEFAULT_DURATION = 1.0
CHUNK_SIZE = 1024

parser = argparse.ArgumentParser(description="Stream microphone audio to STM32 for keyword recognition")
parser.add_argument('--duration', type=float, default=DEFAULT_DURATION, help='Seconds to record per query')
parser.add_argument('--samplerate', type=int, default=DEFAULT_SAMPLE_RATE, help='Audio sample rate')
parser.add_argument('--port', type=str, default=None, help='Serial port to use')
args = parser.parse_args()

ports = glob.glob("/dev/cu.usbmodem*")
if not ports and not args.port:
    print("Error: No STM32 device found matching /dev/cu.usbmodem*. Provide --port if necessary.")
    sys.exit(1)

port_name = args.port if args.port else ports[0]
print(f"Connecting to {port_name}...")

ser = serial.Serial(port_name, 115200, timeout=0.1)
ser.reset_input_buffer()
ser.reset_output_buffer()

print("Waiting for STM32 'READY' signal...")
while True:
    if ser.in_waiting > 0:
        line = ser.readline().decode('utf-8', errors='ignore')
        print(line, end="")
        if "READY_TO_RECEIVE" in line:
            print("\n✅ STM32 is READY. Starting audio stream...")
            break
    time.sleep(0.01)

print(f"Recording {args.duration} seconds @ {args.samplerate} Hz")

while True:
    print("Recording...")
    audio = sd.rec(int(args.duration * args.samplerate), samplerate=args.samplerate, channels=1, dtype='int16')
    sd.wait()
    pcm = audio.flatten().tobytes()

    if hasattr(ser, 'read_all'):
        ser.read_all()
    else:
        ser.read(ser.in_waiting)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    header = b'\xAA\x55' + struct.pack('<H', len(pcm))
    print("Sending audio packet...", end="", flush=True)
    ser.write(header)
    ser.flush()
    time.sleep(0.01)

    chunk_size = 256
    for i in range(0, len(pcm), chunk_size):
        ser.write(pcm[i:i+chunk_size])
        time.sleep(0.005)

    ser.flush()
    print(" sent. Waiting for inference result...")

    start_time = time.time()
    rx_buffer = bytearray()

    while (time.time() - start_time) < 20.0:
        if ser.in_waiting > 0:
            rx_buffer.extend(ser.read(ser.in_waiting))
            idx = rx_buffer.find(b'\xBB\x66')
            if idx != -1 and idx + 2 < len(rx_buffer):
                code = rx_buffer[idx + 2]
                if code == 0:
                    label = 'NO'
                elif code == 1:
                    label = 'YES'
                elif code == 2:
                    label = 'UNKNOWN'
                else:
                    label = f'CODE_{code}'
                print(f"🤖 INFERENCE: {label}")
                end_idx = rx_buffer.find(b'\xBB\x67', idx + 3)
                if end_idx != -1 and end_idx + 2 < len(rx_buffer):
                    leaf = rx_buffer[end_idx + 2]
                    print(f"📌 Branch path: {leaf:02b}")
                print("")
                break
    else:
        print("Timeout waiting for STM32 response.")
    time.sleep(0.5)

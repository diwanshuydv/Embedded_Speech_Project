import cv2, serial, struct, time, numpy as np, glob, sys

# Find the appropriate serial port automatically
ports = glob.glob("/dev/cu.usbmodem*")
if not ports:
    print("Error: No STM32 device found matching /dev/cu.usbmodem*")
    sys.exit(1)

port_name = ports[0]
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
            print("\n✅ STM32 is READY. Starting Stream...")
            break
    time.sleep(0.01)

# 🔥 FIX 1: Open the camera exactly ONCE outside the loop.
cap = cv2.VideoCapture(0)
# Optional: Ask OpenCV to minimize its internal ring buffer
cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

# Give the hardware sensor 2 seconds to turn on, adjust auto-exposure, and fix white-balance
print("Warming up camera...")
time.sleep(2) 

while True:
    # 🔥 FIX 2: Drain the OS ring buffer to guarantee we get a real-time frame
    # Calling grab() is extremely fast because it doesn't decode the image pixels
    for _ in range(5):
        cap.grab()
    
    ret, frame = cap.read()
    
    if not ret: 
        print("Camera read failed.")
        time.sleep(1)
        continue

    # Preprocess (This will now consistently convert BGR -> RGB without glitching)
    rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
    small = cv2.resize(rgb, (96, 96))
    img_data = small.flatten().tobytes()

    # Forcefully drain any leftover bytes in the OS queue
    if hasattr(ser, 'read_all'):
        ser.read_all()
    else:
        ser.read(ser.in_waiting)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # 📤 SEND HEADER
    header = b'\xAA\x55' + struct.pack('<H', len(img_data))
    print(f"Sending Frame...", end="", flush=True)
    ser.write(header)
    ser.flush()
    time.sleep(0.05) 
    
    # 📤 SEND ALL PIXELS IN CHUNKS
    chunk_size = 128
    for i in range(0, len(img_data), chunk_size):
        ser.write(img_data[i:i+chunk_size])
        time.sleep(0.011)
        
    ser.flush()
    print(" Sent. Waiting for AI...")

    # 📥 WAIT FOR AI RESULT
    start_time = time.time()
    rx_buffer = bytearray()
    
    while (time.time() - start_time) < 20.0:
        if ser.in_waiting > 0:
            rx_buffer.extend(ser.read(ser.in_waiting))
            if b'\xBB\x66' in rx_buffer:
                idx = rx_buffer.find(b'\xBB\x66')
                if idx + 2 < len(rx_buffer):
                    res = "PERSON" if rx_buffer[idx+2] == 1 else "NONE"
                    print(f"🤖 AI RESULT: {res}\n")
                    break
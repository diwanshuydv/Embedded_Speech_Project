import cv2, serial, struct, time, numpy as np
#For Linux:
# ser = serial.Serial("/dev/ttyACM0", 115200, timeout=0.1)
#For Mac:
ser = serial.Serial("/dev/cu.usbmodem11403", 115200, timeout=0.1)
ser.reset_input_buffer()
ser.reset_output_buffer()
#For phone app based:
# cap = cv2.VideoCapture("http://172.31.75.172:8080/video") # Ensure this IP is still correct
#For laptop camera based:
cap = cv2.VideoCapture(0)
print("Waiting for STM32 'READY' signal...")
while True:
    if ser.in_waiting > 0:
        line = ser.readline().decode('utf-8', errors='ignore')
        print(line, end="")
        if "READY_TO_RECEIVE" in line:
            print("\n✅ STM32 is READY. Starting Stream...")
            break
    time.sleep(0.01)

while True:
    # Flush the OpenCV buffer to get the freshest frame
    for _ in range(5):
        cap.grab()
    ret, frame = cap.read()
    if not ret: break

    # Preprocess
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    small = cv2.resize(gray, (96, 96))
    img_data = small.flatten().tobytes()

    ser.reset_input_buffer()

    # 📤 SEND HEADER
    header = b'\xAA\x55' + struct.pack('<H', len(img_data))
    print(f"Sending Frame...", end="", flush=True)
    ser.write(header)
    
    # 📤 SEND PIXELS IN CHUNKS (This is the fix)
    chunk_size = 512
    for i in range(0, len(img_data), chunk_size):
        ser.write(img_data[i:i+chunk_size])
        ser.flush()
        time.sleep(0.005) # 5ms delay allows STM32 hardware buffer to clear
        
    print(" Sent. Waiting for AI...")

    # 📥 WAIT FOR AI RESULT (Max 5 seconds)
    start_time = time.time()
    rx_buffer = bytearray()
    
    while (time.time() - start_time) < 5.0:
        if ser.in_waiting > 0:
            rx_buffer.extend(ser.read(ser.in_waiting))
            if b'\xBB\x66' in rx_buffer:
                idx = rx_buffer.find(b'\xBB\x66')
                if idx + 2 < len(rx_buffer):
                    res = "PERSON" if rx_buffer[idx+2] == 1 else "NONE"
                    print(f"🤖 AI RESULT: {res}\n")
                    break
    
    time.sleep(0.1) # Shorter delay for a better framerate
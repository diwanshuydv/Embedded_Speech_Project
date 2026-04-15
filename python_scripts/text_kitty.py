#!/usr/bin/env python3
"""
Text Kitty — Text-based Shape/Image Controller
If the command matches a shape → draws it. Otherwise → searches web for an image
and streams it to the STM32 LCD.

Usage:
    python3 text_kitty.py [--port /dev/cu.usbmodemXXXX]
"""

import argparse
import glob
import io
import struct
import sys
import time

try:
    import serial
except ImportError:
    print("Error: pyserial is required. Install with: pip install pyserial")
    sys.exit(1)

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is required. Install with: pip install Pillow")
    sys.exit(1)

try:
    import requests
except ImportError:
    print("Error: requests is required. Install with: pip install requests")
    sys.exit(1)


# ============================================================
#  Constants
# ============================================================
IMG_W = 160   # Image width to send
IMG_H = 120   # Image height to send

# Serial protocol
SYNC = b'\xAA\x55'
CMD_WAKE = 0x01
CMD_SHAPE = 0x02
CMD_CLEAR = 0x03
CMD_TIMEOUT = 0x04
CMD_IMAGE = 0x05

# Shape mapping
SHAPE_MAP = {
    "square":    0x00,
    "circle":    0x01,
    "triangle":  0x02,
    "rectangle": 0x03,
    "star":      0x04,
    "diamond":   0x05,
    "pentagon":  0x06,
    "hexagon":   0x07,
    "heart":     0x08,
    "arrow":     0x09,
    "cross":     0x0A,
    "plus":      0x0A,
    "ellipse":   0x0B,
    "oval":      0x0B,
}

# ANSI colors
class C:
    CYAN = "\033[96m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    RED = "\033[91m"
    MAGENTA = "\033[95m"
    BLUE = "\033[94m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    END = "\033[0m"


# ============================================================
#  Parsing Helpers
# ============================================================
def parse_shape(text: str):
    text = text.lower()
    for name, shape_id in SHAPE_MAP.items():
        if name in text:
            return shape_id, name
    return None, None

def extract_search_query(text: str) -> str:
    text = text.lower().strip()
    for prefix in ["show me", "show", "display", "find", "search for",
                   "search", "draw", "draw me", "get", "get me",
                   "look up", "look for", "show me a", "show a",
                   "i want", "i want to see", "can you show"]:
        if text.startswith(prefix):
            text = text[len(prefix):].strip()
            break
    for article in ["a ", "an ", "the ", "some "]:
        if text.startswith(article):
            text = text[len(article):]
    return text.strip()


# ============================================================
#  Image Search & Processing
# ============================================================
def search_image(query: str) -> str:
    """Search for an image URL. Returns image URL or None."""
    import urllib.parse
    import re
    import json
    
    headers = {
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
    }

    # 1st attempt: Wikipedia API
    try:
        wiki_url = f"https://en.wikipedia.org/w/api.php?action=query&titles={urllib.parse.quote(query)}&prop=pageimages|images&piprop=original&pilicense=any&format=json"
        resp = requests.get(wiki_url, headers=headers, timeout=5)
        data = resp.json()
        pages = data.get("query", {}).get("pages", {})
        for page_id, page_info in pages.items():
            if page_id != "-1" and "original" in page_info:
                url = page_info["original"].get("source")
                if url:
                    return url
    except Exception as e:
        print(f"  {C.DIM}Wiki scrape error: {e}{C.END}")

    # 2nd attempt: Bing Images scrape
    try:
        import html
        url = f"https://www.bing.com/images/search?q={urllib.parse.quote(query)}&first=1"
        resp = requests.get(url, headers=headers, timeout=5)
        text = html.unescape(resp.text)
        
        matches = re.findall(r'"murl":"(https?://[^"]+)"', text)
        if not matches:
            matches = re.findall(r'm="{.*?\"murl\":\"(https?://.*?)\"', resp.text)
            
        for u in matches:
            if u.lower().endswith((".jpg", ".png", ".jpeg", ".webp")):
                return u
        
        urls = re.findall(r'https?://[^"\'<>&;\s]+\.(?:jpg|jpeg|png|webp)', text)
        for u in urls:
             if "bing.com" not in u and "microsoft.com" not in u:
                 return u
    except Exception as e:
        print(f"  {C.YELLOW}Bing scrape error: {e}{C.END}")
        
    return None

def download_image(url: str) -> Image.Image:
    headers = {
        "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                      "AppleWebKit/537.36 Chrome/120.0.0.0 Safari/537.36"
    }
    resp = requests.get(url, headers=headers, timeout=10, stream=True)
    resp.raise_for_status()
    return Image.open(io.BytesIO(resp.content))

def image_to_rgb565(img: Image.Image, width: int, height: int) -> bytes:
    img = img.convert("RGB")
    img_ratio = img.width / img.height
    target_ratio = width / height
    
    if img_ratio > target_ratio:
        new_h = img.height
        new_w = int(new_h * target_ratio)
        left = (img.width - new_w) // 2
        img = img.crop((left, 0, left + new_w, new_h))
    else:
        new_w = img.width
        new_h = int(new_w / target_ratio)
        top = (img.height - new_h) // 2
        img = img.crop((0, top, new_w, top + new_h))
    
    img = img.resize((width, height), Image.LANCZOS)
    
    data = bytearray(width * height * 2)
    pixels = img.load()
    idx = 0
    for y in range(height):
        for x in range(width):
            r, g, b = pixels[x, y]
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            data[idx] = (rgb565 >> 8) & 0xFF
            data[idx + 1] = rgb565 & 0xFF
            idx += 2
    return bytes(data)


# ============================================================
#  Serial Communication
# ============================================================
class STM32Link:
    def __init__(self, port: str, baudrate: int = 115200):
        self.ser = serial.Serial(port, baudrate, timeout=0.1)
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
    
    def wait_ready(self, timeout: float = 10.0):
        print(f"{C.CYAN}⏳ Waiting for STM32 ready signal...{C.END}")
        start = time.time()
        while time.time() - start < timeout:
            if self.ser.in_waiting > 0:
                line = self.ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    print(f"  {C.DIM}STM32: {line}{C.END}")
                if "HEYKITTY_READY" in line or "READY" in line:
                    print(f"{C.GREEN}✓ STM32 is ready!{C.END}")
                    return True
            time.sleep(0.01)
        print(f"{C.RED}✗ Timeout waiting for STM32.{C.END}")
        return False
    
    def send_wake(self):
        self.ser.write(SYNC + bytes([CMD_WAKE]))
        self.ser.flush()
    
    def send_shape(self, shape_id: int):
        self.ser.write(SYNC + bytes([CMD_SHAPE, shape_id]))
        self.ser.flush()
    
    def send_timeout(self):
        self.ser.write(SYNC + bytes([CMD_TIMEOUT]))
        self.ser.flush()
    
    def send_clear(self):
        self.ser.write(SYNC + bytes([CMD_CLEAR]))
        self.ser.flush()
    
    def send_image(self, pixel_data: bytes, width: int, height: int):
        header = SYNC + bytes([CMD_IMAGE, width, height])
        self.ser.write(header)
        self.ser.flush()
        time.sleep(0.1)
        
        total = len(pixel_data)
        chunk_size = 128
        sent = 0
        start = time.time()
        
        while sent < total:
            end = min(sent + chunk_size, total)
            self.ser.write(pixel_data[sent:end])
            sent = end
            time.sleep(0.011)
            
            pct = sent * 100 // total
            bar = "█" * (pct // 2) + "░" * (50 - pct // 2)
            sys.stdout.write(f"\r  📡 [{bar}] {pct}% ({sent}/{total} bytes)")
            sys.stdout.flush()
        
        self.ser.flush()
        elapsed = time.time() - start
        print(f"\n  {C.GREEN}✓ Image sent in {elapsed:.1f}s{C.END}")
    
    def drain_debug(self):
        if self.ser.in_waiting > 0:
            data = self.ser.read(self.ser.in_waiting)
            try:
                text = data.decode('utf-8', errors='ignore').strip()
                if text and not text.startswith('\xbb'):
                    for line in text.split('\n'):
                        line = line.strip()
                        if line:
                            print(f"  {C.CYAN}[STM32] {line}{C.END}")
            except:
                pass


# ============================================================
#  Main
# ============================================================
def main():
    parser = argparse.ArgumentParser(
        description="Text Kitty — Text-controlled LCD shape/image display",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument('--port', type=str, default=None,
                        help='Serial port (auto-detected if omitted)')
    args = parser.parse_args()
    
    # Header
    print(f"\n{C.BOLD}{C.CYAN}╔═══════════════════════════════════════════╗{C.END}")
    print(f"{C.BOLD}{C.CYAN}║    📝 Text Kitty — Shape + Image Display  ║{C.END}")
    print(f"{C.BOLD}{C.CYAN}║     Type commands to display on STM32     ║{C.END}")
    print(f"{C.BOLD}{C.CYAN}╚═══════════════════════════════════════════╝{C.END}\n")
    
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports and not args.port:
        print(f"{C.RED}✗ No STM32 found. Provide --port.{C.END}")
        sys.exit(1)
    
    port_name = args.port if args.port else ports[0]
    print(f"{C.CYAN}🔌 Connecting to {port_name}...{C.END}")
    
    stm32 = STM32Link(port_name)
    if not stm32.wait_ready():
        print(f"{C.YELLOW}⚠ Proceeding without READY signal.{C.END}")
    
    print(f"\n{C.GREEN}{'='*55}{C.END}")
    print(f"{C.GREEN}  ✅ Ready!{C.END}")
    print(f"{C.GREEN}  📐 Shapes: square, circle, triangle, star, etc.{C.END}")
    print(f"{C.GREEN}  🌐 Otherwise: web image search & display!{C.END}")
    print(f"{C.GREEN}  Type 'quit' or 'exit' to stop.{C.END}")
    print(f"{C.GREEN}{'='*55}{C.END}\n")
    
    while True:
        try:
            print(f"{C.CYAN}📝 Enter command:{C.END}", end=" ")
            command_text = input().strip()
            if command_text.lower() in ["quit", "exit", "q"]:
                raise KeyboardInterrupt
            if not command_text:
                continue

            # Wake up sequence
            stm32.send_wake()
            time.sleep(0.3)
            stm32.drain_debug()
            
            # Check if it's a shape
            shape_id, shape_name = parse_shape(command_text)
            if shape_id is not None:
                print(f"{C.BOLD}{C.MAGENTA}🎨 Drawing: {shape_name.upper()}{C.END}")
                stm32.send_shape(shape_id)
                time.sleep(0.1)
                stm32.drain_debug()
                print(f"{C.CYAN}  Shape displayed for 10 seconds.{C.END}")
                time.sleep(2.0)
            else:
                # NOT a shape → search web for image!
                query = extract_search_query(command_text)
                if not query:
                    query = command_text
                
                print(f"{C.BOLD}{C.BLUE}🔍 Searching web for: \"{query}\"{C.END}")
                
                image_url = search_image(query)
                if image_url:
                    print(f"  {C.DIM}URL: {image_url[:80]}...{C.END}")
                    print(f"  {C.CYAN}⬇ Downloading image...{C.END}")
                    
                    try:
                        pil_img = download_image(image_url)
                        print(f"  {C.GREEN}✓ Downloaded ({pil_img.width}x{pil_img.height}){C.END}")
                        
                        print(f"  {C.CYAN}🔄 Converting to {IMG_W}x{IMG_H} RGB565...{C.END}")
                        pixel_data = image_to_rgb565(pil_img, IMG_W, IMG_H)
                        
                        print(f"  {C.CYAN}📡 Streaming to LCD...{C.END}")
                        stm32.send_image(pixel_data, IMG_W, IMG_H)
                        
                        time.sleep(0.5)
                        stm32.drain_debug()
                        print(f"{C.BOLD}{C.GREEN}  🖼 \"{query}\" displayed on LCD for 10 seconds!{C.END}")
                        time.sleep(2.0)
                        
                    except Exception as e:
                        print(f"  {C.RED}✗ Image error: {e}{C.END}")
                        stm32.send_timeout()
                else:
                    print(f"  {C.RED}✗ No images found for \"{query}\"{C.END}")
                    stm32.send_timeout()
            
            time.sleep(0.5)
            stm32.drain_debug()
            print()
            
        except EOFError:
            print(f"\n{C.YELLOW}👋 Goodbye!{C.END}")
            break
        except KeyboardInterrupt:
            print(f"\n{C.YELLOW}👋 Goodbye!{C.END}")
            try:
                stm32.send_clear()
            except:
                pass
            sys.exit(0)
        except Exception as e:
            print(f"{C.RED}Error: {e}{C.END}")
            import traceback
            traceback.print_exc()
            time.sleep(1.0)


if __name__ == "__main__":
    main()

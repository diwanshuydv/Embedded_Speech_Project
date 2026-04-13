extern "C" {
    #include <stdint.h>
    #include <stdio.h>
    #include <libopencm3/stm32/rcc.h>
    #include <libopencm3/stm32/gpio.h>
    #include <libopencm3/stm32/usart.h>
    #include <libopencm3/cm3/nvic.h>
    #include "usart/usart.h" 
    #include "sdram/sdram.h" 
    #include "console/console.h"
    #include "gfx.h"
    extern void clock_setup(void); 
    extern void lcd_spi_init(void);
    extern void lcd_draw_pixel(int x, int y, uint16_t color);
    extern void lcd_show_frame(void);
    extern uint32_t mtime(void);
    extern void msleep(uint32_t);
}

// Linker fixes for USART logging
extern "C" int _write(int file, char *ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) usart_send_blocking(USART1, ptr[i]);
    return len;
}

// ============================================================
//  Constants
// ============================================================
#define SCREEN_W  320
#define SCREEN_H  240
#define CENTER_X  (SCREEN_W / 2)
#define CENTER_Y  (SCREEN_H / 2)

#define SHAPE_DISPLAY_MS  10000  // 10 seconds

// Serial sync
#define SYNC1 0xAA
#define SYNC2 0x55

// Commands from host
#define CMD_WAKE    0x01
#define CMD_SHAPE   0x02
#define CMD_CLEAR   0x03
#define CMD_TIMEOUT 0x04
#define CMD_IMAGE   0x05

// Image receive buffer (160x120 RGB565 = 38,400 bytes)
#define IMG_MAX_W   160
#define IMG_MAX_H   120
static uint8_t img_buffer[IMG_MAX_W * IMG_MAX_H * 2];
static uint8_t img_w = 0, img_h = 0;

// Shape IDs
#define SHAPE_SQUARE    0x00
#define SHAPE_CIRCLE    0x01
#define SHAPE_TRIANGLE  0x02
#define SHAPE_RECTANGLE 0x03
#define SHAPE_STAR      0x04
#define SHAPE_DIAMOND   0x05
#define SHAPE_PENTAGON  0x06
#define SHAPE_HEXAGON   0x07
#define SHAPE_HEART     0x08
#define SHAPE_ARROW     0x09
#define SHAPE_CROSS     0x0A
#define SHAPE_ELLIPSE   0x0B
#define NUM_SHAPES      12

// States
enum AppState {
    STATE_IDLE,
    STATE_LISTENING,
    STATE_DRAWING,
    STATE_IMAGE_RECEIVING
};

// ============================================================
//  Color palette — rich, vibrant colors
// ============================================================
#define COL_BG          0x0000   // Black
#define COL_IDLE_TEXT   0x07FF   // Cyan
#define COL_LISTEN_TEXT 0x07E0   // Green
#define COL_SHAPE_TEXT  0xFFE0   // Yellow
#define COL_TITLE_BG   0x0000   // Black

// Shape fill colors (vibrant palette)
static const uint16_t shape_colors[NUM_SHAPES] = {
    0xF81F, // Magenta  — Square
    0x07FF, // Cyan     — Circle
    0xFBE0, // Orange   — Triangle
    0x001F, // Blue     — Rectangle
    0xFFE0, // Yellow   — Star
    0xF800, // Red      — Diamond
    0x07E0, // Green    — Pentagon
    0xFD20, // Coral    — Hexagon
    0xF81F, // Magenta  — Heart
    0x07FF, // Cyan     — Arrow
    0xFFFF, // White    — Cross
    0xAFE5, // Teal     — Ellipse
};

// Shape outline colors (slightly brighter)
static const uint16_t shape_outline[NUM_SHAPES] = {
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xF81F, 0xFFFF,
};

static const char* shape_names[NUM_SHAPES] = {
    "SQUARE", "CIRCLE", "TRIANGLE", "RECTANGLE",
    "STAR", "DIAMOND", "PENTAGON", "HEXAGON",
    "HEART", "ARROW", "CROSS", "ELLIPSE"
};

// ============================================================
//  Trig helpers (fixed-point for bare metal, no <math.h> float)
// ============================================================
// sin/cos lookup table for 0..359 degrees, scaled by 1024
// We only need a few points for polygon drawing
static int16_t sin1024(int deg) {
    // Normalize to 0..359
    deg = deg % 360;
    if (deg < 0) deg += 360;
    // Approximate using a small table for key angles
    // sin(0)=0, sin(30)=512, sin(45)=724, sin(60)=887, sin(90)=1024
    // Use symmetry for all 4 quadrants
    int sign = 1;
    if (deg >= 180) { sign = -1; deg -= 180; }
    if (deg > 90) deg = 180 - deg;
    // Linear interpolation between key points
    // 0->0, 30->512, 45->724, 60->887, 90->1024
    static const int16_t table[] = {
        0, 18, 36, 54, 71, 89, 105, 122, 139, 156,       // 0-9
        174, 191, 208, 225, 242, 259, 276, 292, 309, 326, // 10-19
        342, 358, 375, 391, 407, 423, 438, 454, 469, 485, // 20-29
        500, 515, 530, 545, 559, 574, 588, 602, 616, 629, // 30-39
        643, 656, 669, 682, 695, 707, 719, 731, 743, 755, // 40-49
        766, 777, 788, 799, 809, 819, 829, 839, 848, 857, // 50-59
        866, 875, 883, 891, 899, 906, 914, 921, 927, 934, // 60-69
        940, 946, 951, 956, 961, 966, 970, 974, 978, 982, // 70-79
        985, 988, 990, 993, 995, 996, 998, 999, 999, 1000, // 80-89
        1000                                                // 90
    };
    return (int16_t)(sign * (int)table[deg] * 1024 / 1000);
}

static int16_t cos1024(int deg) {
    return sin1024(deg + 90);
}

// ============================================================
//  Shape Drawing Functions
// ============================================================

static void drawShapeSquare(uint16_t fill, uint16_t outline) {
    int sz = 80;
    gfx_fillRect(CENTER_X - sz, CENTER_Y - sz, sz*2, sz*2, fill);
    gfx_drawRect(CENTER_X - sz, CENTER_Y - sz, sz*2, sz*2, outline);
    gfx_drawRect(CENTER_X - sz - 1, CENTER_Y - sz - 1, sz*2 + 2, sz*2 + 2, outline);
}

static void drawShapeCircle(uint16_t fill, uint16_t outline) {
    gfx_fillCircle(CENTER_X, CENTER_Y, 75, fill);
    gfx_drawCircle(CENTER_X, CENTER_Y, 75, outline);
    gfx_drawCircle(CENTER_X, CENTER_Y, 76, outline);
}

static void drawShapeTriangle(uint16_t fill, uint16_t outline) {
    int16_t x0 = CENTER_X, y0 = CENTER_Y - 80;
    int16_t x1 = CENTER_X - 80, y1 = CENTER_Y + 60;
    int16_t x2 = CENTER_X + 80, y2 = CENTER_Y + 60;
    gfx_fillTriangle(x0, y0, x1, y1, x2, y2, fill);
    gfx_drawTriangle(x0, y0, x1, y1, x2, y2, outline);
}

static void drawShapeRectangle(uint16_t fill, uint16_t outline) {
    gfx_fillRect(CENTER_X - 100, CENTER_Y - 50, 200, 100, fill);
    gfx_drawRect(CENTER_X - 100, CENTER_Y - 50, 200, 100, outline);
    gfx_drawRect(CENTER_X - 101, CENTER_Y - 51, 202, 102, outline);
}

static void drawShapeStar(uint16_t fill, uint16_t outline) {
    // 5-pointed star using line drawing
    int16_t ox[10], oy[10];
    int R = 75, r = 35;
    for (int i = 0; i < 5; i++) {
        int ang_out = -90 + i * 72;
        int ang_in  = -90 + i * 72 + 36;
        ox[i*2]   = CENTER_X + (int16_t)((long)R * cos1024(ang_out) / 1024);
        oy[i*2]   = CENTER_Y + (int16_t)((long)R * sin1024(ang_out) / 1024);
        ox[i*2+1] = CENTER_X + (int16_t)((long)r * cos1024(ang_in)  / 1024);
        oy[i*2+1] = CENTER_Y + (int16_t)((long)r * sin1024(ang_in)  / 1024);
    }
    // Fill star using triangles from center
    for (int i = 0; i < 10; i++) {
        int ni = (i + 1) % 10;
        gfx_fillTriangle(CENTER_X, CENTER_Y, ox[i], oy[i], ox[ni], oy[ni], fill);
    }
    // Outline
    for (int i = 0; i < 10; i++) {
        int ni = (i + 1) % 10;
        gfx_drawLine(ox[i], oy[i], ox[ni], oy[ni], outline);
    }
}

static void drawShapeDiamond(uint16_t fill, uint16_t outline) {
    int16_t top_x = CENTER_X, top_y = CENTER_Y - 80;
    int16_t right_x = CENTER_X + 55, right_y = CENTER_Y;
    int16_t bottom_x = CENTER_X, bottom_y = CENTER_Y + 80;
    int16_t left_x = CENTER_X - 55, left_y = CENTER_Y;
    gfx_fillTriangle(top_x, top_y, right_x, right_y, bottom_x, bottom_y, fill);
    gfx_fillTriangle(top_x, top_y, left_x, left_y, bottom_x, bottom_y, fill);
    gfx_drawLine(top_x, top_y, right_x, right_y, outline);
    gfx_drawLine(right_x, right_y, bottom_x, bottom_y, outline);
    gfx_drawLine(bottom_x, bottom_y, left_x, left_y, outline);
    gfx_drawLine(left_x, left_y, top_x, top_y, outline);
}

static void drawPolygon(int sides, int radius, uint16_t fill, uint16_t outline) {
    int16_t px[8], py[8];  // max 8 sides
    if (sides > 8) sides = 8;
    for (int i = 0; i < sides; i++) {
        int ang = -90 + i * (360 / sides);
        px[i] = CENTER_X + (int16_t)((long)radius * cos1024(ang) / 1024);
        py[i] = CENTER_Y + (int16_t)((long)radius * sin1024(ang) / 1024);
    }
    // Fill using fan triangulation from center
    for (int i = 0; i < sides; i++) {
        int ni = (i + 1) % sides;
        gfx_fillTriangle(CENTER_X, CENTER_Y, px[i], py[i], px[ni], py[ni], fill);
    }
    // Outline
    for (int i = 0; i < sides; i++) {
        int ni = (i + 1) % sides;
        gfx_drawLine(px[i], py[i], px[ni], py[ni], outline);
    }
}

static void drawShapePentagon(uint16_t fill, uint16_t outline) {
    drawPolygon(5, 75, fill, outline);
}

static void drawShapeHexagon(uint16_t fill, uint16_t outline) {
    drawPolygon(6, 75, fill, outline);
}

static void drawShapeHeart(uint16_t fill, uint16_t outline) {
    // Heart = two circles on top + triangle on bottom
    int r = 35;
    int offset = 30;
    gfx_fillCircle(CENTER_X - offset, CENTER_Y - 20, r, fill);
    gfx_fillCircle(CENTER_X + offset, CENTER_Y - 20, r, fill);
    gfx_drawCircle(CENTER_X - offset, CENTER_Y - 20, r, outline);
    gfx_drawCircle(CENTER_X + offset, CENTER_Y - 20, r, outline);
    // Bottom triangle
    int16_t lx = CENTER_X - offset - r + 5;
    int16_t rx = CENTER_X + offset + r - 5;
    int16_t ty = CENTER_Y - 10;
    int16_t by = CENTER_Y + 70;
    gfx_fillTriangle(lx, ty, rx, ty, CENTER_X, by, fill);
    gfx_drawLine(lx, ty, CENTER_X, by, outline);
    gfx_drawLine(rx, ty, CENTER_X, by, outline);
}

static void drawShapeArrow(uint16_t fill, uint16_t outline) {
    // Arrowhead (triangle)
    gfx_fillTriangle(CENTER_X, CENTER_Y - 80,
                     CENTER_X - 60, CENTER_Y - 10,
                     CENTER_X + 60, CENTER_Y - 10, fill);
    gfx_drawTriangle(CENTER_X, CENTER_Y - 80,
                     CENTER_X - 60, CENTER_Y - 10,
                     CENTER_X + 60, CENTER_Y - 10, outline);
    // Shaft (rectangle)
    gfx_fillRect(CENTER_X - 20, CENTER_Y - 10, 40, 80, fill);
    gfx_drawRect(CENTER_X - 20, CENTER_Y - 10, 40, 80, outline);
}

static void drawShapeCross(uint16_t fill, uint16_t outline) {
    int arm = 25, length = 75;
    // Vertical bar
    gfx_fillRect(CENTER_X - arm, CENTER_Y - length, arm*2, length*2, fill);
    gfx_drawRect(CENTER_X - arm, CENTER_Y - length, arm*2, length*2, outline);
    // Horizontal bar
    gfx_fillRect(CENTER_X - length, CENTER_Y - arm, length*2, arm*2, fill);
    gfx_drawRect(CENTER_X - length, CENTER_Y - arm, length*2, arm*2, outline);
}

static void drawShapeEllipse(uint16_t fill, uint16_t outline) {
    // Approximation: draw filled scanlines for an ellipse
    int rx = 100, ry = 60;
    for (int y = -ry; y <= ry; y++) {
        // x^2/rx^2 + y^2/ry^2 = 1 => x = rx * sqrt(1 - y^2/ry^2)
        long x_sq = (long)rx * rx * ((long)ry * ry - (long)y * y) / ((long)ry * ry);
        int x = 0;
        // Integer sqrt
        if (x_sq > 0) {
            x = 1;
            while ((long)x * x < x_sq) x++;
            if ((long)x * x > x_sq) x--;
        }
        if (x > 0) {
            gfx_drawFastHLine(CENTER_X - x, CENTER_Y + y, x * 2 + 1, fill);
        }
    }
    // Draw outline points
    for (int deg = 0; deg < 360; deg += 2) {
        int px = CENTER_X + (int)((long)rx * cos1024(deg) / 1024);
        int py = CENTER_Y + (int)((long)ry * sin1024(deg) / 1024);
        gfx_drawPixel(px, py, outline);
    }
}

// Dispatch shape drawing
static void drawShape(uint8_t shape_id) {
    if (shape_id >= NUM_SHAPES) return;
    uint16_t fill = shape_colors[shape_id];
    uint16_t outl = shape_outline[shape_id];

    switch (shape_id) {
        case SHAPE_SQUARE:    drawShapeSquare(fill, outl); break;
        case SHAPE_CIRCLE:    drawShapeCircle(fill, outl); break;
        case SHAPE_TRIANGLE:  drawShapeTriangle(fill, outl); break;
        case SHAPE_RECTANGLE: drawShapeRectangle(fill, outl); break;
        case SHAPE_STAR:      drawShapeStar(fill, outl); break;
        case SHAPE_DIAMOND:   drawShapeDiamond(fill, outl); break;
        case SHAPE_PENTAGON:  drawShapePentagon(fill, outl); break;
        case SHAPE_HEXAGON:   drawShapeHexagon(fill, outl); break;
        case SHAPE_HEART:     drawShapeHeart(fill, outl); break;
        case SHAPE_ARROW:     drawShapeArrow(fill, outl); break;
        case SHAPE_CROSS:     drawShapeCross(fill, outl); break;
        case SHAPE_ELLIPSE:   drawShapeEllipse(fill, outl); break;
    }
}

// ============================================================
//  Animations
// ============================================================

// Particle for idle animation
struct Particle {
    int16_t x, y;
    int16_t vx, vy;
    uint16_t color;
    int16_t radius;
};

#define NUM_PARTICLES 6
static Particle particles[NUM_PARTICLES];

static uint16_t particle_palette[] = {
    0xF81F, 0x07FF, 0xFFE0, 0x07E0, 0xFD20, 0x001F
};

static void initParticles() {
    for (int i = 0; i < NUM_PARTICLES; i++) {
        particles[i].x = 40 + (i * 50) % 260;
        particles[i].y = 40 + (i * 37) % 160;
        particles[i].vx = (i % 2 == 0) ? 2 : -2;
        particles[i].vy = (i % 3 == 0) ? 1 : -1;
        if (i % 2 == 1) particles[i].vy = 2;
        particles[i].color = particle_palette[i % 6];
        particles[i].radius = 8 + (i % 4) * 3;
    }
}

static void updateParticles() {
    for (int i = 0; i < NUM_PARTICLES; i++) {
        particles[i].x += particles[i].vx;
        particles[i].y += particles[i].vy;
        // Bounce off walls
        if (particles[i].x <= particles[i].radius || particles[i].x >= SCREEN_W - particles[i].radius) {
            particles[i].vx = -particles[i].vx;
            particles[i].x += particles[i].vx;
        }
        if (particles[i].y <= 30 + particles[i].radius || particles[i].y >= SCREEN_H - 30 - particles[i].radius) {
            particles[i].vy = -particles[i].vy;
            particles[i].y += particles[i].vy;
        }
    }
}

static void renderIdleFrame(uint32_t frame) {
    gfx_fillScreen(COL_BG);

    // Draw bouncing orbs
    for (int i = 0; i < NUM_PARTICLES; i++) {
        gfx_fillCircle(particles[i].x, particles[i].y, particles[i].radius, particles[i].color);
        // Highlight dot for "glass" effect
        gfx_fillCircle(particles[i].x - particles[i].radius/3,
                        particles[i].y - particles[i].radius/3,
                        particles[i].radius/4 + 1, 0xFFFF);
    }

    // Title bar
    gfx_fillRect(0, 0, SCREEN_W, 26, 0x10A2);  // Dark grey
    gfx_setCursor(20, 6);
    gfx_setTextSize(2);
    gfx_setTextColor(COL_IDLE_TEXT, 0x10A2);
    gfx_puts((char*)"Hey Kitty");

    // Blinking prompt at the bottom
    gfx_fillRect(0, SCREEN_H - 24, SCREEN_W, 24, 0x10A2);
    gfx_setTextSize(1);
    if ((frame / 30) % 2 == 0) {
        gfx_setCursor(60, SCREEN_H - 18);
        gfx_setTextColor(0xFFFF, 0x10A2);
        gfx_puts((char*)"Say  \"Hey Kitty\"  to start...");
    } else {
        gfx_setCursor(90, SCREEN_H - 18);
        gfx_setTextColor(0x7BEF, 0x10A2);
        gfx_puts((char*)"Waiting for wake word...");
    }

    lcd_show_frame();
}

static void renderListeningFrame(uint32_t frame) {
    gfx_fillScreen(COL_BG);

    // Pulsing concentric rings
    int pulse = (int)(frame % 40);
    for (int ring = 0; ring < 4; ring++) {
        int r = 20 + ring * 22 + pulse;
        if (r > 0 && r < 120) {
            uint16_t color;
            if (ring == 0) color = 0x07E0;       // Green
            else if (ring == 1) color = 0x03E0;   // Darker green
            else if (ring == 2) color = 0x01E0;   // Even darker
            else color = 0x00E0;                   // Dimmest
            gfx_drawCircle(CENTER_X, CENTER_Y, r, color);
            gfx_drawCircle(CENTER_X, CENTER_Y, r + 1, color);
        }
    }

    // Center mic icon (simple circle)
    gfx_fillCircle(CENTER_X, CENTER_Y, 18, 0x07E0);
    gfx_fillCircle(CENTER_X, CENTER_Y, 12, COL_BG);
    gfx_fillCircle(CENTER_X, CENTER_Y, 6, 0x07E0);

    // Title
    gfx_fillRect(0, 0, SCREEN_W, 26, 0x0320); // Dark green bar
    gfx_setCursor(50, 6);
    gfx_setTextSize(2);
    gfx_setTextColor(0xFFFF, 0x0320);
    gfx_puts((char*)"Listening...");

    // Bottom prompt
    gfx_fillRect(0, SCREEN_H - 24, SCREEN_W, 24, 0x0320);
    gfx_setCursor(30, SCREEN_H - 18);
    gfx_setTextSize(1);
    gfx_setTextColor(0xFFFF, 0x0320);
    gfx_puts((char*)"Say a shape: square, circle...");

    lcd_show_frame();
}

static void renderDrawingScreen(uint8_t shape_id, uint32_t time_left_ms) {
    gfx_fillScreen(COL_BG);

    // Draw the shape
    drawShape(shape_id);

    // Title bar showing shape name
    gfx_fillRect(0, 0, SCREEN_W, 26, 0x4208);  // Medium dark grey
    gfx_setCursor(10, 6);
    gfx_setTextSize(2);
    gfx_setTextColor(COL_SHAPE_TEXT, 0x4208);
    if (shape_id < NUM_SHAPES) {
        gfx_puts((char*)shape_names[shape_id]);
    }

    // Timer bar at bottom
    gfx_fillRect(0, SCREEN_H - 10, SCREEN_W, 10, 0x4208);
    int bar_width = (int)((uint32_t)SCREEN_W * time_left_ms / SHAPE_DISPLAY_MS);
    if (bar_width > SCREEN_W) bar_width = SCREEN_W;
    if (bar_width > 0) {
        gfx_fillRect(0, SCREEN_H - 10, bar_width, 10, 0x07E0);
    }

    lcd_show_frame();
}

// ============================================================
//  Non-blocking serial check (reads from console.c ISR ring buffer)
// ============================================================

// Access console.c's ring buffer (filled by usart1_isr interrupt)
extern char recv_buf[];
extern volatile int recv_ndx_nxt;
extern volatile int recv_ndx_cur;
#define CONSOLE_RECV_BUF_SIZE 4096

static inline bool console_rx_available() {
    return recv_ndx_cur != recv_ndx_nxt;
}

static inline uint8_t console_rx_read() {
    uint8_t b = (uint8_t)recv_buf[recv_ndx_cur];
    recv_ndx_cur = (recv_ndx_cur + 1) % CONSOLE_RECV_BUF_SIZE;
    return b;
}

enum RxState { RX_IDLE_ST, RX_GOT_SYNC1, RX_GOT_SYNC2, RX_GOT_CMD, RX_GOT_CMD2 };
static RxState rx_state = RX_IDLE_ST;
static uint8_t rx_cmd = 0;

static bool pollCommand(uint8_t &cmd_out, uint8_t &arg_out, uint8_t &arg2_out) {
    // Read from the interrupt-driven ring buffer (never misses bytes)
    while (console_rx_available()) {
        uint8_t b = console_rx_read();
        switch (rx_state) {
            case RX_IDLE_ST:
                if (b == SYNC1) rx_state = RX_GOT_SYNC1;
                break;
            case RX_GOT_SYNC1:
                if (b == SYNC2) rx_state = RX_GOT_SYNC2;
                else if (b == SYNC1) rx_state = RX_GOT_SYNC1;
                else rx_state = RX_IDLE_ST;
                break;
            case RX_GOT_SYNC2:
                rx_cmd = b;
                if (b == CMD_SHAPE) {
                    rx_state = RX_GOT_CMD;  // Need one more byte (shape_id)
                } else if (b == CMD_IMAGE) {
                    rx_state = RX_GOT_CMD;  // Need two more bytes (W, H)
                } else {
                    cmd_out = b;
                    arg_out = 0;
                    arg2_out = 0;
                    rx_state = RX_IDLE_ST;
                    return true;
                }
                break;
            case RX_GOT_CMD:
                if (rx_cmd == CMD_IMAGE) {
                    // First arg byte = width, need one more (height)
                    arg_out = b;
                    rx_state = RX_GOT_CMD2;
                } else {
                    cmd_out = rx_cmd;
                    arg_out = b;
                    arg2_out = 0;
                    rx_state = RX_IDLE_ST;
                    return true;
                }
                break;
            case RX_GOT_CMD2:
                cmd_out = rx_cmd;
                arg2_out = b;  // arg_out already has width
                rx_state = RX_IDLE_ST;
                return true;
        }
    }
    return false;
}

// Send ACK back to host
static void sendAck(uint8_t code, uint8_t arg = 0) {
    usart_send_blocking(USART1, 0xBB);
    usart_send_blocking(USART1, 0x66);
    usart_send_blocking(USART1, code);
    if (code == 0x02) {
        usart_send_blocking(USART1, arg);
    }
}

// ============================================================
//  Image Display
// ============================================================
static bool display_is_image = false;  // true = showing image, false = showing shape

static void renderImageScreen(uint32_t time_left_ms) {
    gfx_fillScreen(COL_BG);
    
    // Scale image to fill screen (nearest neighbor 2x)
    int scale_x = SCREEN_W / img_w;
    int scale_y = SCREEN_H / img_h;
    if (scale_x < 1) scale_x = 1;
    if (scale_y < 1) scale_y = 1;
    int scale = (scale_x < scale_y) ? scale_x : scale_y;
    int off_x = (SCREEN_W - img_w * scale) / 2;
    int off_y = (SCREEN_H - img_h * scale) / 2;

    for (int y = 0; y < img_h; y++) {
        for (int x = 0; x < img_w; x++) {
            int idx = (y * img_w + x) * 2;
            uint16_t raw_color = ((uint16_t)img_buffer[idx] << 8) | img_buffer[idx + 1];
            // Cortex-M4 is Little-Endian, ILI9341 SPI expects Big-Endian pixels. Swap bytes!
            uint16_t pixel = (raw_color >> 8) | (raw_color << 8);
            // Draw scaled block
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = off_x + x * scale + sx;
                    int py = off_y + y * scale + sy;
                    if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H) {
                        gfx_drawPixel(px, py, pixel);
                    }
                }
            }
        }
    }

    // Draw timer bar at the bottom
    gfx_fillRect(0, SCREEN_H - 10, SCREEN_W, 10, 0x4208);
    int bar_width = (int)((uint32_t)SCREEN_W * time_left_ms / SHAPE_DISPLAY_MS);
    if (bar_width > SCREEN_W) bar_width = SCREEN_W;
    if (bar_width > 0) {
        gfx_fillRect(0, SCREEN_H - 10, bar_width, 10, 0x07E0);
    }
    
    lcd_show_frame();
}

static void renderReceivingScreen(uint32_t received, uint32_t total) {
    gfx_fillScreen(COL_BG);

    // Title
    gfx_fillRect(0, 0, SCREEN_W, 26, 0x4208);
    gfx_setCursor(30, 6);
    gfx_setTextSize(2);
    gfx_setTextColor(0x07FF, 0x4208);
    gfx_puts((char*)"Receiving...");

    // Progress bar
    int bar_y = CENTER_Y - 10;
    gfx_drawRect(20, bar_y, SCREEN_W - 40, 20, 0x07FF);
    int fill_w = 0;
    if (total > 0) {
        fill_w = (int)((uint32_t)(SCREEN_W - 44) * received / total);
    }
    if (fill_w > 0) {
        gfx_fillRect(22, bar_y + 2, fill_w, 16, 0x07E0);
    }

    // Percentage text
    int pct = 0;
    if (total > 0) pct = (int)((uint32_t)100 * received / total);
    char pct_str[8];
    pct_str[0] = '0' + (pct / 10) % 10;
    pct_str[1] = '0' + pct % 10;
    pct_str[2] = '%';
    pct_str[3] = '\0';
    gfx_setCursor(CENTER_X - 15, bar_y + 30);
    gfx_setTextSize(2);
    gfx_setTextColor(0xFFFF, COL_BG);
    gfx_puts(pct_str);

    lcd_show_frame();
}

// ============================================================
//  Main
// ============================================================
int main(void) {
    clock_setup(); 
    usart_clock_setup();
    usart_setup();
    gpio_setup();

    // Enable USART1 RX interrupt via console.c — captures every
    // byte into recv_buf[] even while LCD is rendering
    nvic_enable_irq(NVIC_USART1_IRQ);
    usart_enable_rx_interrupt(USART1);

    gpio_set(GPIOG, GPIO13);
    for (int i = 0; i < 2000000; i++) __asm__("nop");
    gpio_clear(GPIOG, GPIO13);

    sdram_init(); 
    lcd_spi_init(); 
    gfx_init(lcd_draw_pixel, SCREEN_W, SCREEN_H);

    usart_send_string("HEYKITTY_READY\r\n");

    // Init screen — green flash
    gfx_fillScreen(0x07E0);
    gfx_setCursor(40, 100);
    gfx_setTextSize(2);
    gfx_setTextColor(0x0000, 0x07E0);
    gfx_puts((char*)"Hey Kitty Ready!");
    lcd_show_frame();
    msleep(800);

    initParticles();

    AppState state = STATE_IDLE;
    uint32_t frame = 0;
    uint32_t drawing_start_time = 0;
    uint8_t current_shape = 0;
    uint32_t img_bytes_received = 0;
    uint32_t img_total_bytes = 0;
    uint32_t img_recv_start = 0;
    uint32_t last_progress_update = 0;

    uint32_t last_frame_time = mtime();

    while (1) {
        uint8_t cmd = 0, arg = 0, arg2 = 0;
        bool got_cmd = false;
        
        // Only poll for commands if we are NOT blindly receiving image data
        if (state != STATE_IMAGE_RECEIVING) {
            got_cmd = pollCommand(cmd, arg, arg2);
        }

        switch (state) {
            case STATE_IDLE: {
                // Check for wake command
                if (got_cmd && cmd == CMD_WAKE) {
                    state = STATE_LISTENING;
                    frame = 0;
                    sendAck(0x01);
                    usart_send_string("STATE: LISTENING\r\n");
                    gpio_set(GPIOG, GPIO13);
                } else if (got_cmd && cmd == CMD_SHAPE) {
                    state = STATE_DRAWING;
                    display_is_image = false;
                    current_shape = arg;
                    drawing_start_time = mtime();
                    sendAck(0x02, arg);
                } else if (got_cmd && cmd == CMD_IMAGE) {
                    // Image command: arg=width, arg2=height
                    img_w = arg;
                    img_h = arg2;
                    img_bytes_received = 0;
                    img_total_bytes = (uint32_t)img_w * img_h * 2;
                    img_recv_start = mtime();
                    last_progress_update = 0;
                    state = STATE_IMAGE_RECEIVING;
                    sendAck(0x05);
                    usart_send_string("STATE: IMAGE_RECEIVING\r\n");
                    gpio_set(GPIOG, GPIO13);
                }

                // Render idle animation at ~30 FPS
                uint32_t now = mtime();
                if (now - last_frame_time >= 33) {
                    last_frame_time = now;
                    updateParticles();
                    renderIdleFrame(frame);
                    frame++;
                }
                break;
            }

            case STATE_LISTENING: {
                if (got_cmd) {
                    if (cmd == CMD_SHAPE) {
                        state = STATE_DRAWING;
                        display_is_image = false;
                        current_shape = arg;
                        drawing_start_time = mtime();
                        sendAck(0x02, arg);
                        usart_send_string("STATE: DRAWING\r\n");
                        gpio_clear(GPIOG, GPIO13);
                    } else if (cmd == CMD_IMAGE) {
                        // Image command: arg=width, arg2=height
                        img_w = arg;
                        img_h = arg2;
                        img_bytes_received = 0;
                        img_total_bytes = (uint32_t)img_w * img_h * 2;
                        img_recv_start = mtime();
                        last_progress_update = 0;
                        state = STATE_IMAGE_RECEIVING;
                        sendAck(0x05);
                        usart_send_string("STATE: IMAGE_RECEIVING\r\n");
                    } else if (cmd == CMD_TIMEOUT || cmd == CMD_CLEAR) {
                        state = STATE_IDLE;
                        frame = 0;
                        sendAck(0x03);
                        usart_send_string("STATE: IDLE (timeout)\r\n");
                        gpio_clear(GPIOG, GPIO13);
                    }
                }

                // Render listening animation at ~30 FPS
                uint32_t now = mtime();
                if (now - last_frame_time >= 33) {
                    last_frame_time = now;
                    renderListeningFrame(frame);
                    frame++;
                }
                break;
            }

            case STATE_IMAGE_RECEIVING: {
                // Tight loop: read bytes from ring buffer into img_buffer
                while (console_rx_available() && img_bytes_received < img_total_bytes) {
                    img_buffer[img_bytes_received++] = console_rx_read();
                }

                // Update progress display every 200ms
                uint32_t now = mtime();
                if (now - last_progress_update >= 200) {
                    last_progress_update = now;
                    renderReceivingScreen(img_bytes_received, img_total_bytes);
                }

                // Check for completion
                if (img_bytes_received >= img_total_bytes) {
                    usart_send_string("IMAGE: COMPLETE\r\n");
                    display_is_image = true;
                    drawing_start_time = mtime();
                    state = STATE_DRAWING;
                    sendAck(0x06);  // Image display ACK
                    gpio_clear(GPIOG, GPIO13);
                }

                // Timeout after 30 seconds
                if (now - img_recv_start > 30000) {
                    usart_send_string("IMAGE: TIMEOUT\r\n");
                    state = STATE_IDLE;
                    frame = 0;
                    sendAck(0x03);
                    gpio_clear(GPIOG, GPIO13);
                }
                break;
            }

            case STATE_DRAWING: {
                uint32_t elapsed = mtime() - drawing_start_time;
                uint32_t time_left = 0;
                if (elapsed < SHAPE_DISPLAY_MS) {
                    time_left = SHAPE_DISPLAY_MS - elapsed;
                }

                // Check for new commands even while drawing
                if (got_cmd) {
                    if (cmd == CMD_SHAPE) {
                        display_is_image = false;
                        current_shape = arg;
                        drawing_start_time = mtime();
                        sendAck(0x02, arg);
                    } else if (cmd == CMD_IMAGE) {
                        img_w = arg;
                        img_h = arg2;
                        img_bytes_received = 0;
                        img_total_bytes = (uint32_t)img_w * img_h * 2;
                        img_recv_start = mtime();
                        last_progress_update = 0;
                        state = STATE_IMAGE_RECEIVING;
                        sendAck(0x05);
                        break;
                    } else if (cmd == CMD_CLEAR) {
                        state = STATE_IDLE;
                        frame = 0;
                        sendAck(0x03);
                        gpio_clear(GPIOG, GPIO13);
                        break;
                    } else if (cmd == CMD_WAKE) {
                        state = STATE_LISTENING;
                        frame = 0;
                        sendAck(0x01);
                        gpio_set(GPIOG, GPIO13);
                        break;
                    }
                }

                // Auto-return to idle after timeout
                if (time_left == 0) {
                    state = STATE_IDLE;
                    frame = 0;
                    sendAck(0x03);
                    usart_send_string("STATE: IDLE (shape timeout)\r\n");
                    gpio_clear(GPIOG, GPIO13);
                    break;
                }

                // Render at ~15 FPS
                uint32_t now = mtime();
                if (now - last_frame_time >= 66) {
                    last_frame_time = now;
                    if (display_is_image) {
                        renderImageScreen(time_left);
                    } else {
                        renderDrawingScreen(current_shape, time_left);
                    }
                }
                break;
            }
        }
    }
}

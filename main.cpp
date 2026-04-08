extern "C" {
    #include <stdint.h>
    #include <stdio.h>
    #include <libopencm3/stm32/rcc.h>
    #include <libopencm3/stm32/gpio.h>
    #include "usart/usart.h" 
    #include "sdram/sdram.h" 
    #include "gfx.h"
    extern void clock_setup(void); 
    extern void lcd_spi_init(void);
    extern void lcd_draw_pixel(int x, int y, uint16_t color);
    extern void lcd_show_frame(void); // This pushes SDRAM to the Screen
}

// Linker fixes for USART logging
extern "C" int _write(int file, char *ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) usart_send_blocking(USART1, ptr[i]);
    return len;
}


#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "model_data.h" 

#define SDRAM_BASE 0xD0000000
#define IMG_MAX 27648 
uint8_t* image_buffer = (uint8_t*)(SDRAM_BASE + 0x80000); // offset by 512KB for LCD buffer protection


// 🔥 SAFETY FIX: Move Arena to 1MB offset to avoid LCD Buffer collision
constexpr int kTensorArenaSize = 150 * 1024;
uint8_t* tensor_arena = (uint8_t*)(SDRAM_BASE + 0x100000); 

uint16_t gray_to_rgb565(uint8_t gray) {
    uint8_t r = (gray >> 3) & 0x1F;
    uint8_t g = (gray >> 2) & 0x3F;
    uint8_t b = (gray >> 3) & 0x1F;
    return (r << 11) | (g << 5) | b;
}

// ... [Includes and externs remain the same] ...

int main(void) {
    clock_setup(); 
    usart_clock_setup();
    usart_setup();
    
    // --- BREADCRUMB 1: GPIO INIT ---
    gpio_setup();
    gpio_set(GPIOG, GPIO13); // LED ON
    for(int i=0; i<2000000; i++) __asm__("nop");
    gpio_clear(GPIOG, GPIO13); // LED OFF (If you see this, GPIO is OK)

    sdram_init(); 
    lcd_spi_init(); 
    gfx_init(lcd_draw_pixel, GFX_WIDTH, GFX_HEIGHT);
    
    // --- BREADCRUMB 2: AI INIT ---
    const tflite::Model* model = tflite::GetModel(person_detect_tflite); 
    
    tflite::MicroMutableOpResolver<6> resolver;
    resolver.AddAveragePool2D(); resolver.AddConv2D();
    resolver.AddDepthwiseConv2D(); resolver.AddReshape(); resolver.AddSoftmax();

    // --- BREADCRUMB 3: ARENA ALLOCATION ---
    // If the board crashes here, your SDRAM base address is wrong
    tflite::MicroInterpreter interpreter(model, resolver, tensor_arena, kTensorArenaSize);
    
    gpio_set(GPIOG, GPIO13); // LED ON AGAIN
    if (interpreter.AllocateTensors() != kTfLiteOk) {
        usart_send_string("ALLOC_FAIL\r\n");
        gfx_fillScreen(0xF800); // RED screen means allocation failed
        gfx_setCursor(10, 10);
        gfx_setTextColor(0xFFFF, 0xF800);
        gfx_puts((char*)"ALLOC_FAIL");
        while(1); 
    }
    gpio_clear(GPIOG, GPIO13); // LED OFF (If you see this, AI is READY)

    usart_send_string("READY_TO_RECEIVE\r\n");
    
    // Show green screen on init success
    gfx_fillScreen(0x07E0); // GREEN screen means success
    gfx_setCursor(10, 10);
    gfx_setTextColor(0xFFFF, 0x07E0);
    gfx_puts((char*)"AI INIT SUCCESS");
    
    TfLiteTensor* input = interpreter.input(0);
    TfLiteTensor* output = interpreter.output(0); 
    
    // Pre-compute lookup tables for nearest-neighbor scaling 96 -> 320 and 96 -> 240
    // This maps each screen pixel back to its source pixel in the 96x96 image.
    uint8_t src_x_lut[GFX_WIDTH];   // 320 entries
    uint8_t src_y_lut[GFX_HEIGHT];  // 240 entries
    for (int sx = 0; sx < GFX_WIDTH; sx++)
        src_x_lut[sx] = (uint8_t)((sx * 96) / GFX_WIDTH);
    for (int sy = 0; sy < GFX_HEIGHT; sy++)
        src_y_lut[sy] = (uint8_t)((sy * 96) / GFX_HEIGHT);
    
    while (1) {
       gfx_setCursor(10, 30);
        gfx_setTextColor(0xFFFF, 0x0000);
        gfx_puts((char*)"WAITING FOR SERIAL INPUT ");

        // 🔥 FIX 1: The FLUSH block is completely deleted.

        // 🔥 FIX 2: Sliding-window header search. 
        // It safely chews through garbage bytes until it perfectly locks onto 0xAA 0x55.
        uint8_t sync1 = 0;
        uint8_t sync2 = 0;
        while (true) {
            sync1 = sync2;
            sync2 = (uint8_t)usart_read_char();
            if (sync1 == 0xAA && sync2 == 0x55) {
                break; // Header locked!
            }
        }

        // 3. Header confirmed. Read Size (2 bytes)
        uint8_t size_low = usart_read_char();
        uint8_t size_high = usart_read_char();
        uint16_t expected_size = (size_high << 8) | size_low;

        if (expected_size != 27648) {
            continue; // Wrong size, wait for the next frame
        }

        // 4. Read Pixels using DMA
        dma_rx_complete = 0;
        // dma_rx_error = 0; // 🔥 Uncomment if you have this flag declared!
        usart_dma_receive(image_buffer, expected_size);
        
        while(!dma_rx_complete) {
            if (dma_rx_error) break;
            __asm__("nop"); 
        }
        
        if (dma_rx_error) {
            gfx_setCursor(10, 30);
            gfx_setTextColor(0xFFFF, 0xF800);
            gfx_puts((char*)"DMA ERROR         ");
            lcd_show_frame();
            for(int j=0; j<2000000; j++) __asm__("nop"); 
            continue;
        }

        // 5. Populate TFLite Tensor
        gpio_set(GPIOG, GPIO13);
        for (int y = 0; y < 96; y++) {
            for (int x = 0; x < 96; x++) {
                int base_idx = (y * 96 + x) * 3;
                uint8_t r = image_buffer[base_idx];
                uint8_t g = image_buffer[base_idx + 1];
                uint8_t b = image_buffer[base_idx + 2];
                
                if (input->bytes == 27648) { // 3-channel (RGB) model
                    input->data.int8[base_idx]     = (int8_t)((int16_t)r - 128);
                    input->data.int8[base_idx + 1] = (int8_t)((int16_t)g - 128);
                    input->data.int8[base_idx + 2] = (int8_t)((int16_t)b - 128);
                } else { // Fallback for 1-channel (Grayscale) models
                    uint8_t gray = (r * 77 + g * 150 + b * 29) >> 8;
                    input->data.int8[y * 96 + x] = (int8_t)((int16_t)gray - 128);
                }
            }
        }

        // 6. Draw the image FULLSCREEN (320x240) using nearest-neighbor scaling
        // Each screen pixel maps back to its corresponding 96x96 source pixel.
        for (int sy = 0; sy < GFX_HEIGHT; sy++) {
            int src_y = src_y_lut[sy];
            for (int sx = 0; sx < GFX_WIDTH; sx++) {
                int src_x = src_x_lut[sx];
                int base_idx = (src_y * 96 + src_x) * 3;
                uint8_t r = image_buffer[base_idx];
                uint8_t g = image_buffer[base_idx + 1];
                uint8_t b = image_buffer[base_idx + 2];
                
                uint16_t raw_color = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                // Cortex-M4 is Little-Endian, ILI9341 SPI expects Big-Endian pixels. Swap bytes!
                uint16_t color = (raw_color >> 8) | (raw_color << 8);
                
                lcd_draw_pixel(sx, sy, color);
            }
        }

        interpreter.Invoke();

        // 7. Display and Response - overlay text on the fullscreen image
        int8_t no_person_score = output->data.int8[0];
        int8_t person_score = output->data.int8[1];
        bool is_person = person_score > no_person_score;

        char dbg_buf[64];
        sprintf(dbg_buf, "P: %d NP: %d    ", person_score, no_person_score);
        gfx_setCursor(10, 10);
        gfx_setTextColor(0xFFFF, 0x0000);
        gfx_puts(dbg_buf);

        // Draw classification label at the bottom of the fullscreen image
        gfx_setCursor(10, GFX_HEIGHT - 20);
        if (is_person) {
            gfx_setTextColor(0x07E0, 0x0000); // Green
            gfx_puts((char*)"PERSON ");
        } else {
            gfx_setTextColor(0xF800, 0x0000); // Red
            gfx_puts((char*)"NO PERSON ");
        }

        lcd_show_frame(); 

        // Send Result (Only 3 bytes: 0xBB 0x66 Result)
        usart_send_blocking(USART1, 0xBB);
        usart_send_blocking(USART1, 0x66);
        usart_send_blocking(USART1, is_person ? 1 : 0);

        gpio_clear(GPIOG, GPIO13);
    }
}
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
#include "test_image.h" // Contains the dummy_image[9216] test array

#define SDRAM_BASE 0xD0000000
#define IMG_MAX 9216 
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
    resolver.AddAveragePool2D(); 
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D(); 
    resolver.AddReshape(); 
    resolver.AddSoftmax();

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
    lcd_show_frame();
    
    TfLiteTensor* input = interpreter.input(0);
    TfLiteTensor* output = interpreter.output(0); 
    
    while (1) {
        gfx_setCursor(10, 30);
        gfx_setTextColor(0xFFFF, 0x0000);
        gfx_puts((char*)"PROCESSING HARDCODED...     ");
        lcd_show_frame();

        // 1. Load the hardcoded image directly into the buffer (Bypassing USART completely)
        for (uint16_t i = 0; i < 9216; i++) {
            image_buffer[i] = dummy_image[i];
        }
        
        // 2. Process (LED ON)
        gpio_set(GPIOG, GPIO13);

        gfx_setCursor(10, 30);
        gfx_setTextColor(0xFFFF, 0x0000);
        gfx_puts((char*)"PROCESSING...     ");
        lcd_show_frame();
        
        // Copy image_buffer to TFLite input tensor and draw to LCD (scaled by 2)
        int draw_x_offset = (GFX_WIDTH - 192) / 2; // scale by 2 horizontally
        int draw_y_offset = (GFX_HEIGHT - 192) / 2; // scale by 2 vertically

        for (int y = 0; y < 96; y++) {
            for (int x = 0; x < 96; x++) {
                int i = y * 96 + x;
                uint8_t pixel = image_buffer[i];
                input->data.int8[i] = (int8_t)((int16_t)pixel - 128);
                
                // Convert 8-bit grayscale to 16-bit RGB565 correctly
                uint16_t raw_color = ((pixel >> 3) << 11) | ((pixel >> 2) << 5) | (pixel >> 3);
                // Cortex-M4 is Little-Endian, ILI9341 SPI expects Big-Endian pixels. Swap bytes!
                uint16_t color = (raw_color >> 8) | (raw_color << 8);

                // Draw 2x2 scaled block
                lcd_draw_pixel(draw_x_offset + x*2, draw_y_offset + y*2, color);
                lcd_draw_pixel(draw_x_offset + x*2 + 1, draw_y_offset + y*2, color);
                lcd_draw_pixel(draw_x_offset + x*2, draw_y_offset + y*2 + 1, color);
                lcd_draw_pixel(draw_x_offset + x*2 + 1, draw_y_offset + y*2 + 1, color);
            }
        }

        // 3. Run AI Inference
        interpreter.Invoke();

        // 4. Display and Response
        int8_t no_person_score = output->data.int8[0];
        int8_t person_score = output->data.int8[1];
        bool is_person = person_score > no_person_score;

        char dbg_buf[64];
        sprintf(dbg_buf, "P: %d NP: %d    ", person_score, no_person_score);
        gfx_setCursor(10, 50);
        gfx_setTextColor(0xFFFF, 0x0000);
        gfx_puts(dbg_buf);

        // Draw classification label nicely below the scaled image
        gfx_setCursor(draw_x_offset, draw_y_offset + 192 + 15);
        if (is_person) {
            gfx_setTextColor(0x07E0, 0x0000); // Green
            gfx_puts((char*)"PERSON      ");
        } else {
            gfx_setTextColor(0xF800, 0x0000); // Red
            gfx_puts((char*)"NO PERSON   ");
        }

        lcd_show_frame(); 

        // Send Result over USART
        usart_send_blocking(USART1, 0xBB);
        usart_send_blocking(USART1, 0x66);
        usart_send_blocking(USART1, is_person ? 1 : 0);

        gpio_clear(GPIOG, GPIO13);

        // 🛑 HALT: Stop looping so you can read the LCD screen easily.
        // Once this works, you know the AI is perfect.
        while(1);
    }
}
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


#include <cstring>
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "model_speech.h" 

#define SDRAM_BASE 0xD0000000
#define AUDIO_MAX_BYTES 32000
uint8_t* audio_buffer = (uint8_t*)(SDRAM_BASE + 0x80000); // offset by 512KB for LCD buffer protection

// 🔥 SAFETY FIX: Move Arena to 1MB offset to avoid LCD Buffer collision
constexpr int kTensorArenaSize = 150 * 1024;
uint8_t* tensor_arena = (uint8_t*)(SDRAM_BASE + 0x100000); 

uint16_t gray_to_rgb565(uint8_t gray) {
    uint8_t r = (gray >> 3) & 0x1F;
    uint8_t g = (gray >> 2) & 0x3F;
    uint8_t b = (gray >> 3) & 0x1F;
    return (r << 11) | (g << 5) | b;
}

constexpr int kQuestionCount = 4;
constexpr int kEndingCount = 16;

const char* kSpeechQuestions[kQuestionCount] = {
    "Question 1: Is it alive?",
    "Question 2: Is it indoors?",
    "Question 3: Is it small?",
    "Question 4: Do you want it?"
};

const char* kSpeechEndings[kEndingCount] = {
    "Result A: No/No/No/No","Result B: No/No/No/Yes",
    "Result C: No/No/Yes/No","Result D: No/No/Yes/Yes",
    "Result E: No/Yes/No/No","Result F: No/Yes/No/Yes",
    "Result G: No/Yes/Yes/No","Result H: No/Yes/Yes/Yes",
    "Result I: Yes/No/No/No","Result J: Yes/No/No/Yes",
    "Result K: Yes/No/Yes/No","Result L: Yes/No/Yes/Yes",
    "Result M: Yes/Yes/No/No","Result N: Yes/Yes/No/Yes",
    "Result O: Yes/Yes/Yes/No","Result P: Yes/Yes/Yes/Yes"
};

const char* kSpeechLabels[] = {
    "silence","unknown","yes","no","up","down","left","right",
    "on","off","stop","go"
};
constexpr int kSpeechLabelCount = sizeof(kSpeechLabels) / sizeof(kSpeechLabels[0]);

static void drawCenteredText(const char* title, const char* line1, const char* line2) {
    gfx_fillScreen(0x0000);
    gfx_setCursor(10, 10);
    gfx_setTextColor(0xFFFF, 0x0000);
    gfx_puts((char*)title);
    gfx_setCursor(10, 50);
    gfx_puts((char*)line1);
    gfx_setCursor(10, 90);
    gfx_puts((char*)line2);
    lcd_show_frame();
}

static bool fillAudioInput(TfLiteTensor* input, uint8_t* buffer, uint16_t size) {
    if (input->type == kTfLiteInt16) {
        int copy_bytes = size;
        if (copy_bytes > input->bytes) copy_bytes = input->bytes;
        memcpy(input->data.i16, buffer, copy_bytes);
        return true;
    }
    if (input->type == kTfLiteInt8) {
        int sample_count = input->bytes;
        int16_t* src = reinterpret_cast<int16_t*>(buffer);
        for (int i = 0; i < sample_count; ++i) {
            int16_t sample = 0;
            if ((i * 2 + 1) < size) {
                sample = (int16_t)(buffer[i*2] | (buffer[i*2 + 1] << 8));
            }
            input->data.int8[i] = (int8_t)(sample >> 8);
        }
        return true;
    }
    if (input->type == kTfLiteUInt8) {
        int sample_count = input->bytes;
        for (int i = 0; i < sample_count; ++i) {
            uint8_t sample = 128;
            if ((i * 2 + 1) < size) {
                sample = (uint8_t)((buffer[i*2] | (buffer[i*2 + 1] << 8)) >> 8) + 128;
            }
            input->data.uint8[i] = sample;
        }
        return true;
    }
    return false;
}

static int findLabelIndex(const TfLiteTensor* output) {
    int count = 0;
    if (output->type == kTfLiteInt8) count = output->bytes;
    else if (output->type == kTfLiteUInt8) count = output->bytes;
    else count = output->bytes;
    if (count <= 0) return -1;

    int best = -1;
    int best_score = -128;
    if (output->type == kTfLiteInt8) {
        for (int i = 0; i < count; ++i) {
            int8_t score = output->data.int8[i];
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
    } else if (output->type == kTfLiteUInt8) {
        int best_u = -1;
        for (int i = 0; i < count; ++i) {
            uint8_t score = output->data.uint8[i];
            if (best < 0 || score > best_u) {
                best_u = score;
                best = i;
            }
        }
    }
    if (best < 0 || best >= kSpeechLabelCount) return best;
    return best;
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
    const tflite::Model* model = tflite::GetModel(micro_speech_quantized_tflite);
    
    // The speech model may use Conv2D, Relu, Reshape, FullyConnected, and Softmax.
    // If necessary, add more ops later once model debug is complete.
    tflite::MicroMutableOpResolver<8> resolver;
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddRelu();
    resolver.AddReshape();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();
    resolver.AddAdd();
    resolver.AddMul();

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

    int question_index = 0;
    uint8_t branch_path = 0;
    const char* last_label = "waiting";
    const char* status_text = "Say yes or no";

    while (1) {
        gfx_fillScreen(0x0000);
        gfx_setCursor(10, 10);
        gfx_setTextColor(0xFFFF, 0x0000);
        gfx_puts((char*)"AUDIO QUESTION MODE");

        gfx_setCursor(10, 50);
        if (question_index < kQuestionCount) {
            gfx_puts((char*)kSpeechQuestions[question_index]);
        } else {
            gfx_puts((char*)"QUESTIONNAIRE COMPLETE");
        }

        gfx_setCursor(10, 90);
        gfx_puts((char*)"Last label:");
        gfx_setCursor(10, 110);
        gfx_puts((char*)last_label);

        gfx_setCursor(10, 150);
        gfx_puts((char*)status_text);
        lcd_show_frame();

        uint8_t sync1 = 0;
        uint8_t sync2 = 0;
        while (true) {
            sync1 = sync2;
            sync2 = (uint8_t)usart_read_char();
            if (sync1 == 0xAA && sync2 == 0x55) {
                break;
            }
        }

        uint8_t size_low = usart_read_char();
        uint8_t size_high = usart_read_char();
        uint16_t expected_size = (size_high << 8) | size_low;

        if (expected_size == 0 || expected_size > AUDIO_MAX_BYTES) {
            gfx_fillScreen(0xF800);
            gfx_setCursor(10, 10);
            gfx_setTextColor(0xFFFF, 0xF800);
            gfx_puts((char*)"BAD AUDIO SIZE");
            lcd_show_frame();
            continue;
        }

        dma_rx_complete = 0;
        dma_rx_error = false;
        usart_dma_receive(audio_buffer, expected_size);
        while (!dma_rx_complete) {
            if (dma_rx_error) break;
            __asm__("nop");
        }

        if (dma_rx_error) {
            gfx_fillScreen(0xF800);
            gfx_setCursor(10, 10);
            gfx_setTextColor(0xFFFF, 0x0000);
            gfx_puts((char*)"DMA ERROR");
            lcd_show_frame();
            continue;
        }

        if (!fillAudioInput(input, audio_buffer, expected_size)) {
            gfx_fillScreen(0xF800);
            gfx_setCursor(10, 10);
            gfx_setTextColor(0xFFFF, 0x0000);
            gfx_puts((char*)"AUDIO INPUT FAIL");
            lcd_show_frame();
            continue;
        }

        gpio_set(GPIOG, GPIO13);
        if (interpreter.Invoke() != kTfLiteOk) {
            gpio_clear(GPIOG, GPIO13);
            gfx_fillScreen(0xF800);
            gfx_setCursor(10, 10);
            gfx_setTextColor(0xFFFF, 0x0000);
            gfx_puts((char*)"INFERENCE FAIL");
            lcd_show_frame();
            continue;
        }
        gpio_clear(GPIOG, GPIO13);

        int label_idx = findLabelIndex(output);
        if (label_idx >= 0 && label_idx < kSpeechLabelCount) {
            last_label = kSpeechLabels[label_idx];
        } else {
            last_label = "unknown";
            label_idx = -1;
        }

        bool heard_yes = (label_idx == 2);
        bool heard_no = (label_idx == 3);
        uint8_t result_code = 2;

        if (heard_yes) {
            result_code = 1;
        } else if (heard_no) {
            result_code = 0;
        }

        if (question_index < kQuestionCount) {
            if (heard_yes) {
                branch_path = (branch_path << 1) | 1;
                question_index++;
                status_text = "Heard YES";
            } else if (heard_no) {
                branch_path = (branch_path << 1);
                question_index++;
                status_text = "Heard NO";
            } else {
                status_text = "Unknown, please repeat";
            }
        }

        if (question_index >= kQuestionCount) {
            gfx_fillScreen(0x001F);
            gfx_setCursor(10, 10);
            gfx_setTextColor(0xFFFF, 0x001F);
            gfx_puts((char*)"QUESTIONNAIRE COMPLETE");
            gfx_setCursor(10, 50);
            gfx_puts((char*)kSpeechEndings[branch_path]);
            gfx_setCursor(10, 90);
            gfx_puts((char*)"Say anything to replay");
            lcd_show_frame();
            if (heard_yes || heard_no) {
                question_index = 0;
                branch_path = 0;
                status_text = "Restarting...";
            }
        } else {
            gfx_fillScreen(0x0000);
            gfx_setCursor(10, 10);
            gfx_setTextColor(0xFFFF, 0x0000);
            gfx_puts((char*)"AUDIO QUESTION MODE");
            gfx_setCursor(10, 50);
            gfx_puts((char*)kSpeechQuestions[question_index]);
            gfx_setCursor(10, 90);
            gfx_puts((char*)"Last label:");
            gfx_setCursor(10, 110);
            gfx_puts((char*)last_label);
            gfx_setCursor(10, 150);
            gfx_puts((char*)status_text);
            lcd_show_frame();
        }

        usart_send_blocking(USART1, 0xBB);
        usart_send_blocking(USART1, 0x66);
        usart_send_blocking(USART1, result_code);
        if (question_index >= kQuestionCount) {
            usart_send_blocking(USART1, 0xBB);
            usart_send_blocking(USART1, 0x67);
            usart_send_blocking(USART1, branch_path);
        }
    }
}

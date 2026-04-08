#ifndef LCD_SPI_H
#define LCD_SPI_H

#include <stdint.h>

#define LCD_SPI           SPI5
#define LCD_WIDTH         320
#define LCD_HEIGHT        240
#define FRAME_SIZE_BYTES  (LCD_WIDTH * LCD_HEIGHT * 2)

void lcd_draw_pixel(int x, int y, uint16_t color);
void lcd_show_frame(void);
void lcd_spi_init(void);
void lcd_draw_fullscreen_image(const uint16_t *image_data);

#endif /* LCD_SPI_H */

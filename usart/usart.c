#include "usart.h"
#include <libopencm3/stm32/dma.h>
#include <libopencm3/cm3/nvic.h>

volatile bool dma_rx_complete = false;
volatile bool dma_rx_error = false;


void usart_clock_setup(void){
    /* Enable GPIOG clock for LED & USARTs. */
    rcc_periph_clock_enable(RCC_GPIOG);
    rcc_periph_clock_enable(RCC_GPIOA);

    /* Enable clocks for USART1. */
    rcc_periph_clock_enable(RCC_USART1); 

    /* Enable DMA2 clock for USART1 RX */
    rcc_periph_clock_enable(RCC_DMA2);
}

void usart_setup(void){
    usart_set_baudrate(USART1, 115200);
    usart_set_databits(USART1, 8);
    usart_set_stopbits(USART1, USART_STOPBITS_1);

    /* TX + RX mode */
    usart_set_mode(USART1, USART_MODE_TX_RX);

    usart_set_parity(USART1, USART_PARITY_NONE);
    usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);

    usart_enable(USART1);
}

void gpio_setup(void){
    /* Setup GPIO pin GPIO13 on GPIO port G for LED. */
    gpio_mode_setup(GPIOG, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO13);

    /* Setup GPIO pins for USART1 transmit (PA9) AND receive (PA10). */
    gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO9 | GPIO10);

    /* Setup USART1 TX/RX pins as alternate function 7. */
    gpio_set_af(GPIOA, GPIO_AF7, GPIO9 | GPIO10);
}

void usart_send_string(const char *str)
{
    while (*str) {
        usart_send_blocking(USART1, *str++);
    }
}

char usart_read_char(void)
{
    return usart_recv_blocking(USART1);
}

void usart_dma_receive(uint8_t *buffer, uint16_t size)
{
    dma_rx_complete = false;
    dma_rx_error = false;

    dma_disable_stream(DMA2, DMA_STREAM2);

    dma_stream_reset(DMA2, DMA_STREAM2);
    dma_channel_select(DMA2, DMA_STREAM2, DMA_SxCR_CHSEL_4);

    dma_set_peripheral_address(DMA2, DMA_STREAM2, (uint32_t)&USART_DR(USART1));
    dma_set_memory_address(DMA2, DMA_STREAM2, (uint32_t)buffer);
    dma_set_number_of_data(DMA2, DMA_STREAM2, size);

    /* Transfer from peripheral (USART1 RX) to memory (buffer) */
    dma_set_transfer_mode(DMA2, DMA_STREAM2, DMA_SxCR_DIR_PERIPHERAL_TO_MEM);
    
    dma_enable_memory_increment_mode(DMA2, DMA_STREAM2);
    dma_disable_peripheral_increment_mode(DMA2, DMA_STREAM2);

    dma_set_peripheral_size(DMA2, DMA_STREAM2, DMA_SxCR_PSIZE_8BIT);
    dma_set_memory_size(DMA2, DMA_STREAM2, DMA_SxCR_MSIZE_8BIT);

    dma_set_priority(DMA2, DMA_STREAM2, DMA_SxCR_PL_VERY_HIGH);

    usart_enable_rx_dma(USART1);

    /* Setup NVIC before enabling interrupts */
    nvic_enable_irq(NVIC_DMA2_STREAM2_IRQ);

    /* Enable interrupts in DMA */
    dma_enable_transfer_complete_interrupt(DMA2, DMA_STREAM2);
    dma_enable_transfer_error_interrupt(DMA2, DMA_STREAM2);

    /* Clear any pending flags */
    dma_clear_interrupt_flags(DMA2, DMA_STREAM2, DMA_TCIF | DMA_TEIF);

    /* Ignite the transfer */
    dma_enable_stream(DMA2, DMA_STREAM2);
}

void dma2_stream2_isr(void)
{
    if (dma_get_interrupt_flag(DMA2, DMA_STREAM2, DMA_TCIF)) {
        dma_clear_interrupt_flags(DMA2, DMA_STREAM2, DMA_TCIF);
        dma_rx_complete = true;
    }
    if (dma_get_interrupt_flag(DMA2, DMA_STREAM2, DMA_TEIF)) {
        dma_clear_interrupt_flags(DMA2, DMA_STREAM2, DMA_TEIF);
        dma_rx_error = true;
    }
}
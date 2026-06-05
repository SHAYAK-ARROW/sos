#include "terminal.h"
#include <stdint.h>
#include <stddef.h>

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_MEMORY  ((uint16_t*)0xB8000)

/* Scrollback buffer - stores 100 lines */
#define SCROLLBACK  100
static uint16_t scrollbuf[SCROLLBACK][VGA_WIDTH];
static int      sb_lines   = 0;   /* total lines stored */
static int      sb_view    = 0;   /* 0 = normal view, >0 = scrolled back by N lines */

static size_t  terminal_row;
static size_t  terminal_col;
static uint8_t terminal_color;

static inline uint8_t make_color(uint8_t fg, uint8_t bg){ return fg | bg<<4; }
static inline uint16_t make_entry(char c, uint8_t color){ return (uint16_t)c | (uint16_t)color<<8; }

static void update_cursor(void){
    /* only update cursor when not in scrollback view */
    if(sb_view > 0) return;
    uint16_t pos = terminal_row * VGA_WIDTH + terminal_col;
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)0x0F),"Nd"((uint16_t)0x3D4));
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)(pos&0xFF)),"Nd"((uint16_t)0x3D5));
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)0x0E),"Nd"((uint16_t)0x3D4));
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)((pos>>8)&0xFF)),"Nd"((uint16_t)0x3D5));
}

/* Save current top row into scrollback before scrolling */
static void sb_push_row(size_t row){
    if(sb_lines < SCROLLBACK){
        for(int c=0;c<VGA_WIDTH;c++)
            scrollbuf[sb_lines][c] = VGA_MEMORY[row * VGA_WIDTH + c];
        sb_lines++;
    } else {
        /* shift scrollback buffer up */
        for(int r=0;r<SCROLLBACK-1;r++)
            for(int c=0;c<VGA_WIDTH;c++)
                scrollbuf[r][c] = scrollbuf[r+1][c];
        for(int c=0;c<VGA_WIDTH;c++)
            scrollbuf[SCROLLBACK-1][c] = VGA_MEMORY[row * VGA_WIDTH + c];
    }
}

static void do_scroll(void){
    /* save row 0 before it disappears */
    sb_push_row(0);
    uint8_t blank = make_color(VGA_LIGHT_GREY, VGA_BLACK);
    for(size_t r=0;r<VGA_HEIGHT-1;r++)
        for(size_t c=0;c<VGA_WIDTH;c++)
            VGA_MEMORY[r*VGA_WIDTH+c] = VGA_MEMORY[(r+1)*VGA_WIDTH+c];
    for(size_t c=0;c<VGA_WIDTH;c++)
        VGA_MEMORY[(VGA_HEIGHT-1)*VGA_WIDTH+c] = make_entry(' ', blank);
    terminal_row = VGA_HEIGHT-1;
}

/* Render scrollback view - show lines ending at sb_lines-sb_view */
static void sb_render(void){
    int end = sb_lines - sb_view; /* last scrollback line to show */
    int start = end - (VGA_HEIGHT - 1); /* show VGA_HEIGHT-1 lines from scrollback */

    for(int r=0; r<VGA_HEIGHT-1; r++){
        int si = start + r;
        if(si >= 0 && si < sb_lines){
            for(int c=0;c<VGA_WIDTH;c++)
                VGA_MEMORY[r*VGA_WIDTH+c] = scrollbuf[si][c];
        } else {
            /* fill with blank */
            uint8_t blank=make_color(VGA_LIGHT_GREY,VGA_BLACK);
            for(int c=0;c<VGA_WIDTH;c++)
                VGA_MEMORY[r*VGA_WIDTH+c]=make_entry(' ',blank);
        }
    }
    /* status bar at bottom */
    uint8_t bar_color = make_color(VGA_BLACK, VGA_LIGHT_GREY);
    char msg[] = " [SCROLL MODE - Ctrl+Up/Down to scroll, Ctrl+End to exit] ";
    for(int c=0;c<VGA_WIDTH;c++){
        char ch = (c < (int)(sizeof(msg)-1)) ? msg[c] : ' ';
        VGA_MEMORY[(VGA_HEIGHT-1)*VGA_WIDTH+c] = make_entry(ch, bar_color);
    }
}

/* Called from shell when Ctrl+Up */
void terminal_scroll_up(void){
    if(sb_view < sb_lines){
        sb_view++;
        sb_render();
    }
}

/* Called from shell when Ctrl+Down */
void terminal_scroll_down(void){
    if(sb_view > 0){
        sb_view--;
        if(sb_view == 0){
            /* restore normal view */
            terminal_scroll_exit();
        } else {
            sb_render();
        }
    }
}

/* Exit scroll mode */
void terminal_scroll_exit(void){
    sb_view = 0;
    /* nothing to redraw - VGA memory still has current screen */
    update_cursor();
}

int terminal_in_scroll(void){ return sb_view > 0; }

void terminal_init(void){
    terminal_row=0; terminal_col=0;
    terminal_color=make_color(VGA_LIGHT_GREY,VGA_BLACK);
    sb_lines=0; sb_view=0;
    for(size_t r=0;r<VGA_HEIGHT;r++)
        for(size_t c=0;c<VGA_WIDTH;c++)
            VGA_MEMORY[r*VGA_WIDTH+c]=make_entry(' ',terminal_color);
    update_cursor();
}

void terminal_clear(void){
    terminal_row=0; terminal_col=0;
    sb_view=0;
    terminal_color=make_color(VGA_LIGHT_GREY,VGA_BLACK);
    for(size_t r=0;r<VGA_HEIGHT;r++)
        for(size_t c=0;c<VGA_WIDTH;c++)
            VGA_MEMORY[r*VGA_WIDTH+c]=make_entry(' ',terminal_color);
    update_cursor();
}

void terminal_setcolor(uint8_t fg, uint8_t bg){
    terminal_color=make_color(fg,bg);
}

void terminal_putchar(char c){
    if(sb_view>0) sb_view=0; /* auto exit scroll on new output */

    if(c=='\n'){
        terminal_col=0; terminal_row++;
        if(terminal_row>=VGA_HEIGHT) do_scroll();
        update_cursor(); return;
    }
    if(c=='\r'){ terminal_col=0; update_cursor(); return; }
    if(c=='\b'){
        if(terminal_col>0){
            terminal_col--;
            VGA_MEMORY[terminal_row*VGA_WIDTH+terminal_col]=make_entry(' ',terminal_color);
            update_cursor();
        }
        return;
    }
    VGA_MEMORY[terminal_row*VGA_WIDTH+terminal_col]=make_entry(c,terminal_color);
    terminal_col++;
    if(terminal_col>=VGA_WIDTH){
        terminal_col=0; terminal_row++;
        if(terminal_row>=VGA_HEIGHT) do_scroll();
    }
    update_cursor();
}

void terminal_write(const char* str){
    for(size_t i=0;str[i];i++) terminal_putchar(str[i]);
}

void terminal_writeline(const char* str){
    terminal_write(str); terminal_putchar('\n');
}

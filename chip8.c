#define SDL_GFX_IMPLEMENTATION
#include "sh_gfx.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// #define DEBUG 0
#ifdef DEBUG
    #define LOG(...)  printf("[LOG] " __VA_ARGS__)
#else
    #define LOG(...)  ((void)0)
#endif

#define C8_DISPLAY_WIDTH   64
#define C8_DISPLAY_HEIGHT  32
#define C8_SCALE           10
#define C8_MEMORY_SIZE     4096
#define C8_REGISTER_COUNT  16
#define C8_STACK_SIZE      16
#define C8_KEY_COUNT       16
#define C8_FONTSET_SIZE    80
#define C8_OPCODE_HZ       700.0        // instructions per second
#define C8_TIMER_HZ        60.0         // timers per second

typedef struct {
    uint8_t  memory[C8_MEMORY_SIZE];
    uint8_t  V[C8_REGISTER_COUNT];         // registers
    uint16_t I;                            // index register
    uint16_t pc;                           // program counter
    uint16_t stack[C8_STACK_SIZE];
    uint8_t  sp;                           // stack pointer
    uint8_t  delay_timer;
    uint8_t  sound_timer;
    uint8_t  display[C8_DISPLAY_WIDTH * C8_DISPLAY_HEIGHT];
    uint8_t  keypad[C8_KEY_COUNT];
    bool     draw_flag;                    // set when display should update
    uint32_t rng_state;
} Chip8;

static const uint8_t FONTSET[C8_FONTSET_SIZE] = {
    0xF0,0x90,0x90,0x90,0xF0, // 0
    0x20,0x60,0x20,0x20,0x70, // 1
    0xF0,0x10,0xF0,0x80,0xF0, // 2
    0xF0,0x10,0xF0,0x10,0xF0, // 3
    0x90,0x90,0xF0,0x10,0x10, // 4
    0xF0,0x80,0xF0,0x10,0xF0, // 5
    0xF0,0x80,0xF0,0x90,0xF0, // 6
    0xF0,0x10,0x20,0x40,0x40, // 7
    0xF0,0x90,0xF0,0x90,0xF0, // 8
    0xF0,0x90,0xF0,0x10,0xF0, // 9
    0xF0,0x90,0xF0,0x90,0x90, // A
    0xE0,0x90,0xE0,0x90,0xE0, // B
    0xF0,0x80,0x80,0x80,0xF0, // C
    0xE0,0x90,0x90,0x90,0xE0, // D
    0xF0,0x80,0xF0,0x80,0xF0, // E
    0xF0,0x80,0xF0,0x80,0x80  // F
};

/*
* Set the whole memory block of c8 to 0
* pc to 0x200 (this is where the rom file is loaded in memory[])
* load font at memory[0x50]
* set rng_state needed for random number generation
*/
static void chip8_init(Chip8* c8) {
    memset(c8, 0, sizeof(*c8)); // sets the whole memory block to 0
    c8->pc = 0x200;             // program starts at 0x200

    for (int i = 0; i < C8_FONTSET_SIZE; i++) {
        c8->memory[0x50 + i] = FONTSET[i];
    }

    c8->rng_state = (uint32_t)time(NULL);
    LOG("chip8 init: pc=0x%04X\n", c8->pc);
}

/*
* Load the rom file at memory[0x200]
* return if the file size if greater (0x1000 - 0x200 = 3584 bytes)
*/
static bool chip8_load_rom(Chip8* c8, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open ROM: %s\n", path);
        return false; 
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size > C8_MEMORY_SIZE - 0x200) {
        fprintf(stderr, "ROM too big: %ld bytes\n", size);
        fclose(f);
        return false;
    }

    size_t n = fread(&c8->memory[0x200], 1, size, f);
    fclose(f);
    LOG("Loaded ROM '%s' (%zu bytes) at 0x200\n", path, n);
    return true;
}

/*
* fetch the opcode (u16 opcode = memory[pc<<8 | pc+1])
* increase the program counter to point to the next instruction
*/
static void chip8_step(Chip8* c8) {
    uint16_t opcode = (c8->memory[c8->pc] << 8) | c8->memory[c8->pc + 1];
    c8->pc += 2;

    LOG("PC=0x%04X OP=0x%04X\n", c8->pc - 2, opcode);

    uint16_t nnn = opcode & 0x0FFF;
    uint8_t  nn  = opcode & 0x00FF;
    uint8_t  n   = opcode & 0x000F;
    uint8_t  x   = (opcode >> 8) & 0xF;
    uint8_t  y   = (opcode >> 4) & 0xF;

    switch (opcode & 0xF000) {
    case 0x0000:
        if (opcode == 0x00E0) {                       // CLS
            memset(c8->display, 0, sizeof(c8->display));
            c8->draw_flag = true;
            LOG("CLS\n");
        } else if (opcode == 0x00EE) {                // RET
            c8->pc = c8->stack[--c8->sp];
            LOG("RET -> 0x%04X\n", c8->pc);
        } else {
            LOG("Unhandled 0x%04X (machine code)\n", opcode);
        }
        break;

    case 0x1000: c8->pc = nnn; break;                                   // JP nnn
    case 0x2000: c8->stack[c8->sp++] = c8->pc; c8->pc = nnn; break;     // CALL nnn
    case 0x3000: if (c8->V[x] == nn) c8->pc += 2; break;                // SE Vx, nn
    case 0x4000: if (c8->V[x] != nn) c8->pc += 2; break;                // SNE Vx, nn
    case 0x5000: if (c8->V[x] == c8->V[y]) c8->pc += 2; break;          // SE Vx, Vy
    case 0x6000: c8->V[x] = nn; break;                                  // LD Vx, nn
    case 0x7000: c8->V[x] += nn; break;                                 // ADD Vx, nn

    case 0x8000:
        switch (n) {
        case 0x0: c8->V[x] = c8->V[y]; break;                 // LD
        case 0x1: c8->V[x] |= c8->V[y]; break;                // OR
        case 0x2: c8->V[x] &= c8->V[y]; break;                // AND
        case 0x3: c8->V[x] ^= c8->V[y]; break;                // XOR
        case 0x4: {                                           // ADD + carry
            uint16_t s = c8->V[x] + c8->V[y];
            c8->V[0xF] = (s > 255) ? 1 : 0;
            c8->V[x] = s & 0xFF;
        } break;
        case 0x5: c8->V[0xF] = (c8->V[x] >= c8->V[y]); c8->V[x] -= c8->V[y]; break; // SUB
        case 0x6: c8->V[0xF] = c8->V[x] & 1; c8->V[x] >>= 1; break;      // SHR
        case 0x7: c8->V[0xF] = (c8->V[y] >= c8->V[x]); c8->V[x] = c8->V[y] - c8->V[x]; break; // SUBN
        case 0xE: c8->V[0xF] = (c8->V[x] >> 7) & 1; c8->V[x] <<= 1; break; // SHL
        }
        break;

    case 0x9000: if (c8->V[x] != c8->V[y]) c8->pc += 2; break;          // SNE Vx, Vy
    case 0xA000: c8->I = nnn; break;                                    // LD I, nnn
    case 0xB000: c8->pc = nnn + c8->V[0]; break;                        // JP V0 + nnn
    case 0xC000: {                                                      // RND
        c8->rng_state = c8->rng_state * 1664525u + 1013904223u;
        c8->V[x] = (c8->rng_state >> 16) & nn;
    } break;

    case 0xD000: {                                                      // DRW Vx, Vy, n
        uint8_t vx = c8->V[x] % C8_DISPLAY_WIDTH;
        uint8_t vy = c8->V[y] % C8_DISPLAY_HEIGHT;
        c8->V[0xF] = 0;
        for (int row = 0; row < n; row++) {
            uint8_t sprite = c8->memory[c8->I + row];
            for (int col = 0; col < 8; col++) {
                if (sprite & (0x80 >> col)) {
                    int px = (vx + col) % C8_DISPLAY_WIDTH;
                    int py = (vy + row) % C8_DISPLAY_HEIGHT;
                    int idx = py * C8_DISPLAY_WIDTH + px;
                    if (c8->display[idx]) c8->V[0xF] = 1;
                    c8->display[idx] ^= 1;
                }
            }
        }
        c8->draw_flag = true;
        LOG("DRW x=%d y=%d n=%d collision=%d\n", vx, vy, n, c8->V[0xF]);
    } break;

    case 0xE000:
        if (nn == 0x9E) { if (c8->keypad[c8->V[x] & 0xF]) c8->pc += 2; }  // SKP
        else if (nn == 0xA1) { if (!c8->keypad[c8->V[x] & 0xF]) c8->pc += 2; } // SKNP
        break;

    case 0xF000:
        switch (nn) {
        case 0x07: c8->V[x] = c8->delay_timer; break;
        case 0x0A: {                                                      // wait key
            static int waiting = -1;
            if (waiting < 0) { waiting = x; c8->pc -= 2; }                // re-run this op
            else {
                for (int k = 0; k < 16; k++) {
                    if (c8->keypad[k]) { c8->V[waiting] = k; waiting = -1; break; }
                }
                if (waiting >= 0) c8->pc -= 2;                            // still waiting
            }
        } break;
        case 0x15: c8->delay_timer = c8->V[x]; break;
        case 0x18: c8->sound_timer = c8->V[x]; break;
        case 0x1E: c8->I += c8->V[x]; break;
        case 0x29: c8->I = 0x50 + (c8->V[x] & 0xF) * 5; break;           // font
        case 0x33: {                                                      // BCD
            c8->memory[c8->I]     = c8->V[x] / 100;
            c8->memory[c8->I + 1] = (c8->V[x] / 10) % 10;
            c8->memory[c8->I + 2] = c8->V[x] % 10;
        } break;
        case 0x55: for (int i = 0; i <= x; i++) c8->memory[c8->I + i] = c8->V[i]; break;
        case 0x65: for (int i = 0; i <= x; i++) c8->V[i] = c8->memory[c8->I + i]; break;
        default: LOG("Unhandled 0xF0%02X\n", nn); break;
        }
        break;

    default:
        LOG("Unhandled opcode: 0x%04X\n", opcode);
        break;
    }
}

/* timers (call at 60 Hz) */
static void chip8_tick_timers(Chip8* c8) {
    if (c8->delay_timer > 0) c8->delay_timer--;
    if (c8->sound_timer > 0) {
        c8->sound_timer--;
        LOG("Beep!..\n");
    }
}

/* render chip8 display (64*32) -> sh_gfx framebuffer (scaled) */
static void chip8_render(Chip8* c8, sh_gfx* gfx) {
    sh_gfx_clear(gfx, 0xFF000000);    // black background

    const uint32_t ON  = 0xFFFFFFFF;  // white color
    const uint32_t OFF = 0xFF000000;  // black color

    for (int y = 0; y < C8_DISPLAY_HEIGHT; y++) {
        for (int x = 0; x < C8_DISPLAY_WIDTH; x++) {
            uint32_t color = c8->display[y * C8_DISPLAY_WIDTH + x] ? ON : OFF;

            int bx = x * C8_SCALE;
            int by = y * C8_SCALE;

            for (int sy = 0; sy < C8_SCALE; sy++) {
                for (int sx = 0; sx < C8_SCALE; sx++) {
                    sh_gfx_put_pixel(gfx, bx + sx, by + sy, color);
                }
            }
        }
    }

    sh_gfx_render(gfx);
}

/* keypad mapping SDL scancode to CHIP-8 key */
static int sdl_key_to_chip8(int32_t keycode) {
    switch (keycode) {
        case SDLK_1: return 0x1; case SDLK_2: return 0x2;
        case SDLK_3: return 0x3; case SDLK_4: return 0xC;
        case SDLK_q: return 0x4; case SDLK_w: return 0x5;
        case SDLK_e: return 0x6; case SDLK_r: return 0xD;
        case SDLK_a: return 0x7; case SDLK_s: return 0x8;
        case SDLK_d: return 0x9; case SDLK_f: return 0xE;
        case SDLK_z: return 0xA; case SDLK_x: return 0x0;
        case SDLK_c: return 0xB; case SDLK_v: return 0xF;
        default: return -1;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path/to/rom.ch8>\n", argv[0]);
        return 1;
    }

    Chip8 c8;
    chip8_init(&c8);
    if (!chip8_load_rom(&c8, argv[1])) return 1;

    sh_gfx* gfx = sh_gfx_init("c8",
                              C8_DISPLAY_WIDTH  * C8_SCALE,
                              C8_DISPLAY_HEIGHT * C8_SCALE);
    if (!gfx) return 1;

    int quit = false;
    SDL_Event event;

    // timing
    uint32_t last_inst_time = SDL_GetTicks();
    uint32_t last_timer_time = SDL_GetTicks();
    const double inst_interval = 1000.0 / C8_OPCODE_HZ;  // ms
    const double timer_interval = 1000.0 / C8_TIMER_HZ;

    while (!quit) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { quit = true; }
            else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                int k = sdl_key_to_chip8(event.key.keysym.sym);
                if (k >= 0) c8.keypad[k] = (event.type == SDL_KEYDOWN);
                if (event.key.keysym.sym == SDLK_ESCAPE) quit = true;
            }
        }

        uint32_t now = SDL_GetTicks();

        // run instructions at C8_OPCODE_HZ
        double elapsed_inst = now - last_inst_time;
        while (elapsed_inst >= inst_interval) {
            chip8_step(&c8);
            elapsed_inst -= inst_interval;
            last_inst_time += (uint32_t)inst_interval;
            if (elapsed_inst > inst_interval) { // catch-up clamp
                last_inst_time = now;
                break;
            }
        }

        // run timers at 60 Hz
        double elapsed_timer = now - last_timer_time;
        while (elapsed_timer >= timer_interval) {
            chip8_tick_timers(&c8);
            elapsed_timer -= timer_interval;
            last_timer_time += (uint32_t)timer_interval;
        }

        if (c8.draw_flag) {
            chip8_render(&c8, gfx);
            c8.draw_flag = false;
        }

        // small delay to not burn CPU; keep responsive
        SDL_Delay(1);
    }

    sh_gfx_cleanup(gfx);
    return 0;
}

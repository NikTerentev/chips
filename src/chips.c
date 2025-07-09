#define SDL_MAIN_USE_CALLBACKS 1 /* use the callbacks instead of main() */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE 4096
#define ROM_MAX_SIZE 3584
#define ROM_GAME_ADDRESS_START 0x200
#define STEP_RATE_IN_MILLISECONDS 100
#define PIXEL_SIZE 20
#define CHIP8_DISPLAY_WIDTH 64
#define CHIP8_DISPLAY_HEIGHT 32
#define CHIP8_DISPLAY_MATRIX_SIZE (CHIP8_DISPLAY_WIDTH * CHIP8_DISPLAY_HEIGHT)
#define SDL_WINDOW_WIDTH (PIXEL_SIZE * CHIP8_DISPLAY_WIDTH)
#define SDL_WINDOW_HEIGHT (PIXEL_SIZE * CHIP8_DISPLAY_HEIGHT)

/*
 * CHIP-8 instructions are divided into broad categories by the first “nibble”,
 * or “half-byte”, which is the first hexadecimal number.
 */
#define GET_FIRST_NIBBLE(instruction) ((instruction & 0xF000) >> 12)
/*
 * The second nibble. Used to look up one of the 16 registers (VX) from V0
 * through VF.
 */
#define GET_SECOND_NIBBLE(instruction) ((instruction & 0xF00) >> 8)
/*
 * The third nibble. Also used to look up one of the 16 registers (VY) from V0
 * through VF.
 */
#define GET_THIRD_NIBBLE(instruction) ((instruction & 0xF0) >> 4)
/*
 * The fourth nibble. A 4-bit number.
 */
#define GET_FOURTH_NIBBLE(instruction) (instruction & 0xF)
/*
 * The second byte (third and fourth nibbles). An 8-bit immediate number.
 */
#define GET_THIRD_AND_FORTH_NIBBLES(instruction) (instruction & 0xFF)
/*
 * The second, third and fourth nibbles. A 12-bit immediate memory address.
 */
#define GET_SECOND_THIRD_AND_FOURTH_NIBBLES(instruction) (instruction & 0xFFF)

typedef struct {
  // Memory
  uint8_t RAM[RAM_SIZE];
  // Points at the current instruction in memory
  uint16_t PC;
  // Points at locations in memory
  uint16_t I;
  // Stack
  // TODO
  // Decremented at a rate of 60 Hz (60 times per second) until it reaches 0
  uint8_t delay_timer;
  // Sound timer which functions like the delay timer, but which also
  // gives off a beeping sound as long as it’s not 0
  uint8_t sound_timer;
  // 64 x 32 black and white screen cells
  uint64_t display_cells[CHIP8_DISPLAY_MATRIX_SIZE / 64U];
  // General-purpose variable registers numbered 0 through F hexadecimal,
  // ie. 0 through 15 in decimal, called V0 through VF
  uint8_t V[16];
} CHIP8Context;

typedef struct {
  SDL_Window *window;
  SDL_Renderer *renderer;
  CHIP8Context chip8_context;
  bool need_redraw;
  Uint64 last_step;
} AppState;

void read_rom_file(int argc, char *argv[], AppState *appstate) {
  char *rom_file_address = argv[2];
  FILE *fp;

  for (int i = 1; i < argc; i += 1) {
    if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--rom") == 0) &&
        (i + 1) < argc) {
      rom_file_address = argv[++i];
    } else {
      puts("You need to provide all correct program arguments!");
    }
  }

  if ((fp = fopen(rom_file_address, "r")) == NULL) {
    perror(rom_file_address);
  }

  /* Calculate file length */
  fseek(fp, 0, SEEK_END);
  int rom_file_size = ftell(fp);
  rewind(fp);

  if (rom_file_size > ROM_MAX_SIZE) {
    puts("Your rom is too big for CHIP-8!");
    fclose(fp);
  }

  fread(&appstate->chip8_context.RAM[0x200], 1, rom_file_size, fp);
  fclose(fp);
}

/* This function runs once at startup. */
SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
  SDL_SetAppMetadata("Chip-8 Emulator", "0.0.1", "com.example.emulator");

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  AppState *as = (AppState *)SDL_calloc(1, sizeof(AppState));

  if (!as) {
    return SDL_APP_FAILURE;
  }

  *appstate = as;

  as->need_redraw = false;
  as->chip8_context.PC = ROM_GAME_ADDRESS_START;
  SDL_zeroa(as->chip8_context.display_cells);

  if (!SDL_CreateWindowAndRenderer("examples/emulator/chip-8", SDL_WINDOW_WIDTH,
                                   SDL_WINDOW_HEIGHT, 0, &as->window,
                                   &as->renderer)) {
    SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
    return SDL_APP_FAILURE;
  }

  read_rom_file(argc, argv, as);

  as->last_step = SDL_GetTicks();

  return SDL_APP_CONTINUE;
}

/* This function runs when a new event (mouse input, keypresses, etc) occurs. */
SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  if (event->type == SDL_EVENT_QUIT) {
    return SDL_APP_SUCCESS; /* end the program, reporting success to the OS. */
  }
  return SDL_APP_CONTINUE;
}

uint16_t fetch_instruction(AppState *appstate) {
  uint16_t instruction =
      ((appstate->chip8_context.RAM[appstate->chip8_context.PC] << 8) |
       appstate->chip8_context.RAM[appstate->chip8_context.PC + 1]);
  appstate->chip8_context.PC += 0x2;
  return instruction;
}

int get_display_cell(AppState *appstate, short row, short col) {
  int shift = CHIP8_DISPLAY_WIDTH - col - 1;
  return (appstate->chip8_context.display_cells[row] >> shift & 0x1);
}

void decode_instruction(uint16_t instruction, char **message,
                        AppState *appstate) {
  // Get first nibble (to decode) from instruction using `bit mask` and `and`
  uint8_t first_nibble = GET_FIRST_NIBBLE(instruction);
  uint8_t second_nibble = GET_SECOND_NIBBLE(instruction);
  uint8_t third_nibble = GET_THIRD_NIBBLE(instruction);
  uint8_t fourth_nibble = GET_FOURTH_NIBBLE(instruction);
  uint8_t third_and_fourth_nibbles = GET_THIRD_AND_FORTH_NIBBLES(instruction);
  uint16_t second_third_and_fourth_nibbles =
      GET_SECOND_THIRD_AND_FOURTH_NIBBLES(instruction);

  switch (first_nibble) {
  case 0x0:
    switch (third_nibble) {
    case 0xE:
      SDL_memset(appstate->chip8_context.display_cells, 0,
                 sizeof(appstate->chip8_context.display_cells));
      appstate->need_redraw = true;
      snprintf(*message, 256, "Clear the display");
      break;
    default:
      snprintf(*message, 256, "Not an instruction");
    }
    break;
  case 0x1:
    appstate->chip8_context.PC = second_third_and_fourth_nibbles;
    snprintf(*message, 256, "Jump to address %x",
             second_third_and_fourth_nibbles);
    break;
  case 0x6:
    appstate->chip8_context.V[second_nibble] = third_and_fourth_nibbles;
    snprintf(*message, 256, "Set value %x to register V%x",
             third_and_fourth_nibbles, second_nibble);
    break;
  case 0x7:
    appstate->chip8_context.V[second_nibble] += third_and_fourth_nibbles;
    snprintf(*message, 256, "Add value %x to register V%x",
             third_and_fourth_nibbles, second_nibble);
    break;
  case 0xA:
    appstate->chip8_context.I = second_third_and_fourth_nibbles;
    snprintf(*message, 256, "Set address %x to register I",
             second_third_and_fourth_nibbles);
    break;
  case 0xD:
    uint16_t x_coord = appstate->chip8_context.V[second_nibble];
    uint16_t y_coord = appstate->chip8_context.V[third_nibble];
    uint8_t n = fourth_nibble;
    appstate->chip8_context.V[0xF] = 0;

    for (short y = 0; y < n; y++) {
      uint8_t sprite_data = appstate->chip8_context.RAM[appstate->chip8_context.I + y];
      uint64_t *display_row = &appstate->chip8_context.display_cells[y_coord + y];
      for (short x = 0; x < 8; x++) {
        int sprite_bit_value = (sprite_data >> (7 - x)) & 0x1;
        int bit_pos = CHIP8_DISPLAY_WIDTH - 1 - (x_coord + x);
        *display_row ^= ((uint64_t)sprite_bit_value << bit_pos);
      }
    }
    appstate->need_redraw = true;
    snprintf(
        *message, 256,
        "Display %x-byte sprite starting at memory location I at (V%x, V%x)",
        fourth_nibble, second_nibble, third_nibble);
    break;
  default:
    snprintf(*message, 256, "Not an instruction");
    break;
  }
}

static void set_rect_xy_(SDL_FRect *r, short x, short y) {
  r->x = (float)(x * PIXEL_SIZE);
  r->y = (float)(y * PIXEL_SIZE);
}

void draw_screen(AppState *appstate) {
  SDL_FRect r;
  r.w = r.h = PIXEL_SIZE;
  unsigned i;
  unsigned j;
  int display_cell;

  SDL_SetRenderDrawColor(appstate->renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(appstate->renderer);

  for (j = 0; j < CHIP8_DISPLAY_HEIGHT; j++) {
    for (i = 0; i < CHIP8_DISPLAY_WIDTH; i++) {
       display_cell = get_display_cell(appstate, j, i);
       if (display_cell == 0) {
         SDL_SetRenderDrawColor(appstate->renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
       } else {
         SDL_SetRenderDrawColor(appstate->renderer, 255, 255, 255, SDL_ALPHA_OPAQUE);
       }
       set_rect_xy_(&r, i, j);
       SDL_RenderFillRect(appstate->renderer, &r);
    }
  }

  SDL_RenderPresent(appstate->renderer);
}

/* This function runs once per frame, and is the heart of the program. */
SDL_AppResult SDL_AppIterate(void *appstate) {
  AppState *as = (AppState *)appstate;
  const Uint64 now = SDL_GetTicks();

  char *message = malloc(256);

  while ((now - as->last_step) >= STEP_RATE_IN_MILLISECONDS) {
    // do emulator step
    uint16_t cur_instruction = fetch_instruction(as);

    decode_instruction(cur_instruction, &message, as);

    printf("%04x: %s\n", cur_instruction, message);

    if (as->need_redraw) {
      draw_screen(as);
      as->need_redraw = false;
    }

    as->last_step += STEP_RATE_IN_MILLISECONDS;
  }

  free(message);

  return SDL_APP_CONTINUE;
}

/* This function runs once at shutdown. */
void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  if (appstate != NULL) {
    AppState *as = (AppState *)appstate;
    SDL_DestroyRenderer(as->renderer);
    SDL_DestroyWindow(as->window);
    SDL_free(as);
  }
}
